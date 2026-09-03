/*
 * This program source code file is part of Cably Desktop, based on KiCad,
 * a free EDA CAD application.
 *
 * Copyright (C) 2026 Cably
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * CABLY_KEYCHAIN_SECRET_STORE: the desktop session in the OS credential store.
 *
 * macOS: one kSecClassGenericPassword item (service = CABLY_KEYCHAIN_SERVICE,
 * account = "session", data = the session JSON) through Security.framework's SecItem
 * API - no Objective-C, plain CoreFoundation from C++.  Link: -framework Security
 * -framework CoreFoundation (see cably/CMakeLists.txt).
 *
 * Windows (Credential Manager) and Linux (libsecret) are NOT implemented yet: every
 * call fails with LastError() set, so the app falls back to signing in each launch.
 */

#include <cably_bridge.h>

#include <string>
#include <vector>

#if defined( __APPLE__ )

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace
{

struct CF_RELEASER
{
    CFTypeRef ref;
    explicit CF_RELEASER( CFTypeRef aRef ) : ref( aRef ) {}
    ~CF_RELEASER()
    {
        if( ref )
            CFRelease( ref );
    }
};


CFStringRef cfString( const std::string& aText )
{
    return CFStringCreateWithBytes( kCFAllocatorDefault,
                                    reinterpret_cast<const UInt8*>( aText.data() ),
                                    static_cast<CFIndex>( aText.size() ), kCFStringEncodingUTF8,
                                    false );
}


std::string statusText( OSStatus aStatus )
{
    std::string out = "OSStatus " + std::to_string( aStatus );
    CFStringRef msg = SecCopyErrorMessageString( aStatus, nullptr );

    if( msg )
    {
        char buf[256];

        if( CFStringGetCString( msg, buf, sizeof( buf ), kCFStringEncodingUTF8 ) )
            out += std::string( " (" ) + buf + ")";

        CFRelease( msg );
    }

    return out;
}


/// The attributes that identify our single item.
CFMutableDictionaryRef baseQuery( const std::string& aService )
{
    CFMutableDictionaryRef q = CFDictionaryCreateMutable( kCFAllocatorDefault, 0,
                                                          &kCFTypeDictionaryKeyCallBacks,
                                                          &kCFTypeDictionaryValueCallBacks );
    CFStringRef service = cfString( aService );
    CFStringRef account = cfString( "session" );
    CFDictionarySetValue( q, kSecClass, kSecClassGenericPassword );
    CFDictionarySetValue( q, kSecAttrService, service );
    CFDictionarySetValue( q, kSecAttrAccount, account );
    CFRelease( service );
    CFRelease( account );
    return q;
}

} // namespace


CABLY_KEYCHAIN_SECRET_STORE::CABLY_KEYCHAIN_SECRET_STORE( const std::string& aService ) :
        m_service( aService )
{
}


bool CABLY_KEYCHAIN_SECRET_STORE::Load( CABLY_SESSION& aOut )
{
    m_error.clear();
    CFMutableDictionaryRef q = baseQuery( m_service );
    CF_RELEASER            qr( q );
    CFDictionarySetValue( q, kSecReturnData, kCFBooleanTrue );
    CFDictionarySetValue( q, kSecMatchLimit, kSecMatchLimitOne );

    CFTypeRef result = nullptr;
    OSStatus  status = SecItemCopyMatching( q, &result );

    if( status == errSecItemNotFound )
        return false;

    if( status != errSecSuccess || !result )
    {
        m_error = "keychain read failed: " + statusText( status );
        return false;
    }

    CF_RELEASER rr( result );
    CFDataRef   data = static_cast<CFDataRef>( result );
    std::string json( reinterpret_cast<const char*>( CFDataGetBytePtr( data ) ),
                      static_cast<size_t>( CFDataGetLength( data ) ) );

    if( !CablySessionFromJson( json, aOut ) )
    {
        m_error = "keychain item is not a session";
        return false;
    }

    return true;
}


bool CABLY_KEYCHAIN_SECRET_STORE::Save( const CABLY_SESSION& aSession )
{
    m_error.clear();
    std::string json = CablySessionToJson( aSession );
    CFDataRef   data = CFDataCreate( kCFAllocatorDefault,
                                     reinterpret_cast<const UInt8*>( json.data() ),
                                     static_cast<CFIndex>( json.size() ) );
    CF_RELEASER dr( data );

    // Update in place when the item exists (keeps its ACL), else add.
    CFMutableDictionaryRef q = baseQuery( m_service );
    CF_RELEASER            qr( q );
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable( kCFAllocatorDefault, 0,
                                                              &kCFTypeDictionaryKeyCallBacks,
                                                              &kCFTypeDictionaryValueCallBacks );
    CF_RELEASER ar( attrs );
    CFDictionarySetValue( attrs, kSecValueData, data );

    OSStatus status = SecItemUpdate( q, attrs );

    if( status == errSecItemNotFound )
    {
        CFMutableDictionaryRef add = baseQuery( m_service );
        CF_RELEASER            addr( add );
        CFStringRef            label = cfString( "Cably Desktop session" );
        CF_RELEASER            lr( label );
        CFDictionarySetValue( add, kSecValueData, data );
        CFDictionarySetValue( add, kSecAttrLabel, label );
        CFDictionarySetValue( add, kSecAttrAccessible, kSecAttrAccessibleWhenUnlockedThisDeviceOnly );
        status = SecItemAdd( add, nullptr );
    }

    if( status != errSecSuccess )
    {
        m_error = "keychain write failed: " + statusText( status );
        return false;
    }

    return true;
}


bool CABLY_KEYCHAIN_SECRET_STORE::Clear()
{
    m_error.clear();
    CFMutableDictionaryRef q = baseQuery( m_service );
    CF_RELEASER            qr( q );
    OSStatus               status = SecItemDelete( q );

    if( status == errSecSuccess || status == errSecItemNotFound )
        return true;

    m_error = "keychain delete failed: " + statusText( status );
    return false;
}

#else // not __APPLE__

CABLY_KEYCHAIN_SECRET_STORE::CABLY_KEYCHAIN_SECRET_STORE( const std::string& aService ) :
        m_service( aService ),
        m_error( "no secret store on this platform" )
{
}


bool CABLY_KEYCHAIN_SECRET_STORE::Load( CABLY_SESSION& )
{
    m_error = "no secret store on this platform";
    return false;
}


bool CABLY_KEYCHAIN_SECRET_STORE::Save( const CABLY_SESSION& )
{
    m_error = "no secret store on this platform";
    return false;
}


bool CABLY_KEYCHAIN_SECRET_STORE::Clear()
{
    m_error = "no secret store on this platform";
    return false;
}

#endif

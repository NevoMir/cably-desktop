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
 * Linux (and other Unix): libsecret (the freedesktop Secret Service: GNOME Keyring,
 * KWallet's bridge, KeePassXC ...) when a service ANSWERS on the session bus, with
 * schema "dev.cably.desktop" {service, account="session"} in the default collection;
 * otherwise the documented fallback file $XDG_CONFIG_HOME/cably-desktop/session.json
 * (0600, directory 0700, temp-then-rename).  "Answers" is probed, not assumed: a GDBus
 * proxy for org.freedesktop.secrets is created without error on any bus, owner or not,
 * so the probe opens a libsecret session (OpenSession, which also D-Bus-activates a
 * keyring that is installed but not running) under a watchdog (CABLY_SECRET_SERVICE_TIMEOUT_MS,
 * default 5000): no bus, a bus nobody listens on, a bus without a keyring (GitHub's
 * ubuntu runners: "The name org.freedesktop.secrets was not provided by any .service
 * files"), or a bus that never replies all mean "use the file".  A D-Bus error in the
 * middle of a lookup/store/clear means the same, and drops the cached service so the
 * next call probes afresh.  The libsecret half sits behind CABLY_SECRET_SERVICE so
 * cably/tests/unit/test_cably_secret_store.cpp drives the store with a fake keyring;
 * built with -DCABLY_HAVE_LIBSECRET=1 + `pkg-config libsecret-1` (cably/CMakeLists.txt
 * does that for the app and the CLI).
 *
 * Windows (Credential Manager) is NOT implemented yet: every call fails with LastError()
 * set, so the app falls back to signing in each launch.
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
        m_service( aService ),
        m_backend( "keychain" )
{
}


CABLY_KEYCHAIN_SECRET_STORE::CABLY_KEYCHAIN_SECRET_STORE( const std::string& aService,
                                                          CABLY_SECRET_SERVICE*,
                                                          const std::string& ) :
        m_service( aService ),
        m_backend( "keychain" )
{
}


std::string CABLY_KEYCHAIN_SECRET_STORE::DefaultFallbackDir()
{
    return std::string(); // the keychain never falls back to a file on macOS
}


std::string CABLY_KEYCHAIN_SECRET_STORE::FallbackPath( const std::string&, const std::string& )
{
    return std::string();
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


bool CABLY_KEYCHAIN_SECRET_STORE::serviceAnswers()
{
    return false; // no Secret Service seam on macOS: the keychain is the only store
}

#else // not __APPLE__: Linux and other Unix

#include <cably_config.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if CABLY_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

namespace
{

#if CABLY_HAVE_LIBSECRET

const SecretSchema* cablySchema()
{
    static const SecretSchema schema = {
        "dev.cably.desktop",
        SECRET_SCHEMA_NONE,
        {
            { "service", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { "account", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING },
        }
    };
    return &schema;
}


std::string takeError( GError*& aError, const char* aWhat )
{
    std::string out = std::string( aWhat ) + ": " + ( aError && aError->message ? aError->message : "unknown error" );

    if( aError )
        g_error_free( aError );

    aError = nullptr;
    return out;
}


/// How long one probe or call may take before the service counts as unreachable.
unsigned serviceTimeoutMs()
{
    const char* env = std::getenv( "CABLY_SECRET_SERVICE_TIMEOUT_MS" );

    if( env && *env )
    {
        long v = std::strtol( env, nullptr, 10 );

        if( v > 0 && v < 600000 )
            return static_cast<unsigned>( v );
    }

    return 5000;
}


/**
 * Cancels a GCancellable after a deadline from a helper thread, so a synchronous GDBus
 * call (including the connection + authentication handshake, which the proxy default
 * timeout does not cover) can never hang the app on a broken bus.
 */
class SERVICE_WATCHDOG
{
public:
    explicit SERVICE_WATCHDOG( unsigned aTimeoutMs ) :
            m_cancellable( g_cancellable_new() ),
            m_thread( [this, aTimeoutMs]()
                      {
                          std::unique_lock<std::mutex> lock( m_mutex );

                          if( !m_done.wait_for( lock, std::chrono::milliseconds( aTimeoutMs ),
                                                [this]() { return m_finished; } ) )
                          {
                              m_firedTimeout = true;
                              g_cancellable_cancel( m_cancellable );
                          }
                      } )
    {
    }

    ~SERVICE_WATCHDOG()
    {
        {
            std::lock_guard<std::mutex> lock( m_mutex );
            m_finished = true;
        }

        m_done.notify_all();
        m_thread.join();
        g_object_unref( m_cancellable );
    }

    GCancellable* Cancellable() const { return m_cancellable; }
    bool          TimedOut() const { return m_firedTimeout; }

private:
    GCancellable*           m_cancellable;
    std::mutex              m_mutex;
    std::condition_variable m_done;
    bool                    m_finished = false;
    bool                    m_firedTimeout = false;
    std::thread             m_thread;
};


/// libsecret against the session bus.
class LIBSECRET_SERVICE : public CABLY_SECRET_SERVICE
{
public:
    ~LIBSECRET_SERVICE() override { dropService(); }

    bool Available( std::string& aReason ) override
    {
        aReason.clear();

        // With no session bus at all (containers, CI, ssh) GDBus would try to autolaunch
        // one through X11, which blocks under Xvfb; the fallback file is the answer there.
        const char* addr = std::getenv( "DBUS_SESSION_BUS_ADDRESS" );

        if( !addr || !*addr )
        {
            const char* runtime = std::getenv( "XDG_RUNTIME_DIR" );
            struct stat st;

            if( !runtime || !*runtime || ::stat( ( std::string( runtime ) + "/bus" ).c_str(), &st ) != 0 )
                return false;
        }

        if( m_service )
            return true; // probed earlier in this process and no call has failed since

        const unsigned   timeoutMs = serviceTimeoutMs();
        SERVICE_WATCHDOG watchdog( timeoutMs );
        GError*          error = nullptr;

        // Connects to the bus (or fails right away when nobody listens at the address);
        // the proxy itself is created whether or not the name has an owner.
        SecretService* service = secret_service_get_sync( SECRET_SERVICE_NONE, watchdog.Cancellable(), &error );

        if( !service )
        {
            aReason = takeError( error, watchdog.TimedOut() ? "secret service: no answer from the session bus"
                                                            : "secret service" );
            return false;
        }

        if( error )
            g_error_free( error );

        // Every later call on this proxy (the probe's OpenSession, then the lookups and
        // stores libsecret makes through it) is bounded the same way.
        g_dbus_proxy_set_default_timeout( G_DBUS_PROXY( service ), static_cast<gint>( timeoutMs ) );

        // The real question: does anything answer as org.freedesktop.secrets?  OpenSession
        // is the first call libsecret would make anyway; on a bus without a keyring D-Bus
        // answers "The name org.freedesktop.secrets was not provided by any .service files"
        // at once, an installed keyring gets activated, and a hung one trips the watchdog.
        error = nullptr;

        if( !secret_service_ensure_session_sync( service, watchdog.Cancellable(), &error ) )
        {
            aReason = takeError( error, watchdog.TimedOut() ? "secret service: no answer within the timeout"
                                                            : "secret service" );
            g_object_unref( service );
            return false;
        }

        if( error )
            g_error_free( error );

        // Kept: libsecret hands the password calls this cached instance (session already
        // open, timeout set) instead of connecting again; dropped on the first failure.
        m_service = service;
        return true;
    }

    bool Lookup( const std::string& aService, std::string& aJson, std::string& aError ) override
    {
        aError.clear();
        GError* error = nullptr;
        gchar*  secret = secret_password_lookup_sync( cablySchema(), nullptr, &error, "service",
                                                      aService.c_str(), "account", "session", nullptr );

        if( error )
        {
            aError = takeError( error, "secret service lookup" );
            dropService();
            return false;
        }

        if( !secret )
            return false;

        aJson = secret;
        secret_password_free( secret );
        return true;
    }

    bool Store( const std::string& aService, const std::string& aJson, std::string& aError ) override
    {
        aError.clear();
        GError* error = nullptr;
        secret_password_store_sync( cablySchema(), SECRET_COLLECTION_DEFAULT, "Cably Desktop session",
                                    aJson.c_str(), nullptr, &error, "service", aService.c_str(), "account",
                                    "session", nullptr );

        if( error )
        {
            aError = takeError( error, "secret service store" );
            dropService();
            return false;
        }

        return true;
    }

    bool Clear( const std::string& aService, std::string& aError ) override
    {
        aError.clear();
        GError* error = nullptr;
        secret_password_clear_sync( cablySchema(), nullptr, &error, "service", aService.c_str(), "account",
                                    "session", nullptr );

        if( error )
        {
            aError = takeError( error, "secret service clear" );
            dropService();
            return false;
        }

        return true;
    }

private:
    void dropService()
    {
        if( m_service )
        {
            g_object_unref( m_service );
            m_service = nullptr;
        }
    }

    SecretService* m_service = nullptr;
};


CABLY_SECRET_SERVICE* platformSecretService()
{
    static LIBSECRET_SERVICE service;
    return &service;
}

#else

CABLY_SECRET_SERVICE* platformSecretService()
{
    return nullptr; // built without libsecret: the file is the only store
}

#endif // CABLY_HAVE_LIBSECRET


std::string errnoText( const char* aWhat, const std::string& aPath )
{
    return std::string( aWhat ) + " " + aPath + ": " + std::strerror( errno );
}


/// mkdir -p with mode 0700 for every component we create; the leaf is forced to 0700.
bool ensureDir0700( const std::string& aDir, std::string& aError )
{
    std::string path;

    for( size_t i = 0; i <= aDir.size(); ++i )
    {
        if( i < aDir.size() && aDir[i] != '/' )
            continue;

        path = aDir.substr( 0, i );

        if( path.empty() )
            continue;

        struct stat st;

        if( ::stat( path.c_str(), &st ) == 0 )
        {
            if( !S_ISDIR( st.st_mode ) )
            {
                aError = "not a directory: " + path;
                return false;
            }

            continue;
        }

        if( ::mkdir( path.c_str(), 0700 ) != 0 && errno != EEXIST )
        {
            aError = errnoText( "cannot create", path );
            return false;
        }
    }

    if( ::chmod( aDir.c_str(), 0700 ) != 0 )
    {
        aError = errnoText( "cannot chmod 0700", aDir );
        return false;
    }

    return true;
}


bool writeFile0600( const std::string& aPath, const std::string& aText, std::string& aError )
{
    const std::string tmp = aPath + ".tmp";
    int fd = ::open( tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600 );

    if( fd < 0 )
    {
        aError = errnoText( "cannot create", tmp );
        return false;
    }

    // The umask may have widened the mode above; the file holds tokens, so force it.
    if( ::fchmod( fd, 0600 ) != 0 )
    {
        aError = errnoText( "cannot chmod 0600", tmp );
        ::close( fd );
        ::unlink( tmp.c_str() );
        return false;
    }

    size_t done = 0;

    while( done < aText.size() )
    {
        ssize_t n = ::write( fd, aText.data() + done, aText.size() - done );

        if( n < 0 )
        {
            if( errno == EINTR )
                continue;

            aError = errnoText( "cannot write", tmp );
            ::close( fd );
            ::unlink( tmp.c_str() );
            return false;
        }

        done += static_cast<size_t>( n );
    }

    ::fsync( fd );
    ::close( fd );

    if( ::rename( tmp.c_str(), aPath.c_str() ) != 0 )
    {
        aError = errnoText( "cannot rename into place", aPath );
        ::unlink( tmp.c_str() );
        return false;
    }

    return true;
}

} // namespace


std::string CABLY_KEYCHAIN_SECRET_STORE::DefaultFallbackDir()
{
    const char* xdg = std::getenv( "XDG_CONFIG_HOME" );

    // The XDG base directory spec: a relative XDG_CONFIG_HOME is invalid and ignored.
    if( xdg && xdg[0] == '/' )
        return std::string( xdg ) + "/cably-desktop";

    const char* home = std::getenv( "HOME" );
    return std::string( home && *home ? home : "." ) + "/.config/cably-desktop";
}


std::string CABLY_KEYCHAIN_SECRET_STORE::FallbackPath( const std::string& aDir, const std::string& aService )
{
    std::string dir = aDir;

    while( dir.size() > 1 && dir.back() == '/' )
        dir.pop_back();

    if( aService == CABLY_KEYCHAIN_SERVICE )
        return dir + "/session.json";

    return dir + "/session." + aService + ".json";
}


CABLY_KEYCHAIN_SECRET_STORE::CABLY_KEYCHAIN_SECRET_STORE( const std::string& aService ) :
        CABLY_KEYCHAIN_SECRET_STORE( aService, nullptr, std::string() )
{
}


CABLY_KEYCHAIN_SECRET_STORE::CABLY_KEYCHAIN_SECRET_STORE( const std::string& aService,
                                                          CABLY_SECRET_SERVICE* aBackend,
                                                          const std::string& aFallbackDir ) :
        m_service( aService ),
        m_fallbackDir( aFallbackDir.empty() ? DefaultFallbackDir() : aFallbackDir ),
        m_secretService( aBackend ? aBackend : platformSecretService() )
{
}


bool CABLY_KEYCHAIN_SECRET_STORE::Load( CABLY_SESSION& aOut )
{
    m_error.clear();
    m_note.clear();

    if( serviceAnswers() )
    {
        std::string json, err;

        if( m_secretService->Lookup( m_service, json, err ) )
        {
            m_backend = "secret-service";

            if( !CablySessionFromJson( json, aOut ) )
            {
                m_error = "secret service item is not a session";
                return false;
            }

            return true;
        }

        // An error is the service going away under us: the file is the store now.  No
        // error and no item: a session saved while the service was away may be in the file.
        if( !err.empty() )
            m_note = "secret service read failed, using the file: " + err;
    }

    const std::string path = FallbackPath( m_fallbackDir, m_service );
    m_backend = "file:" + path;

    std::ifstream in( path, std::ios::binary );

    if( !in )
        return false; // nothing stored

    std::string json( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );

    if( !CablySessionFromJson( json, aOut ) )
    {
        m_error = "session file is not a session: " + path;
        return false;
    }

    return true;
}


bool CABLY_KEYCHAIN_SECRET_STORE::Save( const CABLY_SESSION& aSession )
{
    m_error.clear();
    m_note.clear();
    const std::string json = CablySessionToJson( aSession );
    const std::string path = FallbackPath( m_fallbackDir, m_service );

    if( serviceAnswers() )
    {
        std::string err;

        if( m_secretService->Store( m_service, json, err ) )
        {
            m_backend = "secret-service";
            ::unlink( path.c_str() ); // migrate: nothing left on disk once the keyring has it
            return true;
        }

        m_note = "secret service write failed, using the file: " + err;
    }

    m_backend = "file:" + path;
    std::string err;

    if( !ensureDir0700( m_fallbackDir, err ) || !writeFile0600( path, json, err ) )
    {
        m_error = "session file write failed: " + err;
        return false;
    }

    return true;
}


bool CABLY_KEYCHAIN_SECRET_STORE::Clear()
{
    m_error.clear();
    m_note.clear();
    const std::string path = FallbackPath( m_fallbackDir, m_service );
    bool              viaService = false;

    if( serviceAnswers() )
    {
        std::string err;

        if( m_secretService->Clear( m_service, err ) )
            viaService = true;
        else
            m_note = "secret service delete failed: " + err;
    }

    m_backend = viaService ? std::string( "secret-service" ) : "file:" + path;

    // Whatever the service said, a file copy (from a spell without the service) goes.
    if( ::unlink( path.c_str() ) != 0 && errno != ENOENT )
    {
        m_error = "session file delete failed: " + errnoText( "cannot remove", path );
        return false;
    }

    return true;
}


bool CABLY_KEYCHAIN_SECRET_STORE::serviceAnswers()
{
    if( !m_secretService )
        return false;

    std::string reason;

    if( m_secretService->Available( reason ) )
        return true;

    if( !reason.empty() )
        m_note = "secret service unavailable, using the file: " + reason;

    return false;
}

#endif

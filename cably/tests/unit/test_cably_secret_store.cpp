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
 * Standalone unit test for the Linux secret store (cably/src/cably_bridge_keychain.cpp,
 * the non-Apple branch of CABLY_KEYCHAIN_SECRET_STORE).  Built by
 * cably/linux/run-tests.sh with the system compiler:
 *
 *   c++ -std=c++17 -DCABLY_HAVE_LIBSECRET=1 $(pkg-config --cflags libsecret-1) \
 *       -I cably/src -I thirdparty/nlohmann_json -I thirdparty/picosha2 \
 *       cably/src/cably_bridge.cpp cably/src/cably_bridge_keychain.cpp \
 *       cably/tests/unit/test_cably_secret_store.cpp $(pkg-config --libs libsecret-1)
 *
 * The contract under test (the header, cably/src/cably_bridge.h, is the spec):
 *  - No Secret Service reachable (a container, CI, ssh): the session lives in
 *    $XDG_CONFIG_HOME/cably-desktop/session.json (~/.config when XDG_CONFIG_HOME is
 *    unset or relative), mode 0600 in a 0700 directory, written temp-then-rename;
 *    Backend() reports "file:<path>".
 *  - A Secret Service reachable: the item goes there (Backend() "secret-service"), a
 *    file copy left from before is removed by the next Save, and a service that is
 *    reachable but FAILS is an error - never a silent fallback to a plain-text file.
 *  - The service half sits behind CABLY_SECRET_SERVICE so this test drives it with a
 *    fake; the real libsecret backend is compiled in and probed once at the end.
 * On macOS the store is Security.framework (cably/tests/bridge.sh (e)); there this test
 * only checks the platform-independent bits.
 */

#include <cably_bridge.h>
#include <cably_config.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

static int g_checks = 0;

#define CHECK( cond )                                                                    \
    do                                                                                   \
    {                                                                                    \
        ++g_checks;                                                                      \
        if( !( cond ) )                                                                  \
        {                                                                                \
            std::fprintf( stderr, "CHECK failed at line %d: %s\n", __LINE__, #cond );    \
            std::exit( 1 );                                                              \
        }                                                                                \
    } while( 0 )


static fs::path scratchDir()
{
    char        tmpl[] = "/tmp/cably-secret-test-XXXXXX";
    const char* d = ::mkdtemp( tmpl );
    CHECK( d != nullptr );
    return fs::path( d );
}


static unsigned modeOf( const std::string& aPath )
{
    struct stat st;

    if( ::stat( aPath.c_str(), &st ) != 0 )
        return 0xFFFF;

    return static_cast<unsigned>( st.st_mode & 07777 );
}


static CABLY_SESSION sampleSession()
{
    CABLY_SESSION s;
    s.accessToken = "access-token-value";
    s.refreshToken = "refresh-token-value";
    s.email = "unit@example.com";
    s.expiresAt = 4102444800LL;
    return s;
}


static bool sameSession( const CABLY_SESSION& a, const CABLY_SESSION& b )
{
    return a.accessToken == b.accessToken && a.refreshToken == b.refreshToken && a.email == b.email
           && a.expiresAt == b.expiresAt;
}


static void testFallbackPathRule()
{
    // The static path rule holds on every platform that has a fallback file.
    const std::string p = CABLY_KEYCHAIN_SECRET_STORE::FallbackPath( "/x/cfg", CABLY_KEYCHAIN_SERVICE );
    const std::string q = CABLY_KEYCHAIN_SECRET_STORE::FallbackPath( "/x/cfg/", "dev.cably.desktop.test" );
#if defined( __APPLE__ )
    CHECK( p.empty() && q.empty() ); // the keychain never falls back to a file on macOS
#else
    CHECK( p == "/x/cfg/session.json" );
    CHECK( q == "/x/cfg/session.dev.cably.desktop.test.json" ); // trailing slash folded
#endif
}


#if !defined( __APPLE__ )

/// In-memory stand-in for the Secret Service with switchable failure modes.
class FAKE_SECRET_SERVICE : public CABLY_SECRET_SERVICE
{
public:
    bool                               available = true;
    bool                               storeFails = false;
    bool                               lookupErrors = false;
    bool                               clearFails = false;
    int                                lookups = 0, stores = 0, clears = 0;
    std::map<std::string, std::string> items;

    bool Available() override { return available; }

    bool Lookup( const std::string& aService, std::string& aSecret, std::string& aError ) override
    {
        ++lookups;
        aError.clear();

        if( lookupErrors )
        {
            aError = "fake lookup error";
            return false;
        }

        auto it = items.find( aService );

        if( it == items.end() )
            return false;

        aSecret = it->second;
        return true;
    }

    bool Store( const std::string& aService, const std::string& aSecret, std::string& aError ) override
    {
        ++stores;
        aError.clear();

        if( storeFails )
        {
            aError = "fake store error";
            return false;
        }

        items[aService] = aSecret;
        return true;
    }

    bool Clear( const std::string& aService, std::string& aError ) override
    {
        ++clears;
        aError.clear();

        if( clearFails )
        {
            aError = "fake clear error";
            return false;
        }

        items.erase( aService );
        return true;
    }
};


static void testFileFallback()
{
    fs::path          root = scratchDir();
    const std::string dir = ( root / "cfg" ).string();
    const std::string file = dir + "/session.json";

    // No service at all (null backend, e.g. built without libsecret): the file is the store.
    CABLY_KEYCHAIN_SECRET_STORE store( CABLY_KEYCHAIN_SERVICE, nullptr, dir );
    CHECK( CABLY_KEYCHAIN_SECRET_STORE::FallbackPath( dir, CABLY_KEYCHAIN_SERVICE ) == file );

    CABLY_SESSION out;
    CHECK( !store.Load( out ) );
    CHECK( store.LastError().empty() ); // nothing stored is not an error
    CHECK( store.Backend() == "file:" + file );
    CHECK( !fs::exists( dir ) ); // Load never creates anything

    CABLY_SESSION in = sampleSession();
    CHECK( store.Save( in ) );
    CHECK( store.LastError().empty() );
    CHECK( store.Backend() == "file:" + file );
    CHECK( fs::is_regular_file( file ) );
    CHECK( modeOf( file ) == 0600 );
    CHECK( modeOf( dir ) == 0700 );
    CHECK( !fs::exists( file + ".tmp" ) ); // temp-then-rename: no temp file left behind

    // The file holds the session JSON, nothing else.
    std::ifstream f( file );
    std::string   text( ( std::istreambuf_iterator<char>( f ) ), std::istreambuf_iterator<char>() );
    CABLY_SESSION parsed;
    CHECK( CablySessionFromJson( text, parsed ) );
    CHECK( sameSession( parsed, in ) );

    CABLY_SESSION loaded;
    CHECK( store.Load( loaded ) );
    CHECK( sameSession( loaded, in ) );
    CHECK( store.Backend() == "file:" + file );

    // Overwrite keeps the modes (the umask never widens them).
    ::umask( 0 );
    in.email = "second@example.com";
    CHECK( store.Save( in ) );
    CHECK( store.Load( loaded ) && loaded.email == "second@example.com" );
    CHECK( modeOf( file ) == 0600 );

    CHECK( store.Clear() );
    CHECK( !fs::exists( file ) );
    CHECK( store.Clear() ); // clearing an empty store is fine
    CHECK( store.LastError().empty() );
    CHECK( !store.Load( loaded ) );
    CHECK( store.LastError().empty() );

    // A directory that already existed too open gets tightened on Save.
    ::chmod( dir.c_str(), 0755 );
    CHECK( store.Save( in ) );
    CHECK( modeOf( dir ) == 0700 );

    // A non-default service name gets its own file next to it.
    const std::string otherFile = dir + "/session.dev.cably.desktop.test.json";
    CABLY_KEYCHAIN_SECRET_STORE other( "dev.cably.desktop.test", nullptr, dir );
    CHECK( other.Save( in ) );
    CHECK( other.Backend() == "file:" + otherFile );
    CHECK( fs::exists( otherFile ) );
    CHECK( modeOf( otherFile ) == 0600 );
    CHECK( store.Load( loaded ) ); // the default one is untouched
    CHECK( other.Clear() && store.Clear() );
    CHECK( !fs::exists( otherFile ) && !fs::exists( file ) );

    fs::remove_all( root );
}


static void testDefaultPaths()
{
    fs::path root = scratchDir();

    ::setenv( "XDG_CONFIG_HOME", ( root / "xdg" ).c_str(), 1 );
    ::setenv( "HOME", ( root / "home" ).c_str(), 1 );
    CHECK( CABLY_KEYCHAIN_SECRET_STORE::DefaultFallbackDir() == ( root / "xdg" ).string() + "/cably-desktop" );

    // A relative XDG_CONFIG_HOME must be ignored (XDG base directory spec).
    ::setenv( "XDG_CONFIG_HOME", "relative/dir", 1 );
    CHECK( CABLY_KEYCHAIN_SECRET_STORE::DefaultFallbackDir() == ( root / "home" ).string() + "/.config/cably-desktop" );

    ::unsetenv( "XDG_CONFIG_HOME" );
    CHECK( CABLY_KEYCHAIN_SECRET_STORE::DefaultFallbackDir() == ( root / "home" ).string() + "/.config/cably-desktop" );

    // An empty aFallbackDir means the default; the store with a null backend writes
    // there, creating ~/.config/cably-desktop (0700) on the way.
    CABLY_KEYCHAIN_SECRET_STORE store( "dev.cably.desktop.test.paths", nullptr, std::string() );
    const std::string           expect = ( root / "home" ).string() + "/.config/cably-desktop";
    CHECK( store.Save( sampleSession() ) );
    CHECK( store.Backend() == "file:" + expect + "/session.dev.cably.desktop.test.paths.json" );
    CHECK( modeOf( expect ) == 0700 );
    CHECK( modeOf( expect + "/session.dev.cably.desktop.test.paths.json" ) == 0600 );
    CHECK( store.Clear() );

    fs::remove_all( root );
}


static void testCorruptFile()
{
    fs::path          root = scratchDir();
    const std::string dir = ( root / "cfg" ).string();
    fs::create_directories( dir );
    {
        std::ofstream f( dir + "/session.json" );
        f << "this is not json";
    }

    CABLY_KEYCHAIN_SECRET_STORE store( CABLY_KEYCHAIN_SERVICE, nullptr, dir );
    CABLY_SESSION               out;
    CHECK( !store.Load( out ) );
    CHECK( !store.LastError().empty() );
    CHECK( store.LastError().find( "session" ) != std::string::npos );

    // Clear removes the junk so the next sign-in starts clean.
    CHECK( store.Clear() );
    CHECK( !fs::exists( dir + "/session.json" ) );
    fs::remove_all( root );
}


static void testServiceBackend()
{
    fs::path            root = scratchDir();
    const std::string   dir = ( root / "cfg" ).string();
    const std::string   file = dir + "/session.json";
    FAKE_SECRET_SERVICE fake;

    CABLY_KEYCHAIN_SECRET_STORE store( CABLY_KEYCHAIN_SERVICE, &fake, dir );

    // A stale file copy from before the service existed is removed by a service Save.
    fs::create_directories( dir );
    {
        std::ofstream f( file );
        f << CablySessionToJson( sampleSession() );
    }

    CABLY_SESSION in = sampleSession();
    in.email = "service@example.com";
    CHECK( store.Save( in ) );
    CHECK( store.LastError().empty() );
    CHECK( store.Backend() == "secret-service" );
    CHECK( fake.stores == 1 );
    CHECK( fake.items.count( CABLY_KEYCHAIN_SERVICE ) == 1 );
    CHECK( !fs::exists( file ) );

    // The item IS the session JSON under the service's name.
    CABLY_SESSION parsed;
    CHECK( CablySessionFromJson( fake.items[CABLY_KEYCHAIN_SERVICE], parsed ) );
    CHECK( sameSession( parsed, in ) );

    CABLY_SESSION loaded;
    CHECK( store.Load( loaded ) );
    CHECK( sameSession( loaded, in ) );
    CHECK( store.Backend() == "secret-service" );
    CHECK( fake.lookups == 1 );
    CHECK( !fs::exists( file ) ); // Load never writes the file

    CHECK( store.Clear() );
    CHECK( fake.clears == 1 );
    CHECK( fake.items.empty() );
    CHECK( !store.Load( loaded ) );
    CHECK( store.LastError().empty() );

    // Separate service names are separate items.
    CABLY_KEYCHAIN_SECRET_STORE other( "dev.cably.desktop.test", &fake, dir );
    CHECK( other.Save( in ) );
    CHECK( fake.items.count( "dev.cably.desktop.test" ) == 1 );
    CHECK( !store.Load( loaded ) );
    CHECK( other.Clear() );
    CHECK( fake.items.empty() );

    fs::remove_all( root );
}


static void testServiceUnavailableIsTheFileCase()
{
    fs::path            root = scratchDir();
    const std::string   dir = ( root / "cfg" ).string();
    const std::string   file = dir + "/session.json";
    FAKE_SECRET_SERVICE fake;
    fake.available = false; // a backend that is compiled in but has no keyring to talk to

    CABLY_KEYCHAIN_SECRET_STORE store( CABLY_KEYCHAIN_SERVICE, &fake, dir );
    CHECK( store.Save( sampleSession() ) );
    CHECK( store.Backend() == "file:" + file );
    CHECK( fake.stores == 0 ); // never touched
    CHECK( modeOf( file ) == 0600 );

    CABLY_SESSION loaded;
    CHECK( store.Load( loaded ) && loaded.email == "unit@example.com" );
    CHECK( fake.lookups == 0 );
    CHECK( store.Clear() );
    CHECK( fake.clears == 0 );
    CHECK( !fs::exists( file ) );
    fs::remove_all( root );
}


static void testServiceMigrationFromFile()
{
    fs::path            root = scratchDir();
    const std::string   dir = ( root / "cfg" ).string();
    const std::string   file = dir + "/session.json";
    FAKE_SECRET_SERVICE fake;

    // The service appeared after a file-only session was saved.
    {
        CABLY_KEYCHAIN_SECRET_STORE before( CABLY_KEYCHAIN_SERVICE, nullptr, dir );
        CHECK( before.Save( sampleSession() ) );
    }

    CABLY_KEYCHAIN_SECRET_STORE store( CABLY_KEYCHAIN_SERVICE, &fake, dir );
    CABLY_SESSION               loaded;
    CHECK( store.Load( loaded ) );
    CHECK( loaded.email == "unit@example.com" );
    CHECK( store.Backend() == "file:" + file ); // found in the file after an empty lookup
    CHECK( store.LastError().empty() );
    CHECK( fake.lookups == 1 );

    // The next Save moves it into the service and drops the file.
    CHECK( store.Save( loaded ) );
    CHECK( store.Backend() == "secret-service" );
    CHECK( !fs::exists( file ) );
    CHECK( fake.items.count( CABLY_KEYCHAIN_SERVICE ) == 1 );

    CHECK( store.Clear() );
    fs::remove_all( root );
}


static void testReachableServiceFailuresAreErrors()
{
    fs::path            root = scratchDir();
    const std::string   dir = ( root / "cfg" ).string();
    const std::string   file = dir + "/session.json";
    FAKE_SECRET_SERVICE fake;

    CABLY_KEYCHAIN_SECRET_STORE store( CABLY_KEYCHAIN_SERVICE, &fake, dir );

    // Store fails: the session is NOT written to a plain-text file behind the user's
    // back; Save reports the failure (the app keeps the in-memory session).
    fake.storeFails = true;
    CHECK( !store.Save( sampleSession() ) );
    CHECK( store.LastError().find( "fake store error" ) != std::string::npos );
    CHECK( store.Backend() == "secret-service" );
    CHECK( !fs::exists( file ) );
    fake.storeFails = false;

    // Lookup errors surface too.
    fake.lookupErrors = true;
    CABLY_SESSION loaded;
    CHECK( !store.Load( loaded ) );
    CHECK( store.LastError().find( "fake lookup error" ) != std::string::npos );
    fake.lookupErrors = false;

    // A service item that is not a session: an error, no session.
    fake.items[CABLY_KEYCHAIN_SERVICE] = "garbage";
    CHECK( !store.Load( loaded ) );
    CHECK( !store.LastError().empty() );
    fake.items.clear();

    // Clear failing in the service is reported; a file copy is still removed.
    {
        CABLY_KEYCHAIN_SECRET_STORE fileOnly( CABLY_KEYCHAIN_SERVICE, nullptr, dir );
        CHECK( fileOnly.Save( sampleSession() ) );
    }
    fake.items[CABLY_KEYCHAIN_SERVICE] = CablySessionToJson( sampleSession() );
    fake.clearFails = true;
    CHECK( !store.Clear() );
    CHECK( store.LastError().find( "fake clear error" ) != std::string::npos );
    CHECK( !fs::exists( file ) );
    fake.clearFails = false;
    CHECK( store.Clear() );
    CHECK( fake.items.empty() );

    fs::remove_all( root );
}


static void testRealBackend()
{
    // The platform backend (libsecret when CABLY_HAVE_LIBSECRET) is probed for real:
    // in a container there is no D-Bus session, so it reports unavailable and the file
    // is used; on a desktop with a running Secret Service the item goes there.  Either
    // way the round-trip must hold.
    fs::path root = scratchDir();
    ::setenv( "XDG_CONFIG_HOME", ( root / "xdg" ).c_str(), 1 );

    CABLY_KEYCHAIN_SECRET_STORE store( "dev.cably.desktop.test.unit" );
    CHECK( store.Save( sampleSession() ) );
    const std::string where = store.Backend();
    CHECK( where == "secret-service" || where.rfind( "file:", 0 ) == 0 );
#if CABLY_HAVE_LIBSECRET
    std::printf( "libsecret backend compiled in; this run used: %s\n", where.c_str() );
#else
    std::printf( "built WITHOUT libsecret; this run used: %s\n", where.c_str() );
    CHECK( where.rfind( "file:", 0 ) == 0 );
#endif

    if( where.rfind( "file:", 0 ) == 0 )
    {
        const std::string path = where.substr( 5 );
        CHECK( path == ( root / "xdg" ).string() + "/cably-desktop/session.dev.cably.desktop.test.unit.json" );
        CHECK( modeOf( path ) == 0600 );
        CHECK( modeOf( ( root / "xdg" ).string() + "/cably-desktop" ) == 0700 );
    }

    CABLY_SESSION loaded;
    CHECK( store.Load( loaded ) );
    CHECK( sameSession( loaded, sampleSession() ) );
    CHECK( store.Clear() );
    CHECK( !store.Load( loaded ) );
    CHECK( store.LastError().empty() );

    ::unsetenv( "XDG_CONFIG_HOME" );
    fs::remove_all( root );
}

#endif // !__APPLE__


int main()
{
    testFallbackPathRule();
#if defined( __APPLE__ )
    CABLY_KEYCHAIN_SECRET_STORE kc( "dev.cably.desktop.test.unit" );
    CHECK( kc.Backend() == "keychain" );
    CHECK( CABLY_KEYCHAIN_SECRET_STORE::DefaultFallbackDir().empty() );
    std::printf( "test_cably_secret_store: macOS uses Security.framework (cably/tests/bridge.sh (e)); Linux cases skipped\n" );
#else
    testFileFallback();
    testDefaultPaths();
    testCorruptFile();
    testServiceBackend();
    testServiceUnavailableIsTheFileCase();
    testServiceMigrationFromFile();
    testReachableServiceFailuresAreErrors();
    testRealBackend();
#endif
    std::printf( "test_cably_secret_store: %d checks passed\n", g_checks );
    return 0;
}

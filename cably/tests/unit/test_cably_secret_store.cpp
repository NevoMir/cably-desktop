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
 *  - No Secret Service usable, for ANY reason (no session bus - a container, ssh; a bus
 *    address nobody listens on; a bus without a keyring on it, which is what GitHub's
 *    ubuntu runners have; a bus that never replies; a D-Bus error in the middle of a
 *    lookup/store/clear): the session lives in $XDG_CONFIG_HOME/cably-desktop/session.json
 *    (~/.config when XDG_CONFIG_HOME is unset or relative), mode 0600 in a 0700
 *    directory, written temp-then-rename; Backend() reports "file:<path>", LastError() is
 *    empty and Note() says why the service was not used (empty when there is no bus at
 *    all).  Never an error, never a half state.
 *  - A Secret Service that answers: the item goes there (Backend() "secret-service"), a
 *    file copy left from before is removed by the next Save.  Only a stored item that is
 *    not a session is an error.
 *  - The service half sits behind CABLY_SECRET_SERVICE so this test drives it with a
 *    fake; the real libsecret backend is probed for real at the end - in this process on
 *    whatever bus the machine has, and in child processes (this binary re-executed with
 *    CABLY_SECRET_STORE_TEST_CASE set) on a dead bus address, on a Unix socket that
 *    listens but never answers (the watchdog must cut the probe short), and, when
 *    dbus-run-session is installed, on a real session bus with no Secret Service - the
 *    CI case - where the note must carry libsecret's "org.freedesktop.secrets" error.
 * On macOS the store is Security.framework (cably/tests/bridge.sh (e)); there this test
 * only checks the platform-independent bits.
 */

#include <cably_bridge.h>
#include <cably_config.h>

#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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


static bool contains( const std::string& aText, const char* aNeedle )
{
    return aText.find( aNeedle ) != std::string::npos;
}


/// In-memory stand-in for the Secret Service with switchable failure modes.
class FAKE_SECRET_SERVICE : public CABLY_SECRET_SERVICE
{
public:
    bool                               available = true;
    std::string                        unavailableReason; // what Available() reports when it is false
    bool                               storeFails = false;
    bool                               lookupErrors = false;
    bool                               clearFails = false;
    int                                probes = 0, lookups = 0, stores = 0, clears = 0;
    std::map<std::string, std::string> items;

    bool Available( std::string& aReason ) override
    {
        ++probes;
        aReason = available ? std::string() : unavailableReason;
        return available;
    }

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


/**
 * A backend with no keyring behind it (a container, ssh, no bus at all).  The file-only
 * cases use it rather than the platform backend (a nullptr backend), so they never depend
 * on whether the machine running the test happens to have a Secret Service; the platform
 * backend gets its own cases at the end.
 */
static FAKE_SECRET_SERVICE noKeyring()
{
    FAKE_SECRET_SERVICE f;
    f.available = false;
    return f;
}


static void testFileFallback()
{
    fs::path            root = scratchDir();
    const std::string   dir = ( root / "cfg" ).string();
    const std::string   file = dir + "/session.json";
    FAKE_SECRET_SERVICE none = noKeyring();

    // No service at all: the file is the store.
    CABLY_KEYCHAIN_SECRET_STORE store( CABLY_KEYCHAIN_SERVICE, &none, dir );
    CHECK( CABLY_KEYCHAIN_SECRET_STORE::FallbackPath( dir, CABLY_KEYCHAIN_SERVICE ) == file );

    CABLY_SESSION out;
    CHECK( !store.Load( out ) );
    CHECK( store.LastError().empty() ); // nothing stored is not an error
    CHECK( store.Backend() == "file:" + file );
    CHECK( store.Note().empty() ); // no bus at all: nothing to explain
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
    CABLY_KEYCHAIN_SECRET_STORE other( "dev.cably.desktop.test", &none, dir );
    CHECK( other.Save( in ) );
    CHECK( other.Backend() == "file:" + otherFile );
    CHECK( fs::exists( otherFile ) );
    CHECK( modeOf( otherFile ) == 0600 );
    CHECK( store.Load( loaded ) ); // the default one is untouched
    CHECK( other.Clear() && store.Clear() );
    CHECK( !fs::exists( otherFile ) && !fs::exists( file ) );

    // The service was never asked for anything: it is not there.
    CHECK( none.lookups == 0 && none.stores == 0 && none.clears == 0 );

    fs::remove_all( root );
}


static void testDefaultPaths()
{
    fs::path            root = scratchDir();
    FAKE_SECRET_SERVICE none = noKeyring();

    ::setenv( "XDG_CONFIG_HOME", ( root / "xdg" ).c_str(), 1 );
    ::setenv( "HOME", ( root / "home" ).c_str(), 1 );
    CHECK( CABLY_KEYCHAIN_SECRET_STORE::DefaultFallbackDir() == ( root / "xdg" ).string() + "/cably-desktop" );

    // A relative XDG_CONFIG_HOME must be ignored (XDG base directory spec).
    ::setenv( "XDG_CONFIG_HOME", "relative/dir", 1 );
    CHECK( CABLY_KEYCHAIN_SECRET_STORE::DefaultFallbackDir() == ( root / "home" ).string() + "/.config/cably-desktop" );

    ::unsetenv( "XDG_CONFIG_HOME" );
    CHECK( CABLY_KEYCHAIN_SECRET_STORE::DefaultFallbackDir() == ( root / "home" ).string() + "/.config/cably-desktop" );

    // An empty aFallbackDir means the default; with no service the store writes there,
    // creating ~/.config/cably-desktop (0700) on the way.
    CABLY_KEYCHAIN_SECRET_STORE store( "dev.cably.desktop.test.paths", &none, std::string() );
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
    fs::path            root = scratchDir();
    const std::string   dir = ( root / "cfg" ).string();
    FAKE_SECRET_SERVICE none = noKeyring();
    fs::create_directories( dir );
    {
        std::ofstream f( dir + "/session.json" );
        f << "this is not json";
    }

    CABLY_KEYCHAIN_SECRET_STORE store( CABLY_KEYCHAIN_SERVICE, &none, dir );
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
    CHECK( store.Note().empty() );
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
    CHECK( store.Backend() == "secret-service" );
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
    fake.unavailableReason = "The name org.freedesktop.secrets was not provided by any .service files";

    CABLY_KEYCHAIN_SECRET_STORE store( CABLY_KEYCHAIN_SERVICE, &fake, dir );
    CHECK( store.Save( sampleSession() ) );
    CHECK( store.LastError().empty() );
    CHECK( store.Backend() == "file:" + file );
    CHECK( contains( store.Note(), "org.freedesktop.secrets was not provided" ) ); // the reason is kept
    CHECK( contains( store.Note(), "using the file" ) );
    CHECK( fake.stores == 0 ); // never touched
    CHECK( modeOf( file ) == 0600 );

    CABLY_SESSION loaded;
    CHECK( store.Load( loaded ) && loaded.email == "unit@example.com" );
    CHECK( store.LastError().empty() );
    CHECK( contains( store.Note(), "org.freedesktop.secrets" ) );
    CHECK( fake.lookups == 0 );
    CHECK( store.Clear() );
    CHECK( store.LastError().empty() );
    CHECK( store.Backend() == "file:" + file );
    CHECK( fake.clears == 0 );
    CHECK( !fs::exists( file ) );

    // No reason (no bus at all) leaves the note empty: nothing to explain.
    fake.unavailableReason.clear();
    CHECK( store.Save( sampleSession() ) );
    CHECK( store.Backend() == "file:" + file );
    CHECK( store.Note().empty() );
    CHECK( store.Clear() );
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
        FAKE_SECRET_SERVICE         none = noKeyring();
        CABLY_KEYCHAIN_SECRET_STORE before( CABLY_KEYCHAIN_SERVICE, &none, dir );
        CHECK( before.Save( sampleSession() ) );
    }

    CABLY_KEYCHAIN_SECRET_STORE store( CABLY_KEYCHAIN_SERVICE, &fake, dir );
    CABLY_SESSION               loaded;
    CHECK( store.Load( loaded ) );
    CHECK( loaded.email == "unit@example.com" );
    CHECK( store.Backend() == "file:" + file ); // found in the file after an empty lookup
    CHECK( store.LastError().empty() );
    CHECK( store.Note().empty() ); // the service answered; it just had nothing
    CHECK( fake.lookups == 1 );

    // The next Save moves it into the service and drops the file.
    CHECK( store.Save( loaded ) );
    CHECK( store.Backend() == "secret-service" );
    CHECK( !fs::exists( file ) );
    CHECK( fake.items.count( CABLY_KEYCHAIN_SERVICE ) == 1 );

    CHECK( store.Clear() );
    fs::remove_all( root );
}


static void testServiceFailuresFallBackToTheFile()
{
    // The service answered the probe but a call to it fails (the keyring went away, D-Bus
    // timed out, the collection could not be unlocked ...): that is the file case too -
    // the session is never lost and the app never sees an error, but Note() says what
    // happened.  Written RED against the store that reported such failures as errors
    // (GitHub's ubuntu runners: a session bus, no keyring, "The name
    // org.freedesktop.secrets was not provided by any .service files" on the store call).
    fs::path            root = scratchDir();
    const std::string   dir = ( root / "cfg" ).string();
    const std::string   file = dir + "/session.json";
    FAKE_SECRET_SERVICE fake;

    CABLY_KEYCHAIN_SECRET_STORE store( CABLY_KEYCHAIN_SERVICE, &fake, dir );

    // Store fails: the session goes to the 0600 file, no error.
    fake.storeFails = true;
    CHECK( store.Save( sampleSession() ) );
    CHECK( store.LastError().empty() );
    CHECK( store.Backend() == "file:" + file );
    CHECK( contains( store.Note(), "fake store error" ) );
    CHECK( contains( store.Note(), "using the file" ) );
    CHECK( fake.stores == 1 ); // it was tried first
    CHECK( modeOf( file ) == 0600 );
    CHECK( modeOf( dir ) == 0700 );

    // Lookup errors: the file copy is read, no error.
    fake.lookupErrors = true;
    CABLY_SESSION loaded;
    CHECK( store.Load( loaded ) );
    CHECK( store.LastError().empty() );
    CHECK( sameSession( loaded, sampleSession() ) );
    CHECK( store.Backend() == "file:" + file );
    CHECK( contains( store.Note(), "fake lookup error" ) );
    CHECK( fake.lookups == 1 );

    // Clear failing in the service: the file copy is removed and Clear succeeds.
    fake.clearFails = true;
    CHECK( store.Clear() );
    CHECK( store.LastError().empty() );
    CHECK( store.Backend() == "file:" + file );
    CHECK( contains( store.Note(), "fake clear error" ) );
    CHECK( !fs::exists( file ) );
    CHECK( !store.Load( loaded ) ); // gone
    CHECK( store.LastError().empty() );

    // The service recovers: the next Save moves the session into it and drops the file
    // (no half state: the file never outlives a successful service write).
    fake.storeFails = false;
    fake.lookupErrors = false;
    fake.clearFails = false;
    CHECK( store.Save( sampleSession() ) ); // file first? no: the service works now
    CHECK( store.Backend() == "secret-service" );
    CHECK( store.Note().empty() );
    CHECK( !fs::exists( file ) );
    CHECK( fake.items.count( CABLY_KEYCHAIN_SERVICE ) == 1 );

    // ... and a session written to the file during an outage is picked up by Load once the
    // service is back but empty, then moved on the next Save.
    fake.items.clear();
    fake.storeFails = true;
    CHECK( store.Save( sampleSession() ) );
    CHECK( fs::exists( file ) );
    fake.storeFails = false;
    CHECK( store.Load( loaded ) && sameSession( loaded, sampleSession() ) );
    CHECK( store.Backend() == "file:" + file );
    CHECK( store.Save( loaded ) );
    CHECK( store.Backend() == "secret-service" && !fs::exists( file ) );

    // A service item that is not a session is still an error: that is data, not reach.
    fake.items[CABLY_KEYCHAIN_SERVICE] = "garbage";
    CHECK( !store.Load( loaded ) );
    CHECK( !store.LastError().empty() );
    CHECK( store.Backend() == "secret-service" );
    fake.items.clear();

    CHECK( store.Clear() );
    CHECK( fake.items.empty() );

    fs::remove_all( root );
}


/**
 * The platform backend (libsecret when CABLY_HAVE_LIBSECRET) probed for real on the bus
 * this process has.  aExpectFile: the file MUST be used (a broken bus) and the note must
 * contain aNoteNeedle when given; otherwise whichever backend the machine offers is fine
 * (a desktop keyring, or the file) and the round-trip must hold either way.
 */
static void testRealBackend( const char* aCase, bool aExpectFile, const char* aNoteNeedle )
{
    fs::path root = scratchDir();
    ::setenv( "XDG_CONFIG_HOME", ( root / "xdg" ).c_str(), 1 );

    const auto                  t0 = std::chrono::steady_clock::now();
    CABLY_KEYCHAIN_SECRET_STORE store( "dev.cably.desktop.test.unit" );
    CHECK( store.Save( sampleSession() ) );
    const auto        ms = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - t0 ).count();
    const std::string where = store.Backend();
    CHECK( store.LastError().empty() );
    CHECK( where == "secret-service" || where.rfind( "file:", 0 ) == 0 );
#if CABLY_HAVE_LIBSECRET
    std::printf( "test_cably_secret_store[%s]: libsecret compiled in; used %s in %lld ms%s%s\n", aCase, where.c_str(),
                 static_cast<long long>( ms ), store.Note().empty() ? "" : "; note: ", store.Note().c_str() );
#else
    std::printf( "test_cably_secret_store[%s]: built WITHOUT libsecret; used %s\n", aCase, where.c_str() );
    CHECK( where.rfind( "file:", 0 ) == 0 );
#endif

    if( aExpectFile )
        CHECK( where.rfind( "file:", 0 ) == 0 );

    if( aNoteNeedle )
        CHECK( contains( store.Note(), aNoteNeedle ) );

    if( where.rfind( "file:", 0 ) == 0 )
    {
        const std::string path = where.substr( 5 );
        CHECK( path == ( root / "xdg" ).string() + "/cably-desktop/session.dev.cably.desktop.test.unit.json" );
        CHECK( modeOf( path ) == 0600 );
        CHECK( modeOf( ( root / "xdg" ).string() + "/cably-desktop" ) == 0700 );
    }

    CABLY_SESSION loaded;
    CHECK( store.Load( loaded ) );
    CHECK( store.LastError().empty() );
    CHECK( sameSession( loaded, sampleSession() ) );
    CHECK( store.Backend() == where );
    CHECK( store.Clear() );
    CHECK( store.LastError().empty() );
    CHECK( !store.Load( loaded ) );
    CHECK( store.LastError().empty() );

    ::unsetenv( "XDG_CONFIG_HOME" );
    fs::remove_all( root );
}


/// A Unix socket that listens but never accepts nor answers: a "bus" with nothing to say.
struct SILENT_SOCKET
{
    int         fd = -1;
    std::string path;

    explicit SILENT_SOCKET( const std::string& aPath ) : path( aPath )
    {
        fd = ::socket( AF_UNIX, SOCK_STREAM, 0 );
        CHECK( fd >= 0 );
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        CHECK( aPath.size() < sizeof( addr.sun_path ) );
        std::strncpy( addr.sun_path, aPath.c_str(), sizeof( addr.sun_path ) - 1 );
        CHECK( ::bind( fd, reinterpret_cast<sockaddr*>( &addr ), sizeof( addr ) ) == 0 );
        CHECK( ::listen( fd, 4 ) == 0 );
    }

    ~SILENT_SOCKET()
    {
        if( fd >= 0 )
            ::close( fd );

        ::unlink( path.c_str() );
    }
};


/// Child-process entry: one real-backend case under the bus this process was given.
static int runChildCase( const std::string& aCase )
{
    if( aCase == "bus-nobody-listens" )
    {
        // DBUS_SESSION_BUS_ADDRESS names a socket that does not exist: connect fails at
        // once, the file is used, the note says so.
        testRealBackend( aCase.c_str(), true, "secret service unavailable" );
    }
    else if( aCase == "bus-never-answers" )
    {
        // A socket that listens but never completes the D-Bus handshake: without the
        // watchdog the probe would block for good.  CABLY_SECRET_SERVICE_TIMEOUT_MS is
        // set short by the parent; the Save must return within a few times that.
        fs::path      root = scratchDir();
        SILENT_SOCKET sock( ( root / "bus" ).string() );
        ::setenv( "DBUS_SESSION_BUS_ADDRESS", ( "unix:path=" + sock.path ).c_str(), 1 );
        const auto t0 = std::chrono::steady_clock::now();
        testRealBackend( aCase.c_str(), true, "secret service" );
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - t0 ).count();
        std::printf( "test_cably_secret_store[%s]: save+load+clear took %lld ms\n", aCase.c_str(), static_cast<long long>( ms ) );
        CHECK( ms < 6000 ); // 3 operations x a 500 ms watchdog, with margin; never 25 s
        fs::remove_all( root );
    }
    else if( aCase == "bus-without-service" )
    {
        // dbus-run-session: a real session bus, nothing providing org.freedesktop.secrets
        // - exactly what GitHub's ubuntu runners have.
        const char* addr = std::getenv( "DBUS_SESSION_BUS_ADDRESS" );
        CHECK( addr && *addr );
        testRealBackend( aCase.c_str(), true, "org.freedesktop.secrets" );
    }
    else
    {
        std::fprintf( stderr, "unknown CABLY_SECRET_STORE_TEST_CASE '%s'\n", aCase.c_str() );
        return 2;
    }

    std::printf( "test_cably_secret_store[%s]: %d checks passed\n", aCase.c_str(), g_checks );
    return 0;
}


static std::string selfExecutable( const char* aArgv0 )
{
    char    buf[PATH_MAX];
    ssize_t n = ::readlink( "/proc/self/exe", buf, sizeof( buf ) - 1 );

    if( n > 0 )
        return std::string( buf, static_cast<size_t>( n ) );

    return aArgv0 ? aArgv0 : "";
}


/// Re-run this binary on the broken buses; each child must pass, and quickly.
static void testRealBackendOnBrokenBuses( const char* aArgv0 )
{
    const std::string self = selfExecutable( aArgv0 );
    CHECK( !self.empty() && fs::exists( self ) );

    auto run = [&]( const char* aCase, const std::string& aPrefix, long aMaxMs )
    {
        const std::string cmd = aPrefix + " CABLY_SECRET_STORE_TEST_CASE=" + aCase + " '" + self + "'";
        std::printf( "test_cably_secret_store: child %s: %s\n", aCase, cmd.c_str() );
        std::fflush( stdout );
        const auto t0 = std::chrono::steady_clock::now();
        const int  rc = std::system( cmd.c_str() );
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - t0 ).count();
        std::printf( "test_cably_secret_store: child %s: rc=%d in %lld ms\n", aCase, rc, static_cast<long long>( ms ) );
        CHECK( rc == 0 );
        CHECK( ms < aMaxMs );
    };

    run( "bus-nobody-listens", "env DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/cably-bus", 4000 );
    run( "bus-never-answers", "env CABLY_SECRET_SERVICE_TIMEOUT_MS=500", 8000 );

    if( std::system( "command -v dbus-run-session >/dev/null 2>&1" ) == 0 )
        run( "bus-without-service", "dbus-run-session -- env", 4000 );
    else
        std::printf( "test_cably_secret_store: skip bus-without-service (dbus-run-session not installed)\n" );
}

#endif // !__APPLE__


int main( int, char** argv )
{
#if !defined( __APPLE__ )
    if( const char* childCase = std::getenv( "CABLY_SECRET_STORE_TEST_CASE" ) )
        return runChildCase( childCase );
#endif

    testFallbackPathRule();
#if defined( __APPLE__ )
    ( void) argv;
    CABLY_KEYCHAIN_SECRET_STORE kc( "dev.cably.desktop.test.unit" );
    CHECK( kc.Backend() == "keychain" );
    CHECK( kc.Note().empty() );
    CHECK( CABLY_KEYCHAIN_SECRET_STORE::DefaultFallbackDir().empty() );
    std::printf( "test_cably_secret_store: macOS uses Security.framework (cably/tests/bridge.sh (e)); Linux cases skipped\n" );
#else
    testFileFallback();
    testDefaultPaths();
    testCorruptFile();
    testServiceBackend();
    testServiceUnavailableIsTheFileCase();
    testServiceMigrationFromFile();
    testServiceFailuresFallBackToTheFile();
    testRealBackend( "this process", false, nullptr );
    testRealBackendOnBrokenBuses( argv[0] );
#endif
    std::printf( "test_cably_secret_store: %d checks passed\n", g_checks );
    return 0;
}

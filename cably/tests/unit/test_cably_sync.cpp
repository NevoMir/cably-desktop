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
 * Standalone unit test for the F5 sync bridge (cably/src/cably_sync.{h,cpp} and the
 * ImportProject half of cably/src/cably_bridge.{h,cpp}).  Written BEFORE the code
 * existed.  Built and run by cably/tests/bridge.sh with plain clang++: pure C++17, no
 * wxWidgets, no KiCad libraries, NO real network - HTTP goes through a recording fake,
 * the watcher runs on a real temp folder.
 *
 * The watcher cases are the Electron companion's (desktop/src/main/watcher.test.ts)
 * translated one to one, so both desktops agree on what a "save" is.
 */

#include <cably_bridge.h>
#include <cably_sync.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

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


class FAKE_HTTP : public CABLY_HTTP
{
public:
    std::vector<CABLY_HTTP_REQUEST>  requests;
    std::vector<CABLY_HTTP_RESPONSE> queue;

    void Enqueue( int aStatus, const std::string& aBody )
    {
        CABLY_HTTP_RESPONSE r;
        r.transportOk = true;
        r.status = aStatus;
        r.body = aBody;
        queue.push_back( r );
    }

    CABLY_HTTP_RESPONSE Perform( const CABLY_HTTP_REQUEST& aRequest ) override
    {
        requests.push_back( aRequest );

        if( queue.empty() )
        {
            CABLY_HTTP_RESPONSE r;
            r.transportOk = false;
            r.error = "fake: no canned response";
            return r;
        }

        CABLY_HTTP_RESPONSE r = queue.front();
        queue.erase( queue.begin() );
        return r;
    }
};


static bool contains( const std::string& aHay, const std::string& aNeedle )
{
    return aHay.find( aNeedle ) != std::string::npos;
}


static bool startsWith( const std::string& aHay, const std::string& aPrefix )
{
    return aHay.compare( 0, aPrefix.size(), aPrefix ) == 0;
}


static std::string readFile( const fs::path& aPath )
{
    std::ifstream in( aPath, std::ios::binary );
    return std::string( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
}


static void writeFile( const fs::path& aPath, const std::string& aText )
{
    std::ofstream out( aPath, std::ios::binary | std::ios::trunc );
    out << aText;
}


static void sleepMs( int aMs )
{
    std::this_thread::sleep_for( std::chrono::milliseconds( aMs ) );
}


static CABLY_BRIDGE_CONFIG testConfig()
{
    CABLY_BRIDGE_CONFIG c;
    c.supabaseUrl = "https://supabase.test";
    c.publishableKey = "pk-test";
    c.engineUrl = "https://engine.test";
    c.authPageUrl = "https://cably.test/desktop/auth";
    return c;
}


static CABLY_SESSION testSession()
{
    CABLY_SESSION s;
    s.accessToken = "access-1";
    s.refreshToken = "refresh-1";
    s.email = "dev@example.com";
    s.expiresAt = 4102444800LL;
    return s;
}


/// Collects watcher events thread-safely (the callback runs on the watcher thread).
struct EVENTS
{
    std::mutex                    mutex;
    std::vector<CABLY_SAVE_EVENT> list;
    Clock::time_point             firstAt;

    void Push( const CABLY_SAVE_EVENT& e )
    {
        std::lock_guard<std::mutex> lock( mutex );

        if( list.empty() )
            firstAt = Clock::now();

        list.push_back( e );
    }

    size_t Count()
    {
        std::lock_guard<std::mutex> lock( mutex );
        return list.size();
    }

    CABLY_SAVE_EVENT At( size_t i )
    {
        std::lock_guard<std::mutex> lock( mutex );
        return list.at( i );
    }

    /// true when at least aN events arrived within aTimeoutMs.
    bool WaitFor( size_t aN, int aTimeoutMs = 4000 )
    {
        Clock::time_point t0 = Clock::now();

        while( Count() < aN )
        {
            if( Clock::now() - t0 > std::chrono::milliseconds( aTimeoutMs ) )
                return false;

            sleepMs( 10 );
        }

        return true;
    }
};


struct FOLDER
{
    fs::path dir;
    fs::path pcb;
    fs::path sch;

    explicit FOLDER( const std::string& aTag )
    {
        dir = fs::temp_directory_path() / ( "cably-sync-test-" + aTag + "-" + std::to_string( ::getpid() ) );
        fs::remove_all( dir );
        fs::create_directories( dir );
        pcb = dir / "B.kicad_pcb";
        sch = dir / "B.kicad_sch";
        writeFile( pcb, "(kicad_pcb v1)" );
        writeFile( sch, "(kicad_sch v1)" );
    }

    ~FOLDER() { fs::remove_all( dir ); }
};


static CABLY_WATCH_OPTIONS fastOptions( int aDebounceMs )
{
    CABLY_WATCH_OPTIONS o;
    o.debounceMs = aDebounceMs;
    o.pollMs = 20;
    return o;
}


// --------------------------------------------------------------------------------
// classifyPath: the Electron table, verbatim.
// --------------------------------------------------------------------------------
static void testClassify()
{
    CABLY_SAVE_KIND kind;

    CHECK( CablyClassifySavePath( "/x/Board.kicad_pcb", &kind ) && kind == CABLY_SAVE_KIND::BOARD );
    CHECK( CablyClassifySavePath( "/x/Board.kicad_sch", &kind ) && kind == CABLY_SAVE_KIND::SCHEMATIC );
    CHECK( !CablyClassifySavePath( "/x/Board.kicad_pro" ) );
    CHECK( !CablyClassifySavePath( "/x/fp-info-cache" ) );
    CHECK( !CablyClassifySavePath( "/x/Board.kicad_prl" ) );

    // KiCad's backups, locks and autosaves are not the user's save.
    CHECK( !CablyClassifySavePath( "/x/Board.kicad_pcb-bak" ) );
    CHECK( !CablyClassifySavePath( "/x/Board.kicad_sch-bak" ) );
    CHECK( !CablyClassifySavePath( "/x/~Board.kicad_pcb.lck" ) );
    CHECK( !CablyClassifySavePath( "/x/_autosave-Board.kicad_pcb" ) );
    CHECK( !CablyClassifySavePath( "/x/_saved_Board.kicad_pcb" ) );
    CHECK( !CablyClassifySavePath( "/x/Board-backups/Board.kicad_pcb" ) );

    // Both separators, a bare name, and a temp file next to the document.
    CHECK( !CablyClassifySavePath( "C:\\x\\Board-backups\\Board.kicad_pcb" ) );
    CHECK( CablyClassifySavePath( "Board.kicad_pcb", &kind ) && kind == CABLY_SAVE_KIND::BOARD );
    CHECK( !CablyClassifySavePath( "/x/B.kicad_pcb.tmp-write" ) );
    CHECK( !CablyClassifySavePath( "" ) );
}


// --------------------------------------------------------------------------------
// The watcher on a real folder.
// --------------------------------------------------------------------------------
static void testWatchInPlace()
{
    FOLDER              f( "inplace" );
    EVENTS              ev;
    CABLY_PROJECT_WATCH w;
    std::string         err;
    CHECK( w.Start( f.dir.string(), [&]( const CABLY_SAVE_EVENT& e ) { ev.Push( e ); },
                    fastOptions( 100 ), &err ) );
    CHECK( w.IsRunning() );
    CHECK( w.Dir() == f.dir.string() );

    Clock::time_point t0 = Clock::now();
    writeFile( f.pcb, "(kicad_pcb v2 saved-by-kicad)" );
    CHECK( ev.WaitFor( 1 ) );
    long long latency = std::chrono::duration_cast<std::chrono::milliseconds>( ev.firstAt - t0 ).count();
    std::printf( "  in-place save -> event latency: %lld ms (debounce 100, poll 20)\n", latency );
    CHECK( latency >= 100 );  // never before the debounce elapsed
    CHECK( latency < 2000 );

    CABLY_SAVE_EVENT e = ev.At( 0 );
    CHECK( e.kind == CABLY_SAVE_KIND::BOARD );
    CHECK( e.path == f.pcb.string() );
    CHECK( e.text == "(kicad_pcb v2 saved-by-kicad)" );

    // The accepted save became the new baseline.
    CHECK( w.KnownHash( f.pcb.string() ) == CABLY_BRIDGE::Sha256Hex( "(kicad_pcb v2 saved-by-kicad)" ) );
    w.Stop();
    CHECK( !w.IsRunning() );
}


static void testWatchBurst()
{
    FOLDER              f( "burst" );
    EVENTS              ev;
    CABLY_PROJECT_WATCH w;
    CHECK( w.Start( f.dir.string(), [&]( const CABLY_SAVE_EVENT& e ) { ev.Push( e ); }, fastOptions( 250 ) ) );

    writeFile( f.sch, "(kicad_sch partial" );
    sleepMs( 30 );
    writeFile( f.sch, "(kicad_sch partial more" );
    sleepMs( 30 );
    writeFile( f.sch, "(kicad_sch final)" );
    CHECK( ev.WaitFor( 1 ) );
    sleepMs( 500 ); // give a wrongly-implemented watcher time to emit extras
    CHECK( ev.Count() == 1 );
    CHECK( ev.At( 0 ).kind == CABLY_SAVE_KIND::SCHEMATIC );
    CHECK( ev.At( 0 ).text == "(kicad_sch final)" );
    w.Stop();
}


static void testWatchAtomicRename()
{
    FOLDER              f( "atomic" );
    EVENTS              ev;
    CABLY_PROJECT_WATCH w;
    CHECK( w.Start( f.dir.string(), [&]( const CABLY_SAVE_EVENT& e ) { ev.Push( e ); }, fastOptions( 100 ) ) );

    fs::path tmp = f.dir / "B.kicad_pcb.tmp-write";
    writeFile( tmp, "(kicad_pcb atomic)" );
    fs::rename( tmp, f.pcb );
    CHECK( ev.WaitFor( 1 ) );
    CHECK( ev.At( 0 ).text == "(kicad_pcb atomic)" );
    CHECK( ev.At( 0 ).path == f.pcb.string() );
    sleepMs( 300 );
    CHECK( ev.Count() == 1 ); // the temp file itself never reported
    w.Stop();
}


static void testWatchIgnoresBackupsAndLocks()
{
    FOLDER              f( "backups" );
    EVENTS              ev;
    CABLY_PROJECT_WATCH w;
    CHECK( w.Start( f.dir.string(), [&]( const CABLY_SAVE_EVENT& e ) { ev.Push( e ); }, fastOptions( 100 ) ) );

    writeFile( f.dir / "B.kicad_pcb-bak", "old" );
    writeFile( f.dir / "~B.kicad_pcb.lck", "{}" );
    writeFile( f.dir / "_autosave-B.kicad_pcb", "(kicad_pcb auto)" );
    writeFile( f.dir / "_saved_B.kicad_pcb", "(kicad_pcb saved)" );
    writeFile( f.dir / "B.kicad_pro", "{}" );
    fs::create_directories( f.dir / "B-backups" );
    writeFile( f.dir / "B-backups" / "B.kicad_pcb", "(kicad_pcb backup)" );
    sleepMs( 600 );
    CHECK( ev.Count() == 0 );
    w.Stop();
}


static void testWatchExpectOwnWrite()
{
    FOLDER              f( "expect" );
    EVENTS              ev;
    CABLY_PROJECT_WATCH w;
    CHECK( w.Start( f.dir.string(), [&]( const CABLY_SAVE_EVENT& e ) { ev.Push( e ); }, fastOptions( 100 ) ) );

    // Our own hand-off write is announced first, so it is not a KiCad save...
    w.Expect( f.pcb.string(), "(kicad_pcb written-by-cably)" );
    writeFile( f.pcb, "(kicad_pcb written-by-cably)" );
    sleepMs( 700 );
    CHECK( ev.Count() == 0 );

    // ...but the next real one is still heard.
    writeFile( f.pcb, "(kicad_pcb then-edited-in-kicad)" );
    CHECK( ev.WaitFor( 1 ) );
    CHECK( ev.Count() == 1 );
    CHECK( ev.At( 0 ).text == "(kicad_pcb then-edited-in-kicad)" );
    w.Stop();
}


static void testWatchSameBytes()
{
    FOLDER              f( "touch" );
    EVENTS              ev;
    CABLY_PROJECT_WATCH w;
    CHECK( w.Start( f.dir.string(), [&]( const CABLY_SAVE_EVENT& e ) { ev.Push( e ); }, fastOptions( 100 ) ) );

    writeFile( f.pcb, "(kicad_pcb v1)" ); // identical to what setup wrote
    sleepMs( 700 );
    CHECK( ev.Count() == 0 );
    w.Stop();
}


static void testWatchStop()
{
    FOLDER              f( "stop" );
    EVENTS              ev;
    CABLY_PROJECT_WATCH w;
    CHECK( w.Start( f.dir.string(), [&]( const CABLY_SAVE_EVENT& e ) { ev.Push( e ); }, fastOptions( 50 ) ) );
    w.Stop();
    writeFile( f.pcb, "(kicad_pcb after-close)" );
    sleepMs( 500 );
    CHECK( ev.Count() == 0 );

    // A missing folder cannot be watched.
    std::string err;
    CHECK( !w.Start( ( f.dir / "nope" ).string(), []( const CABLY_SAVE_EVENT& ) {}, fastOptions( 50 ), &err ) );
    CHECK( !err.empty() );
    CHECK( !w.IsRunning() );
}


static void testWatchBaselineFromSidecar()
{
    FOLDER f( "sidecar" );

    // The sidecar remembers what the cloud last wrote; the board on disk was edited while
    // nobody watched (the app was closed).  The baseline is the SIDECAR's hash, so...
    CABLY_EXPORT_SIDECAR sc;
    sc.stem = "B";
    sc.files["B.kicad_pcb"] = CABLY_BRIDGE::Sha256Hex( "(kicad_pcb from-cloud)" );
    sc.meta.projectId = "p1";
    CHECK( sc.Save( f.dir.string() ) );
    writeFile( f.pcb, "(kicad_pcb edited-while-closed)" );

    EVENTS              ev;
    CABLY_PROJECT_WATCH w;
    CHECK( w.Start( f.dir.string(), [&]( const CABLY_SAVE_EVENT& e ) { ev.Push( e ); }, fastOptions( 100 ) ) );
    CHECK( w.KnownHash( f.pcb.string() ) == CABLY_BRIDGE::Sha256Hex( "(kicad_pcb from-cloud)" ) );
    // ...a document the sidecar does not know is baselined from disk (Electron semantics)...
    CHECK( w.KnownHash( f.sch.string() ) == CABLY_BRIDGE::Sha256Hex( "(kicad_sch v1)" ) );
    CHECK( w.KnownHash( ( f.dir / "nothing.kicad_pcb" ).string() ).empty() );

    // ...reverting to the cloud's text is not a save, while any other write is.
    writeFile( f.pcb, "(kicad_pcb from-cloud)" );
    sleepMs( 600 );
    CHECK( ev.Count() == 0 );
    writeFile( f.pcb, "(kicad_pcb edited-again)" );
    CHECK( ev.WaitFor( 1 ) );
    CHECK( ev.At( 0 ).text == "(kicad_pcb edited-again)" );
    w.Stop();
}


// --------------------------------------------------------------------------------
// Sidecar round trip + timestamp comparison.
// --------------------------------------------------------------------------------
static void testSidecarRoundTrip()
{
    FOLDER               f( "sc" );
    CABLY_EXPORT_SIDECAR sc;
    sc.stem = "Blinking_LED";
    sc.files["Blinking_LED.kicad_pcb"] = std::string( 64, 'a' );
    sc.files["Blinking_LED.kicad_sch"] = std::string( 64, 'b' );
    sc.meta.projectId = "p1";
    sc.meta.projectName = "Blinking LED";
    sc.meta.cloudUpdatedAt = "2026-09-03T10:00:00.123456+00:00";
    sc.meta.engineVersion = "abc123";
    CHECK( sc.Save( f.dir.string() ) );
    CHECK( fs::exists( f.dir / CABLY_EXPORT_SIDECAR::FileName() ) );
    CHECK( std::string( CABLY_EXPORT_SIDECAR::FileName() ) == ".cably-export.json" );

    json j = json::parse( readFile( f.dir / ".cably-export.json" ) );
    CHECK( j["stem"] == "Blinking_LED" );
    CHECK( j["engine"] == "cably" );
    CHECK( j["projectId"] == "p1" );
    CHECK( j["projectName"] == "Blinking LED" );
    CHECK( j["cloudUpdatedAt"] == "2026-09-03T10:00:00.123456+00:00" );
    CHECK( j["engineVersion"] == "abc123" );
    CHECK( j["files"]["Blinking_LED.kicad_sch"] == std::string( 64, 'b' ) );

    CABLY_EXPORT_SIDECAR back;
    CHECK( CABLY_EXPORT_SIDECAR::Load( f.dir.string(), back ) );
    CHECK( back.stem == sc.stem && back.files == sc.files );
    CHECK( back.meta.projectId == "p1" && back.meta.projectName == "Blinking LED" );
    CHECK( back.meta.cloudUpdatedAt == sc.meta.cloudUpdatedAt && back.meta.engineVersion == "abc123" );

    CHECK( !CABLY_EXPORT_SIDECAR::Load( ( f.dir / "nope" ).string(), back ) );
    writeFile( f.dir / ".cably-export.json", "{not json" );
    CHECK( !CABLY_EXPORT_SIDECAR::Load( f.dir.string(), back ) );

    // A pre-F5 sidecar (no meta) still loads; the meta is simply empty.
    writeFile( f.dir / ".cably-export.json", R"({"stem":"old","files":{"old.kicad_pcb":"00"},"engine":"cably"})" );
    CHECK( CABLY_EXPORT_SIDECAR::Load( f.dir.string(), back ) );
    CHECK( back.stem == "old" && back.files.size() == 1 && back.meta.projectId.empty() );
}


static void testCompareIso()
{
    // PostgREST renders timestamptz as 2026-09-03T10:00:00.123456+00:00; JS as ...Z.
    CHECK( CABLY_BRIDGE::CompareIsoTimestamps( "2026-09-03T10:00:00.123456+00:00", "2026-09-03T10:00:00.123Z" ) > 0 );
    CHECK( CABLY_BRIDGE::CompareIsoTimestamps( "2026-09-03T10:00:00+00:00", "2026-09-03T10:00:00.000Z" ) == 0 );
    CHECK( CABLY_BRIDGE::CompareIsoTimestamps( "2026-09-03T10:00:00Z", "2026-09-03T10:00:01Z" ) < 0 );
    CHECK( CABLY_BRIDGE::CompareIsoTimestamps( "2026-09-03T12:00:00+02:00", "2026-09-03T10:00:00Z" ) == 0 );
    CHECK( CABLY_BRIDGE::CompareIsoTimestamps( "2026-09-03T09:59:59.999999+00:00", "2026-09-03T10:00:00+00:00" ) < 0 );
    CHECK( CABLY_BRIDGE::CompareIsoTimestamps( "2026-09-04T00:00:00Z", "2026-09-03T23:59:59Z" ) > 0 );

    // Unparsable stamps compare as strings (never silently equal to a different one).
    CHECK( CABLY_BRIDGE::CompareIsoTimestamps( "garbage", "garbage" ) == 0 );
    CHECK( CABLY_BRIDGE::CompareIsoTimestamps( "garbage", "2026-09-03T10:00:00Z" ) != 0 );
}


// --------------------------------------------------------------------------------
// ImportProject: GET row -> POST /v1/import -> PATCH data.project (chat kept) -> new updated_at.
// --------------------------------------------------------------------------------
static const char* ROW_JSON = R"([{"data":{"schema":"cably-project-session-v2",
   "chat":[{"id":"m1","role":"user","content":"make a blinky","createdAt":1}],
   "project":{"project_name":"Blinking LED","schematic":{"nodes":[],"edges":[]},"pcb":{"tracks":[]}}},
   "updated_at":"2026-09-03T10:00:00.123456+00:00"}])";

static const char* IMPORT_REPLY = R"({"project":{"project_name":"Blinking LED","schematic":{"nodes":[],"edges":[]},
   "pcb":{"tracks":[{"id":"t1"}],"importedFrom":"kicad"}},
   "pcbReport":{"tracks":1,"unmapped":[]},"schReport":null,"engineVersion":"eng-7","timings":{"wallMs":5}})";


static void checkOnlyTheTwoHosts( const FAKE_HTTP& aHttp )
{
    for( const CABLY_HTTP_REQUEST& r : aHttp.requests )
    {
        bool supabase = startsWith( r.url, "https://supabase.test/" );
        bool engine = startsWith( r.url, "https://engine.test/" );
        CHECK( supabase || engine );
        CHECK( r.Header( "Authorization" ) == "Bearer access-1" );
        CHECK( ( r.Header( "apikey" ) == "pk-test" ) == supabase ); // apikey only to Supabase
    }
}


static void testImportProject()
{
    FAKE_HTTP                 http;
    CABLY_MEMORY_SECRET_STORE store;
    store.Save( testSession() );
    CABLY_BRIDGE bridge( http, store, testConfig() );
    CHECK( bridge.LoadSession() );

    const std::string pcbText = "(kicad_pcb (version 20240108) saved)";

    http.Enqueue( 200, ROW_JSON );
    http.Enqueue( 200, IMPORT_REPLY );
    http.Enqueue( 204, "" );
    http.Enqueue( 200, R"([{"updated_at":"2026-09-03T10:05:00.654321+00:00"}])" );

    CABLY_IMPORT_RESULT r;
    CHECK( bridge.ImportProject( "p1", &pcbText, nullptr, "2026-09-03T10:00:00.123456+00:00", r )
           == CABLY_IMPORT_OUTCOME::SYNCED );
    CHECK( r.outcome == CABLY_IMPORT_OUTCOME::SYNCED );
    CHECK( http.requests.size() == 4 );

    // 1. the row, with its version
    const CABLY_HTTP_REQUEST& get = http.requests[0];
    CHECK( get.method == "GET" );
    CHECK( get.url == "https://supabase.test/rest/v1/projects?id=eq.p1&select=data,updated_at" );
    CHECK( get.Header( "apikey" ) == "pk-test" );
    CHECK( get.Header( "Authorization" ) == "Bearer access-1" );

    // 2. the engine: exactly { apiVersion, project, kicadPcb } - no kicadSch key when absent
    const CABLY_HTTP_REQUEST& imp = http.requests[1];
    CHECK( imp.method == "POST" );
    CHECK( imp.url == "https://engine.test/v1/import" );
    CHECK( imp.Header( "Authorization" ) == "Bearer access-1" );
    CHECK( imp.Header( "apikey" ).empty() );
    CHECK( contains( imp.Header( "Content-Type" ), "application/json" ) );
    json impBody = json::parse( imp.body );
    CHECK( impBody.size() == 3 );
    CHECK( impBody["apiVersion"] == 1 );
    CHECK( impBody["project"] == json::parse( ROW_JSON )[0]["data"]["project"] );
    CHECK( impBody["kicadPcb"] == pcbText );
    CHECK( !impBody.contains( "kicadSch" ) );

    // 3. PATCH the row: data with ONLY .project replaced; chat byte-equal; Prefer: return=minimal
    const CABLY_HTTP_REQUEST& patch = http.requests[2];
    CHECK( patch.method == "PATCH" );
    CHECK( patch.url == "https://supabase.test/rest/v1/projects?id=eq.p1" );
    CHECK( patch.Header( "apikey" ) == "pk-test" );
    CHECK( patch.Header( "Authorization" ) == "Bearer access-1" );
    CHECK( contains( patch.Header( "Content-Type" ), "application/json" ) );
    CHECK( patch.Header( "Prefer" ) == "return=minimal" );
    json patchBody = json::parse( patch.body );
    CHECK( patchBody.size() == 1 && patchBody.contains( "data" ) );
    json original = json::parse( ROW_JSON )[0]["data"];
    json expected = original;
    expected["project"] = json::parse( IMPORT_REPLY )["project"];
    CHECK( patchBody["data"] == expected );
    CHECK( patchBody["data"]["chat"].dump() == original["chat"].dump() );
    CHECK( patchBody["data"]["schema"] == "cably-project-session-v2" );
    CHECK( patchBody["data"]["project"]["pcb"]["importedFrom"] == "kicad" );

    // 4. the trigger-bumped version (return=minimal carries none)
    const CABLY_HTTP_REQUEST& ver = http.requests[3];
    CHECK( ver.method == "GET" );
    CHECK( ver.url == "https://supabase.test/rest/v1/projects?id=eq.p1&select=updated_at" );

    CHECK( r.cloudUpdatedAt == "2026-09-03T10:00:00.123456+00:00" );
    CHECK( r.updatedAt == "2026-09-03T10:05:00.654321+00:00" );
    CHECK( r.engineVersion == "eng-7" );
    CHECK( json::parse( r.pcbReport ) == json::parse( R"({"tracks":1,"unmapped":[]})" ) );
    CHECK( r.schReport == "null" );
    CHECK( json::parse( r.project ) == json::parse( IMPORT_REPLY )["project"] );
    checkOnlyTheTwoHosts( http );

    // Schematic only -> kicadSch, no kicadPcb.
    http.requests.clear();
    const std::string schText = "(kicad_sch saved)";
    http.Enqueue( 200, ROW_JSON );
    http.Enqueue( 200, IMPORT_REPLY );
    http.Enqueue( 204, "" );
    http.Enqueue( 200, R"([{"updated_at":"2026-09-03T10:06:00+00:00"}])" );
    CHECK( bridge.ImportProject( "p1", nullptr, &schText, "", r ) == CABLY_IMPORT_OUTCOME::SYNCED );
    impBody = json::parse( http.requests[1].body );
    CHECK( impBody.size() == 3 && impBody["kicadSch"] == schText && !impBody.contains( "kicadPcb" ) );

    // Both at once.
    http.requests.clear();
    http.Enqueue( 200, ROW_JSON );
    http.Enqueue( 200, IMPORT_REPLY );
    http.Enqueue( 204, "" );
    http.Enqueue( 200, R"([{"updated_at":"2026-09-03T10:07:00+00:00"}])" );
    CHECK( bridge.ImportProject( "p1", &pcbText, &schText, "", r ) == CABLY_IMPORT_OUTCOME::SYNCED );
    impBody = json::parse( http.requests[1].body );
    CHECK( impBody.size() == 4 && impBody["kicadPcb"] == pcbText && impBody["kicadSch"] == schText );

    // Neither -> refused before any request.
    http.requests.clear();
    CHECK( bridge.ImportProject( "p1", nullptr, nullptr, "", r ) == CABLY_IMPORT_OUTCOME::FAILED );
    CHECK( http.requests.empty() );
    CHECK( !bridge.LastError().empty() );

    // Ids are percent-encoded in the filter.
    http.requests.clear();
    http.Enqueue( 200, "[]" );
    CHECK( bridge.ImportProject( "a b", &pcbText, nullptr, "", r ) == CABLY_IMPORT_OUTCOME::FAILED );
    CHECK( http.requests[0].url == "https://supabase.test/rest/v1/projects?id=eq.a%20b&select=data,updated_at" );
    CHECK( http.requests.size() == 1 );
}


static void testImportCloudChanged()
{
    FAKE_HTTP                 http;
    CABLY_MEMORY_SECRET_STORE store;
    store.Save( testSession() );
    CABLY_BRIDGE bridge( http, store, testConfig() );
    CHECK( bridge.LoadSession() );
    const std::string pcbText = "(kicad_pcb saved)";
    CABLY_IMPORT_RESULT r;

    // THE RULE: the row is newer than what we exported -> do not touch it.
    // (Mutation-tested: inverting the comparison fails here and in bridge.sh.)
    http.Enqueue( 200, ROW_JSON ); // row: ...10:00:00.123456+00:00
    CHECK( bridge.ImportProject( "p1", &pcbText, nullptr, "2026-09-03T09:00:00.000Z", r )
           == CABLY_IMPORT_OUTCOME::CLOUD_CHANGED );
    CHECK( r.outcome == CABLY_IMPORT_OUTCOME::CLOUD_CHANGED );
    CHECK( r.cloudUpdatedAt == "2026-09-03T10:00:00.123456+00:00" );
    CHECK( http.requests.size() == 1 ); // the GET only: no import, no PATCH
    CHECK( http.requests[0].method == "GET" );

    // Equal instants in the other notation are not a change.
    http.requests.clear();
    http.Enqueue( 200, ROW_JSON );
    http.Enqueue( 200, IMPORT_REPLY );
    http.Enqueue( 204, "" );
    http.Enqueue( 200, R"([{"updated_at":"2026-09-03T10:08:00+00:00"}])" );
    CHECK( bridge.ImportProject( "p1", &pcbText, nullptr, "2026-09-03T10:00:00.123456Z", r )
           == CABLY_IMPORT_OUTCOME::SYNCED );
    CHECK( http.requests.size() == 4 );

    // A row OLDER than our record (clock skew, a restored backup) is not "the cloud changed".
    http.requests.clear();
    http.Enqueue( 200, ROW_JSON );
    http.Enqueue( 200, IMPORT_REPLY );
    http.Enqueue( 204, "" );
    http.Enqueue( 200, R"([{"updated_at":"2026-09-03T10:09:00+00:00"}])" );
    CHECK( bridge.ImportProject( "p1", &pcbText, nullptr, "2026-09-03T11:00:00Z", r )
           == CABLY_IMPORT_OUTCOME::SYNCED );

    // An unparsable expected stamp that differs is treated as changed (never clobber on doubt).
    http.requests.clear();
    http.Enqueue( 200, ROW_JSON );
    CHECK( bridge.ImportProject( "p1", &pcbText, nullptr, "not-a-date", r ) == CABLY_IMPORT_OUTCOME::CLOUD_CHANGED );
    CHECK( http.requests.size() == 1 );

    // Engine failure -> FAILED, no PATCH.
    http.requests.clear();
    http.Enqueue( 200, ROW_JSON );
    http.Enqueue( 500, R"({"error":{"code":"engine_failed","message":"boom"}})" );
    CHECK( bridge.ImportProject( "p1", &pcbText, nullptr, "", r ) == CABLY_IMPORT_OUTCOME::FAILED );
    CHECK( http.requests.size() == 2 );
    CHECK( contains( bridge.LastError(), "boom" ) );

    // PATCH refused -> FAILED, reported.
    http.requests.clear();
    http.Enqueue( 200, ROW_JSON );
    http.Enqueue( 200, IMPORT_REPLY );
    http.Enqueue( 403, R"({"message":"row-level security"})" );
    CHECK( bridge.ImportProject( "p1", &pcbText, nullptr, "", r ) == CABLY_IMPORT_OUTCOME::FAILED );
    CHECK( http.requests.size() == 3 );
    CHECK( contains( bridge.LastError(), "row-level security" ) );

    // A legacy row whose data IS the project: the whole data becomes the new project.
    http.requests.clear();
    http.Enqueue( 200, R"([{"data":{"project_name":"Old","schematic":{"nodes":[],"edges":[]}},"updated_at":"2026-01-01T00:00:00+00:00"}])" );
    http.Enqueue( 200, IMPORT_REPLY );
    http.Enqueue( 204, "" );
    http.Enqueue( 200, R"([{"updated_at":"2026-09-03T10:10:00+00:00"}])" );
    CHECK( bridge.ImportProject( "p0", &pcbText, nullptr, "", r ) == CABLY_IMPORT_OUTCOME::SYNCED );
    CHECK( json::parse( http.requests[2].body )["data"] == json::parse( IMPORT_REPLY )["project"] );

    // Not signed in -> FAILED without a request.
    CABLY_MEMORY_SECRET_STORE empty;
    CABLY_BRIDGE              out( http, empty, testConfig() );
    http.requests.clear();
    CHECK( out.ImportProject( "p1", &pcbText, nullptr, "", r ) == CABLY_IMPORT_OUTCOME::FAILED );
    CHECK( http.requests.empty() );
}


// An expired bearer on the first call is refreshed once and the whole flow continues.
static void testImportRefreshesOn401()
{
    FAKE_HTTP                 http;
    CABLY_MEMORY_SECRET_STORE store;
    store.Save( testSession() );
    CABLY_BRIDGE bridge( http, store, testConfig() );
    CHECK( bridge.LoadSession() );
    const std::string pcbText = "(kicad_pcb saved)";
    CABLY_IMPORT_RESULT r;

    http.Enqueue( 401, R"({"message":"JWT expired"})" );
    http.Enqueue( 200, R"({"access_token":"access-2","refresh_token":"refresh-2","expires_in":3600})" );
    http.Enqueue( 200, ROW_JSON );
    http.Enqueue( 200, IMPORT_REPLY );
    http.Enqueue( 204, "" );
    http.Enqueue( 200, R"([{"updated_at":"2026-09-03T10:11:00+00:00"}])" );
    CHECK( bridge.ImportProject( "p1", &pcbText, nullptr, "", r ) == CABLY_IMPORT_OUTCOME::SYNCED );
    CHECK( http.requests.size() == 6 );
    CHECK( http.requests[1].url == "https://supabase.test/auth/v1/token?grant_type=refresh_token" );

    for( size_t i = 2; i < http.requests.size(); ++i )
        CHECK( http.requests[i].Header( "Authorization" ) == "Bearer access-2" );
}


// --------------------------------------------------------------------------------
// CablySyncSave: sidecar in -> ImportProject -> sidecar out.
// --------------------------------------------------------------------------------
static void testSyncSave()
{
    FOLDER                    f( "sync" );
    FAKE_HTTP                 http;
    CABLY_MEMORY_SECRET_STORE store;
    store.Save( testSession() );
    CABLY_BRIDGE bridge( http, store, testConfig() );
    CHECK( bridge.LoadSession() );

    CABLY_SAVE_EVENT ev;
    ev.kind = CABLY_SAVE_KIND::BOARD;
    ev.path = f.pcb.string();
    ev.text = "(kicad_pcb saved)";

    // No sidecar: not a project opened from Cably -> nothing sent.
    CABLY_SYNC_RESULT s = CablySyncSave( bridge, f.dir.string(), ev );
    CHECK( s.outcome == CABLY_SYNC_OUTCOME::NOT_CABLY_PROJECT );
    CHECK( http.requests.empty() );

    // A sidecar without a project id (pre-F5 export) is not syncable either.
    CABLY_EXPORT_SIDECAR sc;
    sc.stem = "B";
    sc.files["B.kicad_pcb"] = CABLY_BRIDGE::Sha256Hex( "(kicad_pcb v1)" );
    sc.files["B.kicad_sch"] = CABLY_BRIDGE::Sha256Hex( "(kicad_sch v1)" );
    CHECK( sc.Save( f.dir.string() ) );
    s = CablySyncSave( bridge, f.dir.string(), ev );
    CHECK( s.outcome == CABLY_SYNC_OUTCOME::NOT_CABLY_PROJECT );
    CHECK( http.requests.empty() );

    // The real thing.
    sc.meta.projectId = "p1";
    sc.meta.projectName = "Blinking LED";
    sc.meta.cloudUpdatedAt = "2026-09-03T10:00:00.123456+00:00";
    sc.meta.engineVersion = "eng-1";
    CHECK( sc.Save( f.dir.string() ) );

    http.Enqueue( 200, ROW_JSON );
    http.Enqueue( 200, IMPORT_REPLY );
    http.Enqueue( 204, "" );
    http.Enqueue( 200, R"([{"updated_at":"2026-09-03T10:05:00.654321+00:00"}])" );
    s = CablySyncSave( bridge, f.dir.string(), ev );
    CHECK( s.outcome == CABLY_SYNC_OUTCOME::SYNCED );
    CHECK( s.projectId == "p1" );
    CHECK( s.cloudUpdatedAt == "2026-09-03T10:05:00.654321+00:00" );
    CHECK( s.engineVersion == "eng-7" );
    CHECK( json::parse( s.pcbReport )["tracks"] == 1 );
    CHECK( http.requests.size() == 4 );
    CHECK( json::parse( http.requests[1].body )["kicadPcb"] == "(kicad_pcb saved)" );

    // The sidecar now carries the new version, the new baseline hash and the engine.
    CABLY_EXPORT_SIDECAR after;
    CHECK( CABLY_EXPORT_SIDECAR::Load( f.dir.string(), after ) );
    CHECK( after.meta.cloudUpdatedAt == "2026-09-03T10:05:00.654321+00:00" );
    CHECK( after.meta.engineVersion == "eng-7" );
    CHECK( after.files["B.kicad_pcb"] == CABLY_BRIDGE::Sha256Hex( "(kicad_pcb saved)" ) );
    CHECK( after.files["B.kicad_sch"] == CABLY_BRIDGE::Sha256Hex( "(kicad_sch v1)" ) ); // untouched
    CHECK( after.meta.projectId == "p1" && after.meta.projectName == "Blinking LED" && after.stem == "B" );

    // Cloud changed since: nothing patched, sidecar untouched, the row's version reported.
    http.requests.clear();
    http.Enqueue( 200, R"([{"data":{"schema":"cably-project-session-v2","chat":[],"project":{"project_name":"x","schematic":{"nodes":[],"edges":[]}}},
                            "updated_at":"2026-09-03T11:00:00+00:00"}])" );
    CABLY_SAVE_EVENT ev2 = ev;
    ev2.kind = CABLY_SAVE_KIND::SCHEMATIC;
    ev2.path = f.sch.string();
    ev2.text = "(kicad_sch saved)";
    s = CablySyncSave( bridge, f.dir.string(), ev2 );
    CHECK( s.outcome == CABLY_SYNC_OUTCOME::CLOUD_CHANGED );
    CHECK( s.cloudUpdatedAt == "2026-09-03T11:00:00+00:00" );
    CHECK( http.requests.size() == 1 );
    CABLY_EXPORT_SIDECAR again;
    CHECK( CABLY_EXPORT_SIDECAR::Load( f.dir.string(), again ) );
    CHECK( again.meta.cloudUpdatedAt == "2026-09-03T10:05:00.654321+00:00" );
    CHECK( again.files["B.kicad_sch"] == CABLY_BRIDGE::Sha256Hex( "(kicad_sch v1)" ) );

    // A failure is reported with the bridge's message; sidecar untouched.
    http.requests.clear();
    http.Enqueue( 200, ROW_JSON );
    http.Enqueue( 502, R"({"error":{"code":"engine_failed","message":"down"}})" );
    s = CablySyncSave( bridge, f.dir.string(), ev );
    CHECK( s.outcome == CABLY_SYNC_OUTCOME::FAILED );
    CHECK( contains( s.error, "down" ) );
    checkOnlyTheTwoHosts( http );
}


int main()
{
    testClassify();
    testWatchInPlace();
    testWatchBurst();
    testWatchAtomicRename();
    testWatchIgnoresBackupsAndLocks();
    testWatchExpectOwnWrite();
    testWatchSameBytes();
    testWatchStop();
    testWatchBaselineFromSidecar();
    testSidecarRoundTrip();
    testCompareIso();
    testImportProject();
    testImportCloudChanged();
    testImportRefreshesOn401();
    testSyncSave();

    std::printf( "test_cably_sync: %d checks passed\n", g_checks );
    return 0;
}

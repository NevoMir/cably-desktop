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
 * Standalone unit test for the F4 cloud bridge (cably/src/cably_bridge.{h,cpp}).
 * Written BEFORE the bridge existed. Built and run by cably/tests/bridge.sh with
 * plain clang++: the bridge core is pure C++17 (nlohmann/json + picosha2 headers,
 * POSIX sockets), no wxWidgets, no KiCad libraries, NO real network — every HTTP
 * call goes through a recording fake and the loopback handler is driven directly.
 */

#include <cably_bridge.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

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


/// Records every request and answers from a queue (or a fixed default).
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


static std::string readFile( const fs::path& aPath )
{
    std::ifstream in( aPath, std::ios::binary );
    return std::string( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
}


static void writeFile( const fs::path& aPath, const std::string& aText )
{
    std::ofstream out( aPath, std::ios::binary );
    out << aText;
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
    s.expiresAt = 4102444800LL; // 2100-01-01
    return s;
}


// --------------------------------------------------------------------------------
// Loopback handoff: state check, single acceptance, callback page.
// --------------------------------------------------------------------------------
static void testLoopbackHandler()
{
    CABLY_LOOPBACK_SERVER server;
    const std::string      state = server.State();
    CHECK( state.size() == 64 ); // 32 random bytes as hex

    for( char c : state )
        CHECK( ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) );

    // Two servers never share a state.
    CABLY_LOOPBACK_SERVER other;
    CHECK( other.State() != state );

    std::string body, type;

    // /callback serves the page whose script reads location.hash and POSTs it to /token.
    int status = server.HandleRequest( "GET", "/callback", "", body, type );
    CHECK( status == 200 );
    CHECK( contains( type, "text/html" ) );
    CHECK( contains( body, "location.hash" ) );
    CHECK( contains( body, "'/token'" ) );
    CHECK( contains( body, "method: 'POST'" ) || contains( body, "method:'POST'" ) );
    CHECK( contains( body, "Connected" ) );
    CHECK( contains( body, "Cably Desktop" ) );
    CHECK( body == CABLY_LOOPBACK_SERVER::CallbackHtml() );

    // Unknown path -> 404; wrong method on a known path -> 4xx.
    CHECK( server.HandleRequest( "GET", "/nope", "", body, type ) == 404 );
    CHECK( server.HandleRequest( "GET", "/token", "", body, type ) >= 400 );
    CHECK( server.HandleRequest( "POST", "/callback", "{}", body, type ) >= 400 );

    auto tokenBody = [&]( const std::string& aState )
    {
        json j;
        j["state"] = aState;
        j["access_token"] = "access-1";
        j["refresh_token"] = "refresh-1";
        j["expires_at"] = 4102444800LL;
        j["email"] = "dev@example.com";
        return j.dump();
    };

    // State mismatch -> rejected, nothing accepted.
    CHECK( server.HandleRequest( "POST", "/token", tokenBody( "0000" ), body, type ) == 400 );
    CHECK( !server.Accepted() );

    // The other server's (valid-looking) state is not ours either.
    CHECK( server.HandleRequest( "POST", "/token", tokenBody( other.State() ), body, type ) == 400 );
    CHECK( !server.Accepted() );

    // Not JSON / missing fields -> 400.
    CHECK( server.HandleRequest( "POST", "/token", "not json", body, type ) == 400 );
    CHECK( server.HandleRequest( "POST", "/token", "{\"state\":\"" + state + "\"}", body, type )
           == 400 );
    CHECK( !server.Accepted() );

    // Correct state -> accepted exactly once.
    CHECK( server.HandleRequest( "POST", "/token", tokenBody( state ), body, type ) == 200 );
    CHECK( server.Accepted() );

    CABLY_SESSION got;
    CHECK( server.WaitForSession( 0, got ) );
    CHECK( got.accessToken == "access-1" );
    CHECK( got.refreshToken == "refresh-1" );
    CHECK( got.email == "dev@example.com" );
    CHECK( got.expiresAt == 4102444800LL );

    // A replay with the same (now consumed) state is refused.
    CHECK( server.HandleRequest( "POST", "/token", tokenBody( state ), body, type ) != 200 );

    // expires_at may arrive as a numeric string (URLSearchParams gives strings).
    CABLY_LOOPBACK_SERVER third;
    json                  j;
    j["state"] = third.State();
    j["access_token"] = "a";
    j["refresh_token"] = "r";
    j["expires_at"] = "1700000000";
    CHECK( third.HandleRequest( "POST", "/token", j.dump(), body, type ) == 200 );
    CHECK( third.WaitForSession( 0, got ) && got.expiresAt == 1700000000LL );
}


// A real socket smoke: Start() binds an ephemeral port on 127.0.0.1 and Stop() returns.
static void testLoopbackSocket()
{
    CABLY_LOOPBACK_SERVER server;
    std::string           err;
    CHECK( server.Start( &err ) );
    CHECK( server.Port() > 0 && server.Port() < 65536 );
    CHECK( server.IsListening() );

    CABLY_SESSION none;
    CHECK( !server.WaitForSession( 10, none ) ); // nothing arrived, times out cleanly
    server.Stop();
    CHECK( !server.IsListening() );
}


static void testAuthUrl()
{
    std::string url = CABLY_BRIDGE::BuildAuthUrl( "https://cably.test/desktop/auth", 51234,
                                                  "abc123" );
    CHECK( url == "https://cably.test/desktop/auth?port=51234&state=abc123" );

    // Reserved characters are percent-encoded so the page reads the state back verbatim.
    std::string odd = CABLY_BRIDGE::BuildAuthUrl( "https://cably.test/desktop/auth", 80,
                                                  "a b&c=d/e?" );
    CHECK( odd == "https://cably.test/desktop/auth?port=80&state=a%20b%26c%3Dd%2Fe%3F" );

    CHECK( CABLY_BRIDGE::UrlEncode( "AZaz09-_.~" ) == "AZaz09-_.~" );
    CHECK( CABLY_BRIDGE::UrlEncode( "\xC3\xA9" ) == "%C3%A9" );
}


static void testSessionJsonRoundTrip()
{
    CABLY_SESSION s = testSession();
    std::string   text = CablySessionToJson( s );
    CABLY_SESSION back;
    CHECK( CablySessionFromJson( text, back ) );
    CHECK( back.accessToken == s.accessToken && back.refreshToken == s.refreshToken );
    CHECK( back.email == s.email && back.expiresAt == s.expiresAt );
    CHECK( !CablySessionFromJson( "garbage", back ) );
    CHECK( !CablySessionFromJson( "{\"access_token\":\"\"}", back ) );

    CABLY_MEMORY_SECRET_STORE store;
    CABLY_SESSION             loaded;
    CHECK( !store.Load( loaded ) );
    CHECK( store.Save( s ) );
    CHECK( store.Load( loaded ) && loaded.accessToken == "access-1" );
    CHECK( store.Clear() );
    CHECK( !store.Load( loaded ) );
}


// --------------------------------------------------------------------------------
// HTTP calls: exact URLs, header names, bodies.
// --------------------------------------------------------------------------------
static void testRefresh()
{
    FAKE_HTTP                 http;
    CABLY_MEMORY_SECRET_STORE store;
    store.Save( testSession() );
    CABLY_BRIDGE bridge( http, store, testConfig() );
    CHECK( bridge.LoadSession() );

    http.Enqueue( 200, R"({"access_token":"access-2","refresh_token":"refresh-2",
                           "expires_in":3600,"expires_at":4102448400,
                           "user":{"email":"dev@example.com"}})" );
    CHECK( bridge.RefreshSession() );
    CHECK( http.requests.size() == 1 );

    const CABLY_HTTP_REQUEST& r = http.requests[0];
    CHECK( r.method == "POST" );
    CHECK( r.url == "https://supabase.test/auth/v1/token?grant_type=refresh_token" );
    CHECK( r.Header( "apikey" ) == "pk-test" );
    CHECK( contains( r.Header( "Content-Type" ), "application/json" ) );
    CHECK( r.Header( "Authorization" ).empty() ); // the refresh token IS the credential
    CHECK( json::parse( r.body ) == json::parse( R"({"refresh_token":"refresh-1"})" ) );

    // The new session replaced the old one, in memory and in the store.
    CHECK( bridge.Session().accessToken == "access-2" );
    CHECK( bridge.Session().refreshToken == "refresh-2" );
    CHECK( bridge.Session().expiresAt == 4102448400LL );
    CABLY_SESSION stored;
    CHECK( store.Load( stored ) && stored.accessToken == "access-2" );

    // A refused refresh (revoked token) fails and clears the stored session.
    http.Enqueue( 400, R"({"error":"invalid_grant"})" );
    CHECK( !bridge.RefreshSession() );
    CHECK( !bridge.HasSession() );
    CHECK( !store.Load( stored ) );

    // expires_in alone is enough (expires_at is derived from now).
    store.Save( testSession() );
    CHECK( bridge.LoadSession() );
    http.Enqueue( 200, R"({"access_token":"access-3","refresh_token":"refresh-3","expires_in":3600})" );
    CHECK( bridge.RefreshSession() );
    CHECK( bridge.Session().expiresAt > 1700000000LL );
    CHECK( bridge.Session().email == "dev@example.com" ); // kept from the old session
}


static void testValidate()
{
    FAKE_HTTP                 http;
    CABLY_MEMORY_SECRET_STORE store;
    store.Save( testSession() );
    CABLY_BRIDGE bridge( http, store, testConfig() );
    CHECK( bridge.LoadSession() );

    http.Enqueue( 200, R"({"id":"u1","email":"dev@example.com"})" );
    CHECK( bridge.ValidateSession() );
    CHECK( http.requests.size() == 1 );
    CHECK( http.requests[0].method == "GET" );
    CHECK( http.requests[0].url == "https://supabase.test/auth/v1/user" );
    CHECK( http.requests[0].Header( "apikey" ) == "pk-test" );
    CHECK( http.requests[0].Header( "Authorization" ) == "Bearer access-1" );

    // 401 -> refresh once -> retry with the new bearer.
    http.Enqueue( 401, R"({"message":"expired"})" );
    http.Enqueue( 200, R"({"access_token":"access-2","refresh_token":"refresh-2","expires_in":3600})" );
    http.Enqueue( 200, R"({"id":"u1","email":"dev@example.com"})" );
    CHECK( bridge.ValidateSession() );
    CHECK( http.requests.size() == 4 );
    CHECK( http.requests[2].url == "https://supabase.test/auth/v1/token?grant_type=refresh_token" );
    CHECK( http.requests[3].Header( "Authorization" ) == "Bearer access-2" );

    // 401 and the refresh fails -> false, session cleared, no retry.
    http.Enqueue( 401, "{}" );
    http.Enqueue( 401, "{}" );
    CHECK( !bridge.ValidateSession() );
    CHECK( http.requests.size() == 6 );
    CHECK( !bridge.HasSession() );

    // No session at all -> false without any request.
    CHECK( !bridge.ValidateSession() );
    CHECK( http.requests.size() == 6 );
}


static void testListAndFetch()
{
    FAKE_HTTP                 http;
    CABLY_MEMORY_SECRET_STORE store;
    store.Save( testSession() );
    CABLY_BRIDGE bridge( http, store, testConfig() );
    CHECK( bridge.LoadSession() );

    http.Enqueue( 200, R"([{"id":"p2","name":"Radio","updated_at":"2026-09-03T10:00:00Z"},
                           {"id":"p1","name":"Blinking LED","updated_at":"2026-09-01T10:00:00Z"}])" );
    std::vector<CABLY_PROJECT_SUMMARY> list;
    CHECK( bridge.ListProjects( list ) );
    CHECK( list.size() == 2 );
    CHECK( list[0].id == "p2" && list[0].name == "Radio" && list[0].updatedAt == "2026-09-03T10:00:00Z" );
    CHECK( list[1].id == "p1" && list[1].name == "Blinking LED" );

    const CABLY_HTTP_REQUEST& r = http.requests[0];
    CHECK( r.method == "GET" );
    CHECK( r.url == "https://supabase.test/rest/v1/projects?select=id,name,updated_at&order=updated_at.desc&limit=50" );
    CHECK( r.Header( "apikey" ) == "pk-test" );
    CHECK( r.Header( "Authorization" ) == "Bearer access-1" );
    CHECK( r.body.empty() );

    // FetchProject unwraps data.project (PersistedProjectSession).
    http.Enqueue( 200, R"([{"data":{"schema":"cably-project-session-v2","chat":[],
                            "project":{"project_name":"Blinking LED","schematic":{"nodes":[],"edges":[]}}}}])" );
    std::string project;
    CHECK( bridge.FetchProject( "p1", project ) );
    CHECK( json::parse( project ) == json::parse( R"({"project_name":"Blinking LED","schematic":{"nodes":[],"edges":[]}})" ) );
    CHECK( http.requests[1].url == "https://supabase.test/rest/v1/projects?id=eq.p1&select=data" );
    CHECK( http.requests[1].Header( "apikey" ) == "pk-test" );
    CHECK( http.requests[1].Header( "Authorization" ) == "Bearer access-1" );

    // A legacy row whose data IS the project.
    http.Enqueue( 200, R"([{"data":{"project_name":"Old","schematic":{"nodes":[],"edges":[]}}}])" );
    CHECK( bridge.FetchProject( "p0", project ) );
    CHECK( json::parse( project )["project_name"] == "Old" );

    // Ids are percent-encoded in the filter; no row -> failure.
    http.Enqueue( 200, "[]" );
    CHECK( !bridge.FetchProject( "a b", project ) );
    CHECK( http.requests[3].url == "https://supabase.test/rest/v1/projects?id=eq.a%20b&select=data" );
    CHECK( !bridge.LastError().empty() );

    // F5: asking for the row's version selects it too (the sidecar records it at export).
    http.Enqueue( 200, R"([{"data":{"schema":"cably-project-session-v2","chat":[],
                            "project":{"project_name":"Blinking LED","schematic":{"nodes":[],"edges":[]}}},
                            "updated_at":"2026-09-03T10:00:00.123456+00:00"}])" );
    std::string updatedAt;
    CHECK( bridge.FetchProject( "p1", project, &updatedAt ) );
    CHECK( updatedAt == "2026-09-03T10:00:00.123456+00:00" );
    CHECK( json::parse( project )["project_name"] == "Blinking LED" );
    CHECK( http.requests[4].url == "https://supabase.test/rest/v1/projects?id=eq.p1&select=data,updated_at" );
}


static void testExport()
{
    FAKE_HTTP                 http;
    CABLY_MEMORY_SECRET_STORE store;
    store.Save( testSession() );
    CABLY_BRIDGE bridge( http, store, testConfig() );
    CHECK( bridge.LoadSession() );

    const std::string project = R"({"project_name":"Blinking LED","schematic":{"nodes":[],"edges":[]},"pcb":{"tracks":[]}})";
    http.Enqueue( 200, R"json({"kicadPcb":"(kicad_pcb (version 20240108))","kicadSch":"(kicad_sch (version 20231120))",
                           "schematic":{"source":"generated","unmapped":[],"unresolvedPins":0},
                           "engineVersion":"abc123","timings":{"wallMs":12}})json" );
    CABLY_EXPORT_RESULT out;
    CHECK( bridge.ExportProject( project, out ) );
    CHECK( out.hasPcb && out.kicadPcb == "(kicad_pcb (version 20240108))" );
    CHECK( out.hasSch && out.kicadSch == "(kicad_sch (version 20231120))" );
    CHECK( out.engineVersion == "abc123" );

    const CABLY_HTTP_REQUEST& r = http.requests[0];
    CHECK( r.method == "POST" );
    CHECK( r.url == "https://engine.test/v1/export" );
    CHECK( r.Header( "Authorization" ) == "Bearer access-1" );
    CHECK( contains( r.Header( "Content-Type" ), "application/json" ) );
    CHECK( r.Header( "apikey" ).empty() ); // the engine is not Supabase

    // The body is EXACTLY the contract: { apiVersion: 1, project } and nothing else.
    json body = json::parse( r.body );
    json expected;
    expected["apiVersion"] = 1;
    expected["project"] = json::parse( project );
    CHECK( body == expected );
    CHECK( body.size() == 2 );

    // null members are honoured.
    http.Enqueue( 200, R"({"kicadPcb":null,"kicadSch":null,"schematic":null,"engineVersion":"x","timings":{"wallMs":1}})" );
    CHECK( bridge.ExportProject( project, out ) );
    CHECK( !out.hasPcb && !out.hasSch );

    // Engine errors surface as failures with the engine's message.
    http.Enqueue( 401, R"({"error":{"code":"unauthorized","message":"Sign in"}})" );
    http.Enqueue( 401, R"({"error":"invalid_grant"})" ); // the refresh attempt fails too
    CHECK( !bridge.ExportProject( project, out ) );
    CHECK( contains( bridge.LastError(), "Sign in" ) || contains( bridge.LastError(), "unauthorized" ) );

    // Malformed project JSON is refused before any request.
    size_t before = http.requests.size();
    CHECK( !bridge.ExportProject( "{not json", out ) );
    CHECK( http.requests.size() == before );
}


// --------------------------------------------------------------------------------
// WriteProjectFolder: three files + sidecar; never clobber an edited file.
// --------------------------------------------------------------------------------
static void testWriteProjectFolder()
{
    fs::path root = fs::temp_directory_path() / ( "cably-bridge-test-" + std::to_string( ::getpid() ) );
    fs::remove_all( root );

    const std::string pcb1 = "(kicad_pcb v1)";
    const std::string sch1 = "(kicad_sch v1)";

    CABLY_WRITE_RESULT r = CABLY_BRIDGE::WriteProjectFolder( root.string(), "Blinking LED!", pcb1, sch1 );
    CHECK( r.written );
    CHECK( r.conflicts.empty() );
    CHECK( r.dir == ( root / "Blinking_LED" ).string() );
    CHECK( r.proPath == ( root / "Blinking_LED" / "Blinking_LED.kicad_pro" ).string() );
    CHECK( r.pcbPath == ( root / "Blinking_LED" / "Blinking_LED.kicad_pcb" ).string() );
    CHECK( r.schPath == ( root / "Blinking_LED" / "Blinking_LED.kicad_sch" ).string() );
    CHECK( readFile( r.pcbPath ) == pcb1 );
    CHECK( readFile( r.schPath ) == sch1 );

    json pro = json::parse( readFile( r.proPath ) );
    CHECK( pro["meta"]["filename"] == "Blinking_LED.kicad_pro" );
    CHECK( pro["meta"]["version"] == 3 );

    json sidecar = json::parse( readFile( fs::path( r.dir ) / ".cably-export.json" ) );
    CHECK( sidecar["stem"] == "Blinking_LED" );
    CHECK( sidecar["files"]["Blinking_LED.kicad_pcb"].is_string() );
    CHECK( sidecar["files"]["Blinking_LED.kicad_sch"].is_string() );
    CHECK( sidecar["files"]["Blinking_LED.kicad_pcb"].get<std::string>().size() == 64 );

    // KiCad rewrote the project file: we never touch it again.
    writeFile( r.proPath, "{\"meta\":{\"filename\":\"Blinking_LED.kicad_pro\",\"version\":3},\"board\":{}}" );

    // Unedited files are overwritten by a newer export.
    const std::string pcb2 = "(kicad_pcb v2)";
    r = CABLY_BRIDGE::WriteProjectFolder( root.string(), "Blinking LED!", pcb2, sch1 );
    CHECK( r.written );
    CHECK( readFile( r.pcbPath ) == pcb2 );
    CHECK( readFile( r.schPath ) == sch1 );
    CHECK( contains( readFile( r.proPath ), "\"board\"" ) ); // .kicad_pro untouched

    // The user edited the board in KiCad: a new export must NOT clobber it.
    const std::string edited = "(kicad_pcb v2 edited-by-user)";
    writeFile( r.pcbPath, edited );
    const std::string pcb3 = "(kicad_pcb v3)";
    r = CABLY_BRIDGE::WriteProjectFolder( root.string(), "Blinking LED!", pcb3, sch1 );
    CHECK( !r.written );
    CHECK( r.conflicts.size() == 1 );
    CHECK( r.conflicts[0] == r.pcbPath );
    CHECK( readFile( r.pcbPath ) == edited ); // nothing touched
    CHECK( readFile( r.schPath ) == sch1 );

    // Re-exporting the SAME text as the edited file is not a conflict (already what we want).
    r = CABLY_BRIDGE::WriteProjectFolder( root.string(), "Blinking LED!", edited, sch1 );
    CHECK( r.written );
    CHECK( readFile( r.pcbPath ) == edited );

    // Now the baseline is `edited`; editing again and forcing overwrites.
    writeFile( r.pcbPath, "(kicad_pcb hand-edited-2)" );
    r = CABLY_BRIDGE::WriteProjectFolder( root.string(), "Blinking LED!", pcb3, sch1 );
    CHECK( !r.written && r.conflicts.size() == 1 );
    r = CABLY_BRIDGE::WriteProjectFolder( root.string(), "Blinking LED!", pcb3, sch1, true );
    CHECK( r.written );
    CHECK( readFile( r.pcbPath ) == pcb3 );

    // A file of unknown provenance (no baseline in the sidecar) is the user's: conflict.
    fs::path otherRoot = root / "other";
    fs::create_directories( otherRoot / "Amp" );
    writeFile( otherRoot / "Amp" / "Amp.kicad_pcb", "(kicad_pcb someone-elses)" );
    r = CABLY_BRIDGE::WriteProjectFolder( otherRoot.string(), "Amp", pcb1, sch1 );
    CHECK( !r.written && r.conflicts.size() == 1 );
    CHECK( readFile( otherRoot / "Amp" / "Amp.kicad_pcb" ) == "(kicad_pcb someone-elses)" );
    CHECK( !fs::exists( otherRoot / "Amp" / "Amp.kicad_sch" ) ); // nothing written on conflict

    // The stem recorded in the sidecar wins over a renamed project (KiCad owns the .kicad_pro).
    r = CABLY_BRIDGE::WriteProjectFolder( root.string(), "Blinking LED!", pcb3, sch1 );
    CHECK( r.written );
    json sc = json::parse( readFile( fs::path( r.dir ) / ".cably-export.json" ) );
    sc["stem"] = "legacy";
    writeFile( fs::path( r.dir ) / ".cably-export.json", sc.dump() );
    r = CABLY_BRIDGE::WriteProjectFolder( root.string(), "Blinking LED!", pcb3, sch1 );
    CHECK( r.written );
    CHECK( r.pcbPath == ( root / "Blinking_LED" / "legacy.kicad_pcb" ).string() );
    CHECK( fs::exists( root / "Blinking_LED" / "legacy.kicad_pro" ) );

    // No schematic -> only two files.
    r = CABLY_BRIDGE::WriteProjectFolder( root.string(), "PcbOnly", pcb1, "" );
    CHECK( r.written && r.schPath.empty() );
    CHECK( !fs::exists( root / "PcbOnly" / "PcbOnly.kicad_sch" ) );

    // F5: the export records which cloud row it came from, so a later KiCad save can be
    // synced back (projectId, projectName, cloudUpdatedAt, engineVersion in the sidecar).
    CABLY_EXPORT_META meta;
    meta.projectId = "p1";
    meta.projectName = "Blinking LED!";
    meta.cloudUpdatedAt = "2026-09-03T10:00:00.123456+00:00";
    meta.engineVersion = "eng-1";
    r = CABLY_BRIDGE::WriteProjectFolder( root.string(), "With Meta", pcb1, sch1, false, &meta );
    CHECK( r.written );
    sc = json::parse( readFile( fs::path( r.dir ) / ".cably-export.json" ) );
    CHECK( sc["stem"] == "With_Meta" );
    CHECK( sc["engine"] == "cably" );
    CHECK( sc["projectId"] == "p1" );
    CHECK( sc["projectName"] == "Blinking LED!" );
    CHECK( sc["cloudUpdatedAt"] == "2026-09-03T10:00:00.123456+00:00" );
    CHECK( sc["engineVersion"] == "eng-1" );
    CHECK( sc["files"]["With_Meta.kicad_pcb"] == CABLY_BRIDGE::Sha256Hex( pcb1 ) );

    // A re-export without meta keeps what the sidecar knew; with meta, it is refreshed.
    r = CABLY_BRIDGE::WriteProjectFolder( root.string(), "With Meta", pcb2, sch1 );
    CHECK( r.written );
    sc = json::parse( readFile( fs::path( r.dir ) / ".cably-export.json" ) );
    CHECK( sc["projectId"] == "p1" && sc["cloudUpdatedAt"] == "2026-09-03T10:00:00.123456+00:00" );
    CHECK( sc["files"]["With_Meta.kicad_pcb"] == CABLY_BRIDGE::Sha256Hex( pcb2 ) );
    meta.cloudUpdatedAt = "2026-09-03T12:00:00+00:00";
    meta.engineVersion = "eng-2";
    r = CABLY_BRIDGE::WriteProjectFolder( root.string(), "With Meta", pcb2, sch1, false, &meta );
    CHECK( r.written );
    sc = json::parse( readFile( fs::path( r.dir ) / ".cably-export.json" ) );
    CHECK( sc["cloudUpdatedAt"] == "2026-09-03T12:00:00+00:00" && sc["engineVersion"] == "eng-2" );

    // The clobber rule is unchanged by the meta: an edited board still refuses.
    writeFile( r.pcbPath, "(kicad_pcb edited-after-meta)" );
    r = CABLY_BRIDGE::WriteProjectFolder( root.string(), "With Meta", pcb3, sch1, false, &meta );
    CHECK( !r.written && r.conflicts.size() == 1 );
    CHECK( readFile( r.pcbPath ) == "(kicad_pcb edited-after-meta)" );

    // The typed sidecar reader sees the same thing.
    CABLY_EXPORT_SIDECAR typed;
    CHECK( CABLY_EXPORT_SIDECAR::Load( r.dir, typed ) );
    CHECK( typed.stem == "With_Meta" && typed.meta.projectId == "p1" && typed.meta.engineVersion == "eng-2" );
    CHECK( typed.files.at( "With_Meta.kicad_sch" ) == CABLY_BRIDGE::Sha256Hex( sch1 ) );

    CHECK( CABLY_BRIDGE::Sha256Hex( "" ) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" );

    // Stems: no dots, bounded, never empty, path separators neutralised.
    CHECK( CABLY_BRIDGE::SafeStem( "My.Board v2" ) == "My_Board_v2" );
    CHECK( CABLY_BRIDGE::SafeStem( "../../etc/passwd" ) == "etc_passwd" );
    CHECK( CABLY_BRIDGE::SafeStem( "!!!" ) == "cably-project" );
    CHECK( CABLY_BRIDGE::SafeStem( "" ) == "cably-project" );
    CHECK( CABLY_BRIDGE::SafeStem( std::string( 100, 'a' ) ).size() == 64 );

    fs::remove_all( root );
}


int main()
{
    testLoopbackHandler();
    testLoopbackSocket();
    testAuthUrl();
    testSessionJsonRoundTrip();
    testRefresh();
    testValidate();
    testListAndFetch();
    testExport();
    testWriteProjectFolder();

    std::printf( "test_cably_bridge: %d checks passed\n", g_checks );
    return 0;
}

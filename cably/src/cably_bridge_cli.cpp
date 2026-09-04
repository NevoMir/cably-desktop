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
 * cably-bridge-cli: drives the F4 cloud bridge from a shell so cably/tests/bridge.sh can
 * exercise the real KICAD_CURL_EASY transport, the real loopback listener and the real
 * keychain against a mock cloud.  Not shipped in the app bundle (EXCLUDE_FROM_ALL).
 *
 *   cably-bridge-cli [options] <command> [arg]
 *
 *   --supabase-url URL      default CABLY_SUPABASE_URL
 *   --engine-url URL        default CABLY_ENGINE_URL
 *   --auth-url URL          default CABLY_DESKTOP_AUTH_URL
 *   --publishable-key KEY   default CABLY_SUPABASE_PUBLISHABLE_KEY
 *   --store memory|keychain default memory
 *   --keychain-service NAME default CABLY_KEYCHAIN_SERVICE
 *   --token T --refresh-token R --email E --expires-at N   seed the store before the command
 *   --root DIR              where `open` writes (default ~/Documents/Cably Desktop)
 *   --timeout SECONDS       `loopback` wait (default 300)
 *   --force                 `open` overwrites edited files
 *   --pcb FILE --sch FILE   `import`: the KiCad files to send (at least one)
 *   --expect-updated-at T   `import`: the version we exported (empty = no cloud-changed check)
 *   --debounce MS --poll MS `watch` (default 500 / 250)
 *   --watch-timeout SECONDS `watch`: give up after this (default 300)
 *   --once                  `watch`: exit after the first event
 *   --self-write PATH       `watch`: after starting, write PATH through Expect() (must not echo)
 *
 *   loopback     print port=, state=, auth_url=; wait for the handoff; print email=
 *   validate     GET /auth/v1/user (refreshes on 401); print email=
 *   refresh      POST /auth/v1/token?grant_type=refresh_token; print access_token_len=, expires_at=
 *   list         print the projects as JSON
 *   fetch <id>   print the project JSON
 *   open <id>    fetch -> export -> write folder (sidecar records id/name/updated_at/engine);
 *                print the paths as JSON (exit 2 on conflict)
 *   import <id>  F5: GET row -> POST /v1/import -> PATCH; print outcome= updated_at= ...
 *                (exit 3 on cloud-changed)
 *   watch <dir>  F5: watch an exported folder; every save is synced through CablySyncSave;
 *                print watching=, one `event ...` line per save, events=, exit=
 *                (exit 3 when the timeout passed with no event)
 *   save         store the seeded session;  show  print the stored session;  signout  clear it
 *                (all three print store_backend= for --store keychain: keychain,
 *                secret-service or file:<path>)
 */

// kicad_curl headers must precede any wxWidgets header.
#include <kicad_curl/kicad_curl.h>

#include <cably_bridge.h>
#include <cably_config.h>
#include <cably_sync.h>

#include <nlohmann/json.hpp>

#include <wx/init.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;


static int usage()
{
    std::fprintf( stderr, "usage: cably-bridge-cli [options] loopback|validate|refresh|list|fetch <id>|open <id>|import <id>|watch <dir>|save|show|signout\n" );
    return 64;
}


static bool readWholeFile( const std::string& aPath, std::string& aOut )
{
    std::ifstream in( aPath, std::ios::binary );

    if( !in )
        return false;

    aOut.assign( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
    return true;
}


static const char* outcomeName( CABLY_SYNC_OUTCOME aOutcome )
{
    switch( aOutcome )
    {
    case CABLY_SYNC_OUTCOME::SYNCED: return "synced";
    case CABLY_SYNC_OUTCOME::CLOUD_CHANGED: return "cloud-changed";
    case CABLY_SYNC_OUTCOME::NOT_CABLY_PROJECT: return "not-cably";
    case CABLY_SYNC_OUTCOME::FAILED: break;
    }

    return "failed";
}


static std::string homeRoot()
{
    const char* home = std::getenv( "HOME" );
    return std::string( home ? home : "." ) + "/Documents/Cably Desktop";
}


int main( int argc, char** argv )
{
    CABLY_BRIDGE_CONFIG config = CABLY_BRIDGE_CONFIG::Default();
    std::string         storeKind = "memory";
    std::string         keychainService = CABLY_KEYCHAIN_SERVICE;
    std::string         root = homeRoot();
    CABLY_SESSION       seed;
    bool                haveSeed = false;
    bool                force = false;
    int                 timeoutSecs = 300;
    std::string         pcbFile, schFile, expectUpdatedAt, selfWrite;
    CABLY_WATCH_OPTIONS watchOptions;
    int                 watchTimeoutSecs = 300;
    bool                once = false;
    std::vector<std::string> positional;

    for( int i = 1; i < argc; ++i )
    {
        std::string a = argv[i];
        auto        next = [&]() -> std::string
        {
            if( i + 1 >= argc )
            {
                std::fprintf( stderr, "%s needs a value\n", a.c_str() );
                std::exit( 64 );
            }

            return argv[++i];
        };

        if( a == "--supabase-url" )         config.supabaseUrl = next();
        else if( a == "--engine-url" )      config.engineUrl = next();
        else if( a == "--auth-url" )        config.authPageUrl = next();
        else if( a == "--publishable-key" ) config.publishableKey = next();
        else if( a == "--store" )           storeKind = next();
        else if( a == "--keychain-service" ) keychainService = next();
        else if( a == "--token" )           { seed.accessToken = next(); haveSeed = true; }
        else if( a == "--refresh-token" )   { seed.refreshToken = next(); haveSeed = true; }
        else if( a == "--email" )           seed.email = next();
        else if( a == "--expires-at" )      seed.expiresAt = std::atoll( next().c_str() );
        else if( a == "--root" )            root = next();
        else if( a == "--timeout" )         timeoutSecs = std::atoi( next().c_str() );
        else if( a == "--force" )           force = true;
        else if( a == "--pcb" )             pcbFile = next();
        else if( a == "--sch" )             schFile = next();
        else if( a == "--expect-updated-at" ) expectUpdatedAt = next();
        else if( a == "--debounce" )        watchOptions.debounceMs = std::atoi( next().c_str() );
        else if( a == "--poll" )            watchOptions.pollMs = std::atoi( next().c_str() );
        else if( a == "--watch-timeout" )   watchTimeoutSecs = std::atoi( next().c_str() );
        else if( a == "--once" )            once = true;
        else if( a == "--self-write" )      selfWrite = next();
        else if( !a.empty() && a[0] == '-' ) return usage();
        else                                positional.push_back( a );
    }

    if( positional.empty() )
        return usage();

    const std::string command = positional[0];

    // wxBase bootstrap: KICAD_CURL_EASY's user-agent string asks wxPlatformInfo for the OS,
    // which asserts without a wxApp.  No GUI.
    wxInitializer wxinit;

    std::unique_ptr<CABLY_SECRET_STORE> store;

    if( storeKind == "keychain" )
        store = std::make_unique<CABLY_KEYCHAIN_SECRET_STORE>( keychainService );
    else if( storeKind == "memory" )
        store = std::make_unique<CABLY_MEMORY_SECRET_STORE>();
    else
        return usage();

    if( haveSeed )
    {
        if( seed.expiresAt == 0 )
            seed.expiresAt = 4102444800LL;

        if( seed.refreshToken.empty() )
            seed.refreshToken = "-";

        if( !store->Save( seed ) )
        {
            std::fprintf( stderr, "store: %s\n", store->LastError().c_str() );
            return 1;
        }
    }

    try
    {
        KICAD_CURL::Init();
    }
    catch( ... )
    {
        std::fprintf( stderr, "curl init failed\n" );
        return 1;
    }

    CABLY_HTTP_KICAD http;
    CABLY_BRIDGE     bridge( http, *store, config );
    bridge.LoadSession();

    // save/show/signout: which persistent store the platform used (bridge.sh asserts the
    // Linux fallback file on a machine without a Secret Service).
    auto printStoreBackend = [&]()
    {
        if( auto* kc = dynamic_cast<CABLY_KEYCHAIN_SECRET_STORE*>( store.get() ) )
            std::printf( "store_backend=%s\n", kc->Backend().c_str() );
    };

    int rc = 0;

    if( command == "loopback" )
    {
        int port = bridge.StartLoopback();

        if( port == 0 )
        {
            std::fprintf( stderr, "%s\n", bridge.LastError().c_str() );
            rc = 1;
        }
        else
        {
            std::printf( "port=%d\nstate=%s\nauth_url=%s\n", port, bridge.Loopback()->State().c_str(),
                         bridge.AuthUrl().c_str() );
            std::fflush( stdout );

            if( bridge.FinishLoopback( timeoutSecs * 1000 ) )
            {
                std::printf( "email=%s\naccess_token_len=%zu\nexpires_at=%lld\n",
                             bridge.Session().email.c_str(), bridge.Session().accessToken.size(),
                             bridge.Session().expiresAt );
            }
            else
            {
                std::fprintf( stderr, "%s\n", bridge.LastError().c_str() );
                rc = 1;
            }

            std::printf( "exit=%d\n", rc ); // the test script runs this disowned and reads it here
        }
    }
    else if( command == "validate" )
    {
        if( bridge.ValidateSession() )
            std::printf( "email=%s\n", bridge.Session().email.c_str() );
        else
        {
            std::fprintf( stderr, "%s\n", bridge.LastError().c_str() );
            rc = 1;
        }
    }
    else if( command == "refresh" )
    {
        if( bridge.RefreshSession() )
            std::printf( "access_token_len=%zu\nexpires_at=%lld\n", bridge.Session().accessToken.size(),
                         bridge.Session().expiresAt );
        else
        {
            std::fprintf( stderr, "%s\n", bridge.LastError().c_str() );
            rc = 1;
        }
    }
    else if( command == "list" )
    {
        std::vector<CABLY_PROJECT_SUMMARY> projects;

        if( bridge.ListProjects( projects ) )
        {
            json out = json::array();

            for( const auto& p : projects )
                out.push_back( { { "id", p.id }, { "name", p.name }, { "updated_at", p.updatedAt } } );

            std::printf( "%s\n", out.dump( 2 ).c_str() );
        }
        else
        {
            std::fprintf( stderr, "%s\n", bridge.LastError().c_str() );
            rc = 1;
        }
    }
    else if( command == "fetch" || command == "open" )
    {
        if( positional.size() < 2 )
            return usage();

        std::string projectJson;
        std::string cloudUpdatedAt;

        if( !bridge.FetchProject( positional[1], projectJson, command == "open" ? &cloudUpdatedAt : nullptr ) )
        {
            std::fprintf( stderr, "%s\n", bridge.LastError().c_str() );
            return 1;
        }

        if( command == "fetch" )
        {
            std::printf( "%s\n", projectJson.c_str() );
        }
        else
        {
            CABLY_EXPORT_RESULT exported;

            if( !bridge.ExportProject( projectJson, exported ) )
            {
                std::fprintf( stderr, "%s\n", bridge.LastError().c_str() );
                return 1;
            }

            if( !exported.hasPcb )
            {
                std::fprintf( stderr, "This project has no board yet.\n" );
                return 1;
            }

            std::string name = "cably-project";

            try
            {
                name = json::parse( projectJson ).value( "project_name", name );
            }
            catch( ... )
            {
            }

            // F5: the sidecar remembers the row and its version so a KiCad save can go back.
            CABLY_EXPORT_META meta;
            meta.projectId = positional[1];
            meta.projectName = name;
            meta.cloudUpdatedAt = cloudUpdatedAt;
            meta.engineVersion = exported.engineVersion;

            CABLY_WRITE_RESULT w = CABLY_BRIDGE::WriteProjectFolder(
                    root, name, exported.kicadPcb, exported.hasSch ? exported.kicadSch : "", force, &meta );

            json out;
            out["dir"] = w.dir;
            out["proPath"] = w.proPath;
            out["pcbPath"] = w.pcbPath;
            out["schPath"] = w.schPath;
            out["engineVersion"] = exported.engineVersion;
            out["cloudUpdatedAt"] = cloudUpdatedAt;

            if( !w.written )
            {
                out["status"] = w.conflicts.empty() ? "error" : "conflict";
                out["conflicts"] = w.conflicts;
                out["error"] = w.error;
                std::printf( "%s\n", out.dump( 2 ).c_str() );
                return w.conflicts.empty() ? 1 : 2;
            }

            out["status"] = "written";
            std::printf( "%s\n", out.dump( 2 ).c_str() );
        }
    }
    else if( command == "import" )
    {
        if( positional.size() < 2 || ( pcbFile.empty() && schFile.empty() ) )
            return usage();

        std::string pcbText, schText;

        if( !pcbFile.empty() && !readWholeFile( pcbFile, pcbText ) )
        {
            std::fprintf( stderr, "cannot read %s\n", pcbFile.c_str() );
            return 1;
        }

        if( !schFile.empty() && !readWholeFile( schFile, schText ) )
        {
            std::fprintf( stderr, "cannot read %s\n", schFile.c_str() );
            return 1;
        }

        CABLY_IMPORT_RESULT  r;
        CABLY_IMPORT_OUTCOME outcome = bridge.ImportProject( positional[1], pcbFile.empty() ? nullptr : &pcbText,
                                                             schFile.empty() ? nullptr : &schText,
                                                             expectUpdatedAt, r );

        if( outcome == CABLY_IMPORT_OUTCOME::SYNCED )
        {
            std::printf( "outcome=synced\nupdated_at=%s\nengine_version=%s\npcb_report=%s\nsch_report=%s\n",
                         r.updatedAt.c_str(), r.engineVersion.c_str(), r.pcbReport.c_str(), r.schReport.c_str() );
        }
        else if( outcome == CABLY_IMPORT_OUTCOME::CLOUD_CHANGED )
        {
            std::printf( "outcome=cloud-changed\ncloud_updated_at=%s\n", r.cloudUpdatedAt.c_str() );
            rc = 3;
        }
        else
        {
            std::printf( "outcome=failed\n" );
            std::fprintf( stderr, "%s\n", bridge.LastError().c_str() );
            rc = 1;
        }
    }
    else if( command == "watch" )
    {
        if( positional.size() < 2 )
            return usage();

        std::mutex              mutex;
        std::condition_variable cv;
        int                     events = 0;
        bool                    done = false;
        CABLY_PROJECT_WATCH     watch;
        std::string             err;

        // The callback runs on the watcher thread; the sync is done right there (the CLI
        // has no UI thread to marshal to) and the bridge is used from that thread only.
        bool started = watch.Start(
                positional[1],
                [&]( const CABLY_SAVE_EVENT& e )
                {
                    CABLY_SYNC_RESULT s = CablySyncSave( bridge, positional[1], e );
                    std::printf( "event kind=%s path=%s outcome=%s updated_at=%s engine_version=%s bytes=%zu error=%s\n",
                                 e.kind == CABLY_SAVE_KIND::BOARD ? "board" : "schematic", e.path.c_str(),
                                 outcomeName( s.outcome ), s.cloudUpdatedAt.c_str(), s.engineVersion.c_str(),
                                 e.text.size(), s.error.c_str() );
                    std::fflush( stdout );

                    std::lock_guard<std::mutex> lock( mutex );
                    ++events;

                    if( once )
                        done = true;

                    cv.notify_all();
                },
                watchOptions, &err );

        if( !started )
        {
            std::fprintf( stderr, "%s\n", err.c_str() );
            return 1;
        }

        std::printf( "watching=%s debounce_ms=%d poll_ms=%d\n", positional[1].c_str(), watchOptions.debounceMs,
                     watchOptions.pollMs );
        std::fflush( stdout );

        if( !selfWrite.empty() )
        {
            // What the hand-off does: announce, then write.  Must not come back as a save.
            const std::string text = "(kicad_pcb written-by-cably-desktop)\n";
            watch.Expect( selfWrite, text );
            std::ofstream out( selfWrite, std::ios::binary | std::ios::trunc );
            out << text;
            out.close();
            std::printf( "self-write=done\n" );
            std::fflush( stdout );
        }

        {
            std::unique_lock<std::mutex> lock( mutex );
            cv.wait_for( lock, std::chrono::seconds( watchTimeoutSecs ), [&] { return done; } );
        }

        watch.Stop();

        int handled;
        {
            std::lock_guard<std::mutex> lock( mutex );
            handled = events;
        }

        rc = handled > 0 ? 0 : 3;
        std::printf( "events=%d\nexit=%d\n", handled, rc );
    }
    else if( command == "save" )
    {
        if( !haveSeed )
            return usage();

        printStoreBackend();
    }
    else if( command == "show" )
    {
        if( bridge.HasSession() )
        {
            std::printf( "email=%s\naccess_token_len=%zu\nrefresh_token_len=%zu\nexpires_at=%lld\n",
                         bridge.Session().email.c_str(), bridge.Session().accessToken.size(),
                         bridge.Session().refreshToken.size(), bridge.Session().expiresAt );
            printStoreBackend();
        }
        else
        {
            std::fprintf( stderr, "no session (%s)\n", store->LastError().c_str() );
            rc = 1;
        }
    }
    else if( command == "signout" )
    {
        if( !bridge.SignOut() )
        {
            std::fprintf( stderr, "%s\n", store->LastError().c_str() );
            rc = 1;
        }
        else
        {
            printStoreBackend();
        }
    }
    else
    {
        rc = usage();
    }

    KICAD_CURL::Cleanup();
    return rc;
}

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
 *
 *   loopback     print port=, state=, auth_url=; wait for the handoff; print email=
 *   validate     GET /auth/v1/user (refreshes on 401); print email=
 *   refresh      POST /auth/v1/token?grant_type=refresh_token; print access_token=, expires_at=
 *   list         print the projects as JSON
 *   fetch <id>   print the project JSON
 *   open <id>    fetch -> export -> write folder; print the paths as JSON (exit 2 on conflict)
 *   save         store the seeded session;  show  print the stored session;  signout  clear it
 */

// kicad_curl headers must precede any wxWidgets header.
#include <kicad_curl/kicad_curl.h>

#include <cably_bridge.h>
#include <cably_config.h>

#include <nlohmann/json.hpp>

#include <wx/init.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using json = nlohmann::json;


static int usage()
{
    std::fprintf( stderr, "usage: cably-bridge-cli [options] loopback|validate|refresh|list|fetch <id>|open <id>|save|show|signout\n" );
    return 64;
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
            std::printf( "access_token=%s\nexpires_at=%lld\n", bridge.Session().accessToken.c_str(),
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

        if( !bridge.FetchProject( positional[1], projectJson ) )
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

            CABLY_WRITE_RESULT w = CABLY_BRIDGE::WriteProjectFolder(
                    root, name, exported.kicadPcb, exported.hasSch ? exported.kicadSch : "", force );

            json out;
            out["dir"] = w.dir;
            out["proPath"] = w.proPath;
            out["pcbPath"] = w.pcbPath;
            out["schPath"] = w.schPath;
            out["engineVersion"] = exported.engineVersion;

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
    else if( command == "save" )
    {
        if( !haveSeed )
            return usage();
    }
    else if( command == "show" )
    {
        if( bridge.HasSession() )
        {
            std::printf( "email=%s\naccess_token=%s\nrefresh_token=%s\nexpires_at=%lld\n",
                         bridge.Session().email.c_str(), bridge.Session().accessToken.c_str(),
                         bridge.Session().refreshToken.c_str(), bridge.Session().expiresAt );
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
    }
    else
    {
        rc = usage();
    }

    KICAD_CURL::Cleanup();
    return rc;
}

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
 * Standalone unit test for the pure helper in cably/src/cably_home_recent.h.
 * Built and run by cably/tests/home.sh with the toolchain's wx-config; it needs
 * only wxBase (no GUI, no KiCad libraries), so it is cheap to run without the
 * KICAD_BUILD_QA_TESTS tree.
 */

#include <cably_home_recent.h>

#include <wx/init.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

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


int main()
{
    wxInitializer wxinit;

    const std::set<std::string> existing = { "/home/a/Blink/Blink.kicad_pro",
                                             "/home/a/Amp/amp.pro",
                                             "/home/a/Blink/Blink.kicad_pro",
                                             "/home/a/Radio/radio.kicad_pro",
                                             "/home/a/Extra/extra.kicad_pro" };

    auto exists = [&]( const wxString& aPath )
    {
        return existing.count( std::string( aPath.ToUTF8() ) ) > 0;
    };

    // History order is most-recent-first, exactly as FILE_HISTORY hands it out.
    std::vector<wxString> history = {
        "/home/a/Blink/Blink.kicad_pro",   // 0 keep
        "/home/a/Gone/gone.kicad_pro",     // 1 missing -> drop
        "/home/a/Blink/Blink.kicad_pro",   // 2 duplicate -> drop
        "/home/a/Amp/amp.pro",             // 3 legacy ext -> keep
        "/home/a/Notes/notes.txt",         // 4 not a project -> drop
        "/home/a/Radio/radio.kicad_pro",   // 5 keep
        "/home/a/Extra/extra.kicad_pro",   // 6 keep, but beyond aMax below
    };

    std::vector<CABLY_RECENT_PROJECT> all = CablyRecentProjects( history, 10, exists );
    CHECK( all.size() == 4 );
    CHECK( all[0].title == "Blink" && all[0].historyIndex == 0 );
    CHECK( all[1].title == "amp" && all[1].historyIndex == 3 );
    CHECK( all[2].title == "radio" && all[2].historyIndex == 5 );
    CHECK( all[3].title == "extra" && all[3].historyIndex == 6 );
    CHECK( all[0].path == "/home/a/Blink/Blink.kicad_pro" );
    CHECK( all[0].directory == "/home/a/Blink" );

    std::vector<CABLY_RECENT_PROJECT> capped = CablyRecentProjects( history, 3, exists );
    CHECK( capped.size() == 3 );
    CHECK( capped[2].title == "radio" );

    // Extension match is case-insensitive; a directory-only entry is dropped.
    std::vector<wxString> odd = { "/x/Loud.KICAD_PRO", "/x/dir/" };
    auto yes = []( const wxString& ) { return true; };
    std::vector<CABLY_RECENT_PROJECT> oddOut = CablyRecentProjects( odd, 10, yes );
    CHECK( oddOut.size() == 1 && oddOut[0].title == "Loud" );

    // Empty history -> empty list, no crash.
    CHECK( CablyRecentProjects( {}, 10, yes ).empty() );
    CHECK( CablyRecentProjects( history, 0, yes ).empty() );

    std::printf( "test_cably_home_recent: %d checks passed\n", g_checks );
    return 0;
}

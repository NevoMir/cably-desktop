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
 * Standalone unit test for the pure helpers in cably/src/cably_sync_status.h
 * (F5: the sync UI's report summariser, status line and clock wording).  Built and
 * run by cably/tests/home.sh with the toolchain's wx-config plus the nlohmann/json
 * header from thirdparty/; wxBase only, no GUI, no KiCad libraries, no network.
 *
 * The report shapes are the web app's (src/lib/pcb/kicadImport.ts ImportPcbReport,
 * src/lib/sch/importSch.ts ImportSchReport) as returned by POST /v1/import.
 */

#include <cably_sync_status.h>

#include <wx/init.h>

#include <cstdio>
#include <cstdlib>
#include <string>

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


static const char* PCB_REPORT =
        R"json({"dropped":["zone on F.Cu (zones are not supported)"],
            "newNets":["/VCC_3V3","Net-(R1-Pad2)"],
            "unknownRefs":["J9"],
            "missingRefs":["R3","C2","U7"],
            "counts":{"placements":12,"tracks":40,"vias":6,"graphics":3}})json";

static const char* SCH_REPORT =
        R"json({"dropped":["hierarchical sheet 'power' (sheets are flattened)","bus B1"],
            "unknownSymbols":["Device:Q_NPN_BCE"],
            "counts":{"instances":9,"wires":31,"junctions":4,"labels":5}})json";

static const char* QUIET_PCB =
        R"json({"dropped":[],"newNets":[],"unknownRefs":[],"missingRefs":[],
            "counts":{"placements":2,"tracks":3,"vias":0,"graphics":0}})json";


int main()
{
    wxInitializer wxinit;

    const wxDateTime::TimeZone utc( wxDateTime::UTC );

    // --- Clock: "12:04", 24 h, zero-padded, in the given zone ---------------------------
    wxDateTime noon( (time_t) 1788609840 );   // 2026-09-05T12:04:00Z
    CHECK( CablySyncClock( noon, utc ) == wxS( "12:04" ) );
    CHECK( CablySyncClock( wxDateTime( (time_t) 1788566640 ), utc ) == wxS( "00:04" ) );   // 2026-09-05T00:04:00Z
    CHECK( CablySyncClock( wxDateTime(), utc ) == wxEmptyString );   // invalid -> empty

    // --- Summariser: both reports ---------------------------------------------------------
    {
        CABLY_SYNC_SUMMARY s = CablySummariseImport( PCB_REPORT, SCH_REPORT );
        CHECK( s.ok );
        CHECK( s.netsRenamed == 2 );
        CHECK( s.partsRemoved == 3 );
        CHECK( s.unknownRefs == 1 );
        CHECK( s.unknownSymbols == 1 );
        CHECK( s.dropped == 3 );   // 1 board + 2 schematic
        CHECK( s.headline == wxS( "2 nets renamed · 3 parts removed · 1 unknown reference · 1 unknown symbol · 3 items dropped" ) );

        // the full text carries every item verbatim, grouped, with the counts
        CHECK( s.details.Contains( wxS( "Board" ) ) );
        CHECK( s.details.Contains( wxS( "Schematic" ) ) );
        CHECK( s.details.Contains( wxS( "/VCC_3V3" ) ) );
        CHECK( s.details.Contains( wxS( "Net-(R1-Pad2)" ) ) );
        CHECK( s.details.Contains( wxS( "zone on F.Cu (zones are not supported)" ) ) );
        CHECK( s.details.Contains( wxS( "hierarchical sheet 'power' (sheets are flattened)" ) ) );
        CHECK( s.details.Contains( wxS( "bus B1" ) ) );
        CHECK( s.details.Contains( wxS( "Device:Q_NPN_BCE" ) ) );
        CHECK( s.details.Contains( wxS( "R3" ) ) && s.details.Contains( wxS( "U7" ) ) );
        CHECK( s.details.Contains( wxS( "12 placements" ) ) );
        CHECK( s.details.Contains( wxS( "40 tracks" ) ) );
        CHECK( s.details.Contains( wxS( "31 wires" ) ) );
        CHECK( s.details.Contains( wxS( "Dropped (2)" ) ) );   // the schematic's dropped group

        // the notification's one-line "dropped" digest: first item, then "+N more"
        CHECK( CablySyncDroppedDigest( s ) == wxS( "Dropped: zone on F.Cu (zones are not supported) (+2 more)" ) );
    }

    // --- Singular wording -----------------------------------------------------------------
    {
        CABLY_SYNC_SUMMARY s = CablySummariseImport(
                R"json({"dropped":["arc track"],"newNets":["N1"],"unknownRefs":[],"missingRefs":["R1"],
                    "counts":{"placements":1,"tracks":1,"vias":0,"graphics":0}})json",
                std::string() );
        CHECK( s.ok );
        CHECK( s.headline == wxS( "1 net renamed · 1 part removed · 1 item dropped" ) );
        CHECK( CablySyncDroppedDigest( s ) == wxS( "Dropped: arc track" ) );
    }

    // --- Nothing to say: empty headline, empty digest, but the counts are still there -----
    {
        CABLY_SYNC_SUMMARY s = CablySummariseImport( QUIET_PCB, "null" );
        CHECK( s.ok );
        CHECK( s.headline.IsEmpty() );
        CHECK( s.dropped == 0 );
        CHECK( CablySyncDroppedDigest( s ).IsEmpty() );
        CHECK( s.details.Contains( wxS( "2 placements" ) ) );
        CHECK( !s.details.Contains( wxS( "Schematic" ) ) );   // null report: no section
    }

    // --- Missing fields are zero, not a crash; malformed JSON is reported, not thrown -----
    {
        CABLY_SYNC_SUMMARY s = CablySummariseImport( R"json({"counts":{"placements":1}})json", "" );
        CHECK( s.ok );
        CHECK( s.headline.IsEmpty() );
        CHECK( s.netsRenamed == 0 && s.partsRemoved == 0 && s.dropped == 0 );
    }
    {
        CABLY_SYNC_SUMMARY s = CablySummariseImport( "{not json", SCH_REPORT );
        CHECK( !s.ok );
        CHECK( s.details.Contains( wxS( "unreadable" ) ) );
        CHECK( s.unknownSymbols == 1 );   // the readable half still counts
    }
    {
        CABLY_SYNC_SUMMARY s = CablySummariseImport( "", "" );
        CHECK( s.ok );   // two absent reports: a legal (if empty) outcome
        CHECK( s.headline.IsEmpty() );
        CHECK( CablySyncDroppedDigest( s ).IsEmpty() );
    }

    // --- Status line: "Synced to Cably · 12:04 · 2 nets renamed" ---------------------------
    CHECK( CablySyncStatusLine( noon, wxS( "2 nets renamed" ), utc ) == wxS( "Synced to Cably · 12:04 · 2 nets renamed" ) );
    CHECK( CablySyncStatusLine( noon, wxEmptyString, utc ) == wxS( "Synced to Cably · 12:04" ) );
    CHECK( CablySyncStatusLine( wxDateTime(), wxS( "x" ), utc ) == wxS( "Synced to Cably · x" ) );

    // --- Where the sidecar and the report file live in a project folder ---------------------
    CHECK( CablySidecarPath( wxS( "/tmp/proj" ) ).EndsWith( wxS( "/proj/.cably-export.json" ) ) );
    CHECK( CablySyncReportPath( wxS( "/tmp/proj/" ) ).EndsWith( wxS( "/proj/.cably-sync-report.txt" ) ) );

    std::printf( "test_cably_sync_status: %d checks passed\n", g_checks );
    return 0;
}

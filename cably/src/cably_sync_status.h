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

#ifndef CABLY_SYNC_STATUS_H
#define CABLY_SYNC_STATUS_H

/**
 * Pure helpers behind the sync UI of the Cably home screen (F5): what the user reads
 * after a KiCad save was imported into the cably.dev project.
 *
 * The import reports are the web app's (POST /v1/import -> pcbReport / schReport;
 * src/lib/pcb/kicadImport.ts ImportPcbReport, src/lib/sch/importSch.ts
 * ImportSchReport).  Everything here is wxBase + nlohmann/json only (no GUI, no KiCad
 * libraries, no network) so cably/tests/unit/test_cably_sync_status.cpp builds it
 * standalone.
 */

#include <nlohmann/json.hpp>

#include <wx/datetime.h>
#include <wx/filename.h>
#include <wx/string.h>

#include <string>
#include <vector>


/// What one import told us, condensed for the status line, the notification and the
/// "Details" text.
struct CABLY_SYNC_SUMMARY
{
    bool ok = true;            ///< false when a report that was given could not be parsed

    int netsRenamed = 0;       ///< pcbReport.newNets: nets in the file the project had to re-key
    int partsRemoved = 0;      ///< pcbReport.missingRefs: components deleted in KiCad
    int unknownRefs = 0;       ///< pcbReport.unknownRefs: references the project does not know
    int unknownSymbols = 0;    ///< schReport.unknownSymbols
    int dropped = 0;           ///< pcbReport.dropped + schReport.dropped

    std::vector<wxString> droppedItems;   ///< every dropped line, board first, verbatim

    wxString headline;   ///< "2 nets renamed · 1 item dropped"; empty when nothing to say
    wxString details;    ///< the full, grouped report text (plain, multi-line)
};


namespace CABLY_SYNC_DETAIL
{
inline wxString Count( int aN, const wxString& aOne, const wxString& aMany )
{
    return wxString::Format( wxS( "%d %s" ), aN, aN == 1 ? aOne : aMany );
}


inline std::vector<wxString> Strings( const nlohmann::json& aReport, const char* aKey )
{
    std::vector<wxString> out;

    if( !aReport.is_object() || !aReport.contains( aKey ) || !aReport[aKey].is_array() )
        return out;

    for( const nlohmann::json& item : aReport[aKey] )
    {
        if( item.is_string() )
            out.push_back( wxString::FromUTF8( item.get<std::string>().c_str() ) );
        else
            out.push_back( wxString::FromUTF8( item.dump().c_str() ) );
    }

    return out;
}


inline int CountOf( const nlohmann::json& aReport, const char* aKey )
{
    if( !aReport.is_object() || !aReport.contains( "counts" ) || !aReport["counts"].is_object() )
        return 0;

    const nlohmann::json& counts = aReport["counts"];

    if( !counts.contains( aKey ) || !counts[aKey].is_number() )
        return 0;

    return counts[aKey].get<int>();
}


inline void Group( wxString& aOut, const wxString& aLabel, const std::vector<wxString>& aItems )
{
    aOut += wxString::Format( wxS( "  %s (%d)" ), aLabel, (int) aItems.size() );

    if( aItems.empty() )
    {
        aOut += wxS( "\n" );
        return;
    }

    aOut += wxS( ":\n" );

    for( const wxString& item : aItems )
        aOut += wxS( "    - " ) + item + wxS( "\n" );
}


/// "" (absent), "null" and whitespace all mean "no report"; anything else must parse.
/// Returns false only for unreadable text (aOut untouched).
inline bool Parse( const std::string& aJson, nlohmann::json& aOut, bool& aPresent )
{
    aPresent = false;

    size_t b = aJson.find_first_not_of( " \t\r\n" );

    if( b == std::string::npos )
        return true;

    nlohmann::json parsed = nlohmann::json::parse( aJson, nullptr, false );

    if( parsed.is_discarded() )
        return false;

    if( parsed.is_null() )
        return true;

    aPresent = true;
    aOut = std::move( parsed );
    return true;
}
} // namespace CABLY_SYNC_DETAIL


/**
 * Condense the engine's two import reports (JSON text; "" or "null" when the engine
 * returned none) into counts, a headline and the full details text.
 *
 * A report that is present but unreadable sets ok = false and says so in details;
 * the other report is still counted.
 */
inline CABLY_SYNC_SUMMARY CablySummariseImport( const std::string& aPcbReportJson,
                                                const std::string& aSchReportJson )
{
    using namespace CABLY_SYNC_DETAIL;

    CABLY_SYNC_SUMMARY s;
    nlohmann::json     pcb, sch;
    bool               hasPcb = false, hasSch = false;

    if( !Parse( aPcbReportJson, pcb, hasPcb ) )
    {
        s.ok = false;
        s.details += wxS( "Board report unreadable.\n" );
    }

    if( !Parse( aSchReportJson, sch, hasSch ) )
    {
        s.ok = false;
        s.details += wxS( "Schematic report unreadable.\n" );
    }

    if( hasPcb )
    {
        const std::vector<wxString> newNets = Strings( pcb, "newNets" );
        const std::vector<wxString> missing = Strings( pcb, "missingRefs" );
        const std::vector<wxString> unknown = Strings( pcb, "unknownRefs" );
        const std::vector<wxString> dropped = Strings( pcb, "dropped" );

        s.netsRenamed = (int) newNets.size();
        s.partsRemoved = (int) missing.size();
        s.unknownRefs = (int) unknown.size();
        s.dropped += (int) dropped.size();
        s.droppedItems.insert( s.droppedItems.end(), dropped.begin(), dropped.end() );

        s.details += wxString::Format( wxS( "Board (.kicad_pcb): %s, %s, %s, %s\n" ),
                                       Count( CountOf( pcb, "placements" ), wxS( "placement" ), wxS( "placements" ) ),
                                       Count( CountOf( pcb, "tracks" ), wxS( "track" ), wxS( "tracks" ) ),
                                       Count( CountOf( pcb, "vias" ), wxS( "via" ), wxS( "vias" ) ),
                                       Count( CountOf( pcb, "graphics" ), wxS( "graphic" ), wxS( "graphics" ) ) );
        Group( s.details, wxS( "Nets renamed" ), newNets );
        Group( s.details, wxS( "Parts removed in KiCad" ), missing );
        Group( s.details, wxS( "Unknown references" ), unknown );
        Group( s.details, wxS( "Dropped" ), dropped );
    }

    if( hasSch )
    {
        const std::vector<wxString> unknown = Strings( sch, "unknownSymbols" );
        const std::vector<wxString> dropped = Strings( sch, "dropped" );

        s.unknownSymbols = (int) unknown.size();
        s.dropped += (int) dropped.size();
        s.droppedItems.insert( s.droppedItems.end(), dropped.begin(), dropped.end() );

        s.details += wxString::Format( wxS( "Schematic (.kicad_sch): %s, %s, %s, %s\n" ),
                                       Count( CountOf( sch, "instances" ), wxS( "instance" ), wxS( "instances" ) ),
                                       Count( CountOf( sch, "wires" ), wxS( "wire" ), wxS( "wires" ) ),
                                       Count( CountOf( sch, "junctions" ), wxS( "junction" ), wxS( "junctions" ) ),
                                       Count( CountOf( sch, "labels" ), wxS( "label" ), wxS( "labels" ) ) );
        Group( s.details, wxS( "Unknown symbols" ), unknown );
        Group( s.details, wxS( "Dropped" ), dropped );
    }

    std::vector<wxString> parts;

    if( s.netsRenamed )
        parts.push_back( Count( s.netsRenamed, wxS( "net renamed" ), wxS( "nets renamed" ) ) );

    if( s.partsRemoved )
        parts.push_back( Count( s.partsRemoved, wxS( "part removed" ), wxS( "parts removed" ) ) );

    if( s.unknownRefs )
        parts.push_back( Count( s.unknownRefs, wxS( "unknown reference" ), wxS( "unknown references" ) ) );

    if( s.unknownSymbols )
        parts.push_back( Count( s.unknownSymbols, wxS( "unknown symbol" ), wxS( "unknown symbols" ) ) );

    if( s.dropped )
        parts.push_back( Count( s.dropped, wxS( "item dropped" ), wxS( "items dropped" ) ) );

    for( size_t i = 0; i < parts.size(); ++i )
    {
        if( i )
            s.headline += wxS( " · " );

        s.headline += parts[i];
    }

    return s;
}


/// The notification's one-line digest of what was dropped: the first item, then
/// "(+N more)".  Empty when nothing was dropped.
inline wxString CablySyncDroppedDigest( const CABLY_SYNC_SUMMARY& aSummary )
{
    if( aSummary.droppedItems.empty() )
        return wxEmptyString;

    wxString out = wxS( "Dropped: " ) + aSummary.droppedItems.front();

    if( aSummary.droppedItems.size() > 1 )
        out += wxString::Format( wxS( " (+%d more)" ), (int) aSummary.droppedItems.size() - 1 );

    return out;
}


/// "12:04": 24-hour wall clock in @a aTz; empty for an invalid time.
inline wxString CablySyncClock( const wxDateTime& aWhen,
                                const wxDateTime::TimeZone& aTz = wxDateTime::Local )
{
    if( !aWhen.IsValid() )
        return wxEmptyString;

    return aWhen.Format( wxS( "%H:%M" ), aTz );
}


/// "Synced to Cably · 12:04 · 2 nets renamed" — the clock and the headline are each
/// omitted when empty.
inline wxString CablySyncStatusLine( const wxDateTime& aWhen, const wxString& aHeadline,
                                     const wxDateTime::TimeZone& aTz = wxDateTime::Local )
{
    wxString line = wxS( "Synced to Cably" );
    wxString clock = CablySyncClock( aWhen, aTz );

    if( !clock.IsEmpty() )
        line += wxS( " · " ) + clock;

    if( !aHeadline.IsEmpty() )
        line += wxS( " · " ) + aHeadline;

    return line;
}


/// The bridge's export sidecar in a project folder: its presence is what makes a
/// folder "opened from Cably" and therefore syncable (cably_bridge.h WriteProjectFolder).
inline wxString CablySidecarPath( const wxString& aProjectDir )
{
    wxFileName fn( aProjectDir, wxS( ".cably-export.json" ) );
    return fn.GetFullPath();
}


/// Where the full text of the last sync report is written so the manager's
/// notification can link to it ("View Details").
inline wxString CablySyncReportPath( const wxString& aProjectDir )
{
    wxFileName fn( aProjectDir, wxS( ".cably-sync-report.txt" ) );
    return fn.GetFullPath();
}

#endif // CABLY_SYNC_STATUS_H

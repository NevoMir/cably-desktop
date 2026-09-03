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

#ifndef CABLY_HOME_RECENT_H
#define CABLY_HOME_RECENT_H

/**
 * Pure helper behind the "Recent projects" list of the Cably home screen.
 *
 * It depends on wxBase only (no GUI, no KiCad libraries) so that
 * cably/tests/unit/test_cably_home_recent.cpp can build it standalone.
 */

#include <wx/filename.h>
#include <wx/string.h>

#include <functional>
#include <set>
#include <vector>


struct CABLY_RECENT_PROJECT
{
    wxString title;        ///< project name without extension
    wxString path;         ///< the entry exactly as stored in the file history
    wxString directory;    ///< containing directory, no trailing separator
    int      historyIndex; ///< index in FILE_HISTORY, so ID_FILE1 + historyIndex opens it
};


/**
 * Turn the manager's file history (most recent first) into the entries shown on the
 * home screen: project files only (.kicad_pro / legacy .pro, case-insensitive), that
 * still exist according to @a aExists, de-duplicated, capped at @a aMax.
 */
inline std::vector<CABLY_RECENT_PROJECT>
CablyRecentProjects( const std::vector<wxString>&                   aHistory, size_t aMax,
                     const std::function<bool( const wxString& )>& aExists )
{
    std::vector<CABLY_RECENT_PROJECT> out;
    std::set<wxString>                seen;

    for( size_t i = 0; i < aHistory.size() && out.size() < aMax; ++i )
    {
        const wxString& raw = aHistory[i];
        wxFileName      fn( raw );

        if( fn.GetFullName().IsEmpty() )
            continue;

        wxString ext = fn.GetExt().Lower();

        if( ext != wxString( "kicad_pro" ) && ext != wxString( "pro" ) )
            continue;

        if( !aExists( raw ) )
            continue;

        if( !seen.insert( fn.GetFullPath() ).second )
            continue;

        CABLY_RECENT_PROJECT entry;
        entry.title = fn.GetName();
        entry.path = raw;
        entry.directory = fn.GetPath();
        entry.historyIndex = (int) i;
        out.push_back( entry );
    }

    return out;
}

#endif // CABLY_HOME_RECENT_H

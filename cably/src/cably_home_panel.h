/*
 * This program source code file is part of Cably Desktop, based on KiCad,
 * a free EDA CAD application.
 *
 * Copyright (C) 2026 Cably
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors
 * (the launcher-panel/manager-frame patterns this adapts).
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

#ifndef CABLY_HOME_PANEL_H
#define CABLY_HOME_PANEL_H

#include <wx/panel.h>

#include <vector>

#include <cably_home_recent.h>

class KICAD_MANAGER_FRAME;
class wxButton;
class wxListBox;
class wxStaticText;
class wxSysColourChangedEvent;
class wxTextCtrl;


/**
 * The Cably Desktop home screen, shown in the manager window while no project is loaded.
 *
 * Contains the circuit prompt (generation itself arrives with the cloud bridge, so the
 * button is disabled here), the recent-projects list built from the manager's own
 * FILE_HISTORY, an "Open project..." button that runs the manager's open action, and a
 * footer that opens the About dialog.  No network, no images.
 */
class CABLY_HOME_PANEL : public wxPanel
{
public:
    explicit CABLY_HOME_PANEL( KICAD_MANAGER_FRAME* aFrame );
    ~CABLY_HOME_PANEL() override;

    /// Rebuild the recent-projects list from the frame's FILE_HISTORY.
    void RefreshRecentProjects();

    /// Re-apply all translatable labels after a language change.
    void ShowChangedLanguage();

private:
    void buildUi();
    void applyPalette();
    void updateLabels();

    void onOpenProject( wxCommandEvent& aEvent );
    void onRecentActivated( wxCommandEvent& aEvent );
    void onAboutClicked( wxMouseEvent& aEvent );
    void onThemeChanged( wxSysColourChangedEvent& aEvent );

    KICAD_MANAGER_FRAME* m_frame;

    wxStaticText* m_title;
    wxStaticText* m_tagline;
    wxPanel*      m_card;
    wxStaticText* m_promptLabel;
    wxTextCtrl*   m_prompt;
    wxStaticText* m_generateHint;
    wxButton*     m_generate;
    wxStaticText* m_recentLabel;
    wxButton*     m_open;
    wxListBox*    m_recentList;
    wxStaticText* m_recentEmpty;
    wxStaticText* m_footer;

    std::vector<CABLY_RECENT_PROJECT> m_recent;
};

#endif // CABLY_HOME_PANEL_H

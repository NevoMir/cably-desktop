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

#include <memory>
#include <vector>

#include <cably_home_recent.h>

class KICAD_MANAGER_FRAME;
class wxButton;
class wxListBox;
class wxScrolledWindow;
class wxStaticText;
class wxSysColourChangedEvent;
class wxTextCtrl;

struct CABLY_HOME_CLOUD_STATE;   // the panel's cloud/bridge side (cably_home_panel.cpp)


/**
 * The Cably Desktop home screen, shown in the manager window while no project is loaded.
 *
 * Two paths, both visible when signed out:
 *  - "Describe the circuit you want" + Generate: opens cably.dev/app?prompt=... in the
 *    browser (generation stays on the web; the result is opened here afterwards).
 *  - "Your Cably projects": "Sign in to Cably" runs the loopback hand-off through the
 *    bridge (browser opens cably.dev/desktop/auth, tokens come back to 127.0.0.1 and go
 *    to the keychain); once signed in the user's projects are listed newest first with
 *    an "Open" button each, which exports through the engine into
 *    ~/Documents/Cably Desktop/<stem>/ and loads the .kicad_pro in the manager.
 *
 * Plus the recent-projects list from the manager's own FILE_HISTORY, "Open project..."
 * and a footer that opens About.  Every network call runs off the UI thread; results
 * come back through CallAfter guarded by a liveness token.
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
    /// What the "Your Cably projects" card shows.
    enum class CLOUD_UI
    {
        SIGNED_OUT,   ///< sign-in button + the two-paths explanation
        WAITING,      ///< browser opened, loopback listening; Cancel
        SIGNED_IN     ///< "Signed in as", the projects list, Sign out / Refresh
    };

    void buildUi();
    void applyPalette();
    void updateLabels();

    // cloud card
    void setCloudUi( CLOUD_UI aUi );
    void setCloudStatus( const wxString& aText, bool aIsError );
    void rebuildProjectRows();
    void setProjectsBusy( bool aBusy );
    void restoreSession();
    void loadProjects();
    void dropSession( const wxString& aReason );
    void openProject( size_t aIndex );
    void finishOpen( const wxString& aProPath );

    void onGenerate( wxCommandEvent& aEvent );
    void onSignIn( wxCommandEvent& aEvent );
    void onCancelSignIn( wxCommandEvent& aEvent );
    void onSignOut( wxCommandEvent& aEvent );
    void onRefreshProjects( wxCommandEvent& aEvent );

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

    wxPanel*          m_cloudCard;
    wxStaticText*     m_cloudLabel;
    wxStaticText*     m_cloudStatus;
    wxStaticText*     m_cloudExplain;
    wxButton*         m_signIn;
    wxButton*         m_cancelSignIn;
    wxButton*         m_signOut;
    wxButton*         m_refreshProjects;
    wxScrolledWindow* m_projects;
    wxStaticText*     m_projectsEmpty;

    wxStaticText* m_recentLabel;
    wxButton*     m_open;
    wxListBox*    m_recentList;
    wxStaticText* m_recentEmpty;
    wxStaticText* m_footer;

    std::vector<CABLY_RECENT_PROJECT> m_recent;

    CLOUD_UI                                m_cloudUi;
    bool                                    m_cloudStatusIsError;
    std::unique_ptr<CABLY_HOME_CLOUD_STATE> m_cloud;
};

#endif // CABLY_HOME_PANEL_H

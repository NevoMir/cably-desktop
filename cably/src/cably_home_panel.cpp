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

#include <cably_home_panel.h>

#include <file_history.h>
#include <id.h>
#include <kicad_manager_frame.h>
#include <kiplatform/ui.h>
#include <tool/actions.h>
#include <tool/tool_manager.h>
#include <tools/kicad_manager_actions.h>
#include <widgets/ui_common.h>

#include <wx/button.h>
#include <wx/filename.h>
#include <wx/listbox.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tooltip.h>


namespace
{

/// Cably's palette (cably.dev src/index.css), as wxColour.  Light values: background
/// hsl(0 0% 97%), card 100%, foreground 15%, muted-foreground 45%, border 85%,
/// primary hsl(210 100% 45%), accent hsl(265 80% 55%).  Dark values from the same file.
struct CABLY_PALETTE
{
    wxColour background;
    wxColour card;
    wxColour foreground;
    wxColour muted;
    wxColour border;
    wxColour primary;
    wxColour accent;
};


CABLY_PALETTE cablyPalette( bool aDark )
{
    if( aDark )
    {
        return { wxColour( 20, 20, 20 ),    wxColour( 33, 33, 33 ),    wxColour( 230, 230, 230 ),
                 wxColour( 140, 140, 140 ), wxColour( 64, 64, 64 ),    wxColour( 31, 143, 255 ),
                 wxColour( 139, 71, 235 ) };
    }

    return { wxColour( 247, 247, 247 ), wxColour( 255, 255, 255 ), wxColour( 38, 38, 38 ),
             wxColour( 115, 115, 115 ), wxColour( 217, 217, 217 ), wxColour( 0, 115, 230 ),
             wxColour( 125, 48, 232 ) };
}


constexpr size_t MAX_RECENT_PROJECTS = 8;

} // namespace


CABLY_HOME_PANEL::CABLY_HOME_PANEL( KICAD_MANAGER_FRAME* aFrame ) :
        wxPanel( aFrame, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL,
                 wxS( "CablyHomePanel" ) ),
        m_frame( aFrame ),
        m_title( nullptr ),
        m_tagline( nullptr ),
        m_card( nullptr ),
        m_promptLabel( nullptr ),
        m_prompt( nullptr ),
        m_generateHint( nullptr ),
        m_generate( nullptr ),
        m_recentLabel( nullptr ),
        m_open( nullptr ),
        m_recentList( nullptr ),
        m_recentEmpty( nullptr ),
        m_footer( nullptr )
{
    buildUi();
    updateLabels();
    applyPalette();
    RefreshRecentProjects();

    Bind( wxEVT_SYS_COLOUR_CHANGED,
          wxSysColourChangedEventHandler( CABLY_HOME_PANEL::onThemeChanged ), this );
}


CABLY_HOME_PANEL::~CABLY_HOME_PANEL()
{
    Unbind( wxEVT_SYS_COLOUR_CHANGED,
            wxSysColourChangedEventHandler( CABLY_HOME_PANEL::onThemeChanged ), this );
}


void CABLY_HOME_PANEL::buildUi()
{
    const int columnWidth = FromDIP( 640 );

    wxBoxSizer* outer = new wxBoxSizer( wxVERTICAL );
    wxBoxSizer* column = new wxBoxSizer( wxVERTICAL );
    column->SetMinSize( columnWidth, -1 );

    // Header -----------------------------------------------------------------------------
    m_title = new wxStaticText( this, wxID_ANY, wxEmptyString );
    m_tagline = new wxStaticText( this, wxID_ANY, wxEmptyString );
    column->Add( m_title, 0, wxBOTTOM, FromDIP( 4 ) );
    column->Add( m_tagline, 0, wxBOTTOM, FromDIP( 20 ) );

    // Prompt card ------------------------------------------------------------------------
    m_card = new wxPanel( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE );
    wxBoxSizer* cardSizer = new wxBoxSizer( wxVERTICAL );

    m_promptLabel = new wxStaticText( m_card, wxID_ANY, wxEmptyString );
    m_prompt = new wxTextCtrl( m_card, wxID_ANY, wxEmptyString, wxDefaultPosition,
                               wxSize( -1, FromDIP( 110 ) ), wxTE_MULTILINE | wxTE_RICH2 );

    wxBoxSizer* generateRow = new wxBoxSizer( wxHORIZONTAL );
    m_generateHint = new wxStaticText( m_card, wxID_ANY, wxEmptyString );
    m_generate = new wxButton( m_card, wxID_ANY, wxEmptyString );
    m_generate->Disable();      // generation needs the cloud bridge (next phase)
    generateRow->Add( m_generateHint, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 12 ) );
    generateRow->Add( m_generate, 0, wxALIGN_CENTER_VERTICAL );

    cardSizer->Add( m_promptLabel, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP( 16 ) );
    cardSizer->Add( m_prompt, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP( 16 ) );
    cardSizer->Add( generateRow, 0, wxEXPAND | wxALL, FromDIP( 16 ) );
    m_card->SetSizer( cardSizer );

    column->Add( m_card, 0, wxEXPAND | wxBOTTOM, FromDIP( 24 ) );

    // Recent projects --------------------------------------------------------------------
    wxBoxSizer* recentRow = new wxBoxSizer( wxHORIZONTAL );
    m_recentLabel = new wxStaticText( this, wxID_ANY, wxEmptyString );
    m_open = new wxButton( this, wxID_ANY, wxEmptyString );
    recentRow->Add( m_recentLabel, 1, wxALIGN_CENTER_VERTICAL );
    recentRow->Add( m_open, 0, wxALIGN_CENTER_VERTICAL );
    column->Add( recentRow, 0, wxEXPAND | wxBOTTOM, FromDIP( 8 ) );

    m_recentList = new wxListBox( this, wxID_ANY, wxDefaultPosition,
                                  wxSize( -1, FromDIP( 170 ) ), 0, nullptr,
                                  wxLB_SINGLE | wxLB_NEEDED_SB );
    m_recentEmpty = new wxStaticText( this, wxID_ANY, wxEmptyString );
    column->Add( m_recentList, 0, wxEXPAND );
    column->Add( m_recentEmpty, 0, wxTOP, FromDIP( 6 ) );

    // Footer -----------------------------------------------------------------------------
    m_footer = new wxStaticText( this, wxID_ANY, wxEmptyString );
    m_footer->SetCursor( wxCursor( wxCURSOR_HAND ) );

    outer->AddStretchSpacer( 2 );
    outer->Add( column, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP( 32 ) );
    outer->AddStretchSpacer( 3 );
    outer->Add( m_footer, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP( 14 ) );
    SetSizer( outer );

    // Fonts (KIUI keeps the platform face and icon-scale rules).
    wxFont control = KIUI::GetControlFont( this );
    m_title->SetFont( control.Scaled( 2.2f ).Bold() );
    m_tagline->SetFont( control.Larger() );
    m_promptLabel->SetFont( control.Bold() );
    m_recentLabel->SetFont( control.Larger().Bold() );
    m_generateHint->SetFont( KIUI::GetInfoFont( this ) );
    m_recentEmpty->SetFont( KIUI::GetInfoFont( this ) );
    m_footer->SetFont( KIUI::GetInfoFont( this ) );

    m_open->Bind( wxEVT_BUTTON, &CABLY_HOME_PANEL::onOpenProject, this );
    m_recentList->Bind( wxEVT_LISTBOX_DCLICK, &CABLY_HOME_PANEL::onRecentActivated, this );
    m_footer->Bind( wxEVT_LEFT_UP, &CABLY_HOME_PANEL::onAboutClicked, this );
}


void CABLY_HOME_PANEL::updateLabels()
{
    // Trademark: the product is "Cably Desktop", based on KiCad.
    m_title->SetLabel( _( "Cably Desktop" ) );
    m_tagline->SetLabel( _( "Describe a circuit, then open the schematic and board in the editors." ) );
    m_promptLabel->SetLabel( _( "Describe the circuit you want" ) );
    m_prompt->SetHint( _( "e.g. a 5 V USB-powered blinking LED with a 555 timer" ) );
    m_generate->SetLabel( _( "Generate" ) );

    const wxString hint = _( "Sign in to Cably to generate (coming in the next phase)" );
    m_generate->SetToolTip( hint );
    m_card->SetToolTip( hint );     // disabled controls get no hover events on some platforms
    m_generateHint->SetLabel( hint );

    m_recentLabel->SetLabel( _( "Recent projects" ) );
    m_open->SetLabel( _( "Open project..." ) );
    m_recentEmpty->SetLabel( _( "No recent projects yet. Open one, or create a new project from the File menu." ) );
    m_footer->SetLabel( _( "based on KiCad · GPL-3.0-or-later" ) );
    m_footer->SetToolTip( _( "About Cably Desktop" ) );
}


void CABLY_HOME_PANEL::applyPalette()
{
    const CABLY_PALETTE p = cablyPalette( KIPLATFORM::UI::IsDarkTheme() );

    SetBackgroundColour( p.background );
    m_card->SetBackgroundColour( p.card );

    m_title->SetForegroundColour( p.primary );
    m_tagline->SetForegroundColour( p.muted );
    m_promptLabel->SetForegroundColour( p.foreground );
    m_prompt->SetBackgroundColour( p.card );
    m_prompt->SetForegroundColour( p.foreground );
    m_generateHint->SetForegroundColour( p.accent );
    m_recentLabel->SetForegroundColour( p.foreground );
    m_recentList->SetBackgroundColour( p.card );
    m_recentList->SetForegroundColour( p.foreground );
    m_recentEmpty->SetForegroundColour( p.muted );
    m_footer->SetForegroundColour( p.primary );

    Refresh();
}


void CABLY_HOME_PANEL::RefreshRecentProjects()
{
    FILE_HISTORY&         history = m_frame->GetFileHistory();
    std::vector<wxString> entries;

    for( size_t i = 0; i < history.GetCount(); ++i )
        entries.push_back( history.GetHistoryFile( i ) );

    m_recent = CablyRecentProjects( entries, MAX_RECENT_PROJECTS,
                                    []( const wxString& aPath )
                                    {
                                        return wxFileName::FileExists( aPath );
                                    } );

    m_recentList->Clear();

    for( const CABLY_RECENT_PROJECT& entry : m_recent )
        m_recentList->Append( entry.title + wxS( "  —  " ) + entry.directory );

    m_recentList->Show( !m_recent.empty() );
    m_recentEmpty->Show( m_recent.empty() );
    Layout();
}


void CABLY_HOME_PANEL::ShowChangedLanguage()
{
    updateLabels();
    RefreshRecentProjects();
}


void CABLY_HOME_PANEL::onOpenProject( wxCommandEvent& aEvent )
{
    // The manager's own "Open Project..." action (file dialog, MRU path, LoadProject).
    m_frame->GetToolManager()->RunAction( KICAD_MANAGER_ACTIONS::openProject );
}


void CABLY_HOME_PANEL::onRecentActivated( wxCommandEvent& aEvent )
{
    int sel = m_recentList->GetSelection();

    if( sel < 0 || sel >= (int) m_recent.size() )
        return;

    int historyIndex = m_recent[sel].historyIndex;

    // Same path as File > Open Recent: KICAD_MANAGER_FRAME::OnFileHistory resolves the
    // entry through FILE_HISTORY, offers to drop a vanished file, and loads the project.
    // Deferred so the list box finishes its own event before the panel is hidden.
    CallAfter(
            [this, historyIndex]()
            {
                wxCommandEvent evt( wxEVT_MENU, ID_FILE1 + historyIndex );
                m_frame->OnFileHistory( evt );
            } );
}


void CABLY_HOME_PANEL::onAboutClicked( wxMouseEvent& aEvent )
{
    m_frame->GetToolManager()->RunAction( ACTIONS::about );
}


void CABLY_HOME_PANEL::onThemeChanged( wxSysColourChangedEvent& aEvent )
{
    applyPalette();
    aEvent.Skip();
}

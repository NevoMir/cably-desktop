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

#include <cably_config.h>
#include <cably_home_cloud.h>
#include <cably_sync_status.h>

#include <file_history.h>
#include <id.h>
#include <kicad_manager_frame.h>
#include <kiplatform/ui.h>
#include <notifications_manager.h>
#include <pgm_base.h>
#include <tool/actions.h>
#include <tool/tool_manager.h>
#include <tools/kicad_manager_actions.h>
#include <widgets/ui_common.h>

#include <wx/button.h>
#include <wx/datetime.h>
#include <wx/dialog.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/filesys.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/tooltip.h>
#include <wx/utils.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>


// ======================================================================================
// Bridge adapter — THE ONLY PLACE IN THE HOME PANEL THAT NAMES CABLY_BRIDGE.
//
// cably/src/cably_bridge.h (agent "bridge") is plain C++17 with no wx/KiCad types and is
// single-threaded by design (one session, one LastError()).  The panel therefore owns ONE
// bridge behind m_cloud->bridgeMutex; every call site locks it, worker threads for the
// whole operation.  Cancelling a sign-in never touches the bridge from the UI thread: it
// raises a flag, the waiting worker wakes within one WaitForSession() slice and stops the
// listener itself, so no loopback is destroyed under a thread blocked on it.
// ======================================================================================
#include <cably_bridge.h>


// ======================================================================================
// Sync adapter (F5) — the only place in the home panel that names the sync-bridge's types.
//
// cably/src/cably_sync.h (agent "sync-bridge"): CABLY_PROJECT_WATCH polls the project
// folder on its own thread (board/schematic only; backups, locks, autosaves ignored;
// per-path debounce; content-hash dedupe seeded from the sidecar) and calls back ON THAT
// THREAD with the whole saved document; CablySyncSave() reads the sidecar, runs
// CABLY_BRIDGE::ImportProject (GET row -> POST /v1/import -> PATCH row, refusing when the
// row is newer than the sidecar's cloudUpdatedAt) and rewrites the sidecar.
// ======================================================================================
#include <cably_sync.h>

namespace
{

using SAVE_EVENT = CABLY_SAVE_EVENT;   // kind, path, text


/// One import's verdict plus whether the bridge lost its session on the way.
struct SYNC_RESULT
{
    CABLY_SYNC_RESULT result;
    bool              sessionGone = false;
};


/**
 * "Keep my KiCad edits (overwrite cloud)": forget the version the sidecar expects so
 * ImportProject skips its newer-than check (an empty aExpectedUpdatedAt) and the PATCH
 * goes through; the sync that follows records the new version.
 */
bool syncForgetCloudVersion( const std::string& aDir, std::string& aError )
{
    CABLY_EXPORT_SIDECAR sidecar;

    if( !CABLY_EXPORT_SIDECAR::Load( aDir, sidecar ) )
    {
        aError = "no readable .cably-export.json in the project folder";
        return false;
    }

    sidecar.meta.cloudUpdatedAt.clear();

    if( !sidecar.Save( aDir ) )
    {
        aError = "couldn't rewrite .cably-export.json in the project folder";
        return false;
    }

    return true;
}


/// How often the panel re-checks which project the manager has loaded (safety net for
/// the wxEVT_SHOW hook; a project load/close is noticed within this).
constexpr int SYNC_POLL_MS = 1500;

} // namespace
// ---- end sync adapter ------------------------------------------------------------------


namespace
{

using CLOUD_SESSION = CABLY_SESSION;           // accessToken, refreshToken, email, expiresAt
using CLOUD_PROJECT = CABLY_PROJECT_SUMMARY;   // id, name, updatedAt (ISO-8601)
using CLOUD_EXPORT = CABLY_EXPORT_RESULT;      // hasPcb/hasSch, kicadPcb, kicadSch, engineVersion


enum class CLOUD_WRITE
{
    WRITTEN,
    CONFLICT,   ///< a file KiCad edited would be overwritten; nothing was touched
    FAILED
};


/// Maps CABLY_BRIDGE::WriteProjectFolder's result onto the three outcomes the UI knows.
CLOUD_WRITE cloudWriteStatus( const CABLY_WRITE_RESULT& aResult )
{
    if( aResult.written )
        return CLOUD_WRITE::WRITTEN;

    return aResult.conflicts.empty() ? CLOUD_WRITE::FAILED : CLOUD_WRITE::CONFLICT;
}


/// How long "Waiting for the browser…" waits before giving up (the user can Cancel).
constexpr int LOGIN_TIMEOUT_MS = 10 * 60 * 1000;
/// One WaitForSession() slice: the cancel latency.
constexpr int LOGIN_SLICE_MS = 500;
// ---- end bridge adapter ----------------------------------------------------------------



/// Cably's palette (cably.dev src/index.css), as wxColour.  Light values: background
/// hsl(0 0% 97%), card 100%, foreground 15%, muted-foreground 45%, border 85%,
/// primary hsl(210 100% 45%), accent hsl(265 80% 55%), destructive hsl(0 84% 60%).
/// Dark values from the same file.
struct CABLY_PALETTE
{
    wxColour background;
    wxColour card;
    wxColour foreground;
    wxColour muted;
    wxColour border;
    wxColour primary;
    wxColour accent;
    wxColour destructive;
};


CABLY_PALETTE cablyPalette( bool aDark )
{
    if( aDark )
    {
        return { wxColour( 20, 20, 20 ),    wxColour( 33, 33, 33 ),    wxColour( 230, 230, 230 ),
                 wxColour( 140, 140, 140 ), wxColour( 64, 64, 64 ),    wxColour( 31, 143, 255 ),
                 wxColour( 139, 71, 235 ),  wxColour( 248, 113, 113 ) };
    }

    return { wxColour( 247, 247, 247 ), wxColour( 255, 255, 255 ), wxColour( 38, 38, 38 ),
             wxColour( 115, 115, 115 ), wxColour( 217, 217, 217 ), wxColour( 0, 115, 230 ),
             wxColour( 125, 48, 232 ),  wxColour( 220, 38, 38 ) };
}


constexpr size_t MAX_RECENT_PROJECTS = 8;


/// Shared between the panel and its worker threads: once the panel is gone, results are
/// dropped instead of being posted to a dead window.
struct CABLY_HOME_LIVENESS
{
    std::mutex mutex;
    bool       alive = true;
};


/**
 * Run @a aWork on a detached thread and hand its result to @a aDone on the UI thread
 * (wxEvtHandler::CallAfter is thread-safe).  The bridge's HTTPS calls block for seconds;
 * nothing in @a aWork may touch wx GUI objects.
 */
template <typename RESULT>
void cablyRunAsync( const std::shared_ptr<CABLY_HOME_LIVENESS>& aLive, wxEvtHandler* aTarget,
                    std::function<RESULT()> aWork, std::function<void( const RESULT& )> aDone )
{
    std::thread(
            [aLive, aTarget, aWork, aDone]()
            {
                RESULT result = aWork();

                std::lock_guard<std::mutex> lock( aLive->mutex );

                if( !aLive->alive )
                    return;

                aTarget->CallAfter(
                        [aDone, result]()
                        {
                            aDone( result );
                        } );
            } )
            .detach();
}


struct LOGIN_RESULT
{
    bool        ok = false;
    bool        sessionGone = false;   ///< the bridge dropped the session (dead refresh token)
    std::string email;
    std::string error;
};


struct LIST_RESULT
{
    bool                       ok = false;
    bool                       sessionGone = false;
    std::vector<CLOUD_PROJECT> projects;
    std::string                error;
};


struct OPEN_RESULT
{
    bool                          ok = false;
    bool                          sessionGone = false;
    CLOUD_WRITE                   status = CLOUD_WRITE::FAILED;
    wxString                      dir;       ///< the project folder the bridge chose
    wxString                      proPath;   ///< the .kicad_pro to load (also on CONFLICT: the existing one)
    std::shared_ptr<CLOUD_EXPORT> exported;  ///< kept so "Replace" needs no second export
    std::string                   error;
};


wxString cablyProjectsRoot()
{
    // ~/Documents/Cably Desktop — the same root the web app's desktop hand-off uses.
    wxFileName root( wxStandardPaths::Get().GetDocumentsDir(), wxEmptyString );
    root.AppendDir( wxS( "Cably Desktop" ) );
    return root.GetPath();
}

} // namespace


/// The panel's cloud side: the bridge (HTTP through KiCad's curl, session in the macOS
/// keychain), what the UI knows about it, the pending sign-in and the row widgets.
struct CABLY_HOME_CLOUD_STATE
{
    std::shared_ptr<CABLY_HOME_LIVENESS> live = std::make_shared<CABLY_HOME_LIVENESS>();

    CABLY_HTTP_KICAD            http;
    CABLY_KEYCHAIN_SECRET_STORE store{ CABLY_KEYCHAIN_SERVICE };
    CABLY_BRIDGE                bridge{ http, store, CABLY_BRIDGE_CONFIG::Default() };
    std::mutex                  bridgeMutex;   ///< the bridge is single-threaded: one caller at a time

    bool                       signedIn = false;
    std::string                email;          ///< UI copy of bridge.Session().email
    std::vector<CLOUD_PROJECT> projects;

    std::atomic<bool> cancelLogin{ false };    ///< raised by Cancel; the waiting worker stops the listener
    unsigned          serial = 0;              ///< bumped on cancel / sign-out: stale results are dropped
    bool              busy = false;

    struct ROW
    {
        wxStaticText* name;
        wxStaticText* updated;
        wxButton*     open;
    };

    std::vector<ROW> rows;
};


/// The panel's save-to-cloud side (F5).
struct CABLY_HOME_SYNC_STATE
{
    CABLY_PROJECT_WATCH watch;
    wxString            dir;        ///< the folder being watched; empty when no Cably project is loaded
    wxString            lastDir;    ///< the folder the last import ran for (Retry's target)
    bool                importing = false;
    unsigned            serial = 0;            ///< bumped per import: stale results are dropped
    wxString            lastDetails;           ///< the full text behind "Details"

    std::shared_ptr<const SAVE_EVENT>              lastEvent;   ///< the save being / last sent (Retry, overwrite)
    std::vector<std::shared_ptr<const SAVE_EVENT>> pending;     ///< saves that landed mid-import, newest per path

    std::unique_ptr<wxTimer> timer;

    /// Folder -> the cloud project it was opened from in this session (for "reload").
    std::map<wxString, CLOUD_PROJECT> opened;
};


/// The F4 open flow's work: fetch the project, export it through the engine, write the
/// folder.  Runs on a worker thread under the bridge mutex.  Shared by "Open" (and its
/// Replace) and by the F5 "Discard my edits (reload from Cably)".
static OPEN_RESULT cloudFetchExportWrite( CABLY_HOME_CLOUD_STATE* aCloud, const CLOUD_PROJECT& aProject,
                                          const std::string& aRootUtf8, bool aForce )
{
    std::lock_guard<std::mutex> lock( aCloud->bridgeMutex );
    OPEN_RESULT                 r;
    std::string                 projectJson;
    std::string                 cloudUpdatedAt;

    if( !aCloud->bridge.FetchProject( aProject.id, projectJson, &cloudUpdatedAt ) )
    {
        r.error = aCloud->bridge.LastError();
        r.sessionGone = !aCloud->bridge.HasSession();
        return r;
    }

    r.exported = std::make_shared<CLOUD_EXPORT>();

    if( !aCloud->bridge.ExportProject( projectJson, *r.exported ) )
    {
        r.error = aCloud->bridge.LastError();
        r.sessionGone = !aCloud->bridge.HasSession();
        return r;
    }

    // F5: the sidecar records which cloud row (and which version of it) this folder
    // came from; a save there syncs back to that row (cably_sync.h).
    CABLY_EXPORT_META meta;
    meta.projectId = aProject.id;
    meta.projectName = aProject.name;
    meta.cloudUpdatedAt = cloudUpdatedAt;
    meta.engineVersion = r.exported->engineVersion;

    // The bridge derives the folder from the name (SafeStem) and applies the
    // never-clobber-a-KiCad-edit rule via the .cably-export.json sidecar.
    CABLY_WRITE_RESULT w = CABLY_BRIDGE::WriteProjectFolder(
            aRootUtf8, aProject.name, r.exported->kicadPcb,
            r.exported->hasSch ? r.exported->kicadSch : std::string(), aForce, &meta );

    r.status = cloudWriteStatus( w );
    r.ok = r.status != CLOUD_WRITE::FAILED;
    r.error = w.error;
    r.dir = wxString::FromUTF8( w.dir.c_str() );
    r.proPath = wxString::FromUTF8( w.proPath.c_str() );
    return r;
}


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
        m_cloudCard( nullptr ),
        m_cloudLabel( nullptr ),
        m_cloudStatus( nullptr ),
        m_cloudExplain( nullptr ),
        m_signIn( nullptr ),
        m_cancelSignIn( nullptr ),
        m_signOut( nullptr ),
        m_refreshProjects( nullptr ),
        m_projects( nullptr ),
        m_projectsEmpty( nullptr ),
        m_syncStatus( nullptr ),
        m_syncDetails( nullptr ),
        m_syncRetry( nullptr ),
        m_recentLabel( nullptr ),
        m_open( nullptr ),
        m_recentList( nullptr ),
        m_recentEmpty( nullptr ),
        m_footer( nullptr ),
        m_cloudUi( CLOUD_UI::SIGNED_OUT ),
        m_cloudStatusIsError( false ),
        m_syncStatusIsError( false ),
        m_cloud( std::make_unique<CABLY_HOME_CLOUD_STATE>() ),
        m_sync( std::make_unique<CABLY_HOME_SYNC_STATE>() )
{
    buildUi();
    updateLabels();
    applyPalette();
    setCloudUi( CLOUD_UI::SIGNED_OUT );
    setSyncStatus( wxEmptyString, false, false, false );
    RefreshRecentProjects();
    restoreSession();

    Bind( wxEVT_SYS_COLOUR_CHANGED,
          wxSysColourChangedEventHandler( CABLY_HOME_PANEL::onThemeChanged ), this );

    // F5: a project load hides this pane, a close shows it again — that is the hook for
    // starting/stopping the sync watcher; the timer catches anything the show event
    // misses.  No manager-frame change needed.
    Bind( wxEVT_SHOW, &CABLY_HOME_PANEL::onShow, this );
    m_sync->timer = std::make_unique<wxTimer>( this );
    Bind( wxEVT_TIMER, &CABLY_HOME_PANEL::onSyncTimer, this );
    m_sync->timer->Start( SYNC_POLL_MS );
}


CABLY_HOME_PANEL::~CABLY_HOME_PANEL()
{
    Unbind( wxEVT_SYS_COLOUR_CHANGED,
            wxSysColourChangedEventHandler( CABLY_HOME_PANEL::onThemeChanged ), this );
    Unbind( wxEVT_SHOW, &CABLY_HOME_PANEL::onShow, this );
    Unbind( wxEVT_TIMER, &CABLY_HOME_PANEL::onSyncTimer, this );

    if( m_sync->timer )
        m_sync->timer->Stop();

    stopSync();

    {
        std::lock_guard<std::mutex> lock( m_cloud->live->mutex );
        m_cloud->live->alive = false;
    }

    // A worker still waiting for the browser wakes within one slice, stops the listener
    // and releases the bridge; the bridge is destroyed with m_cloud only after that.
    m_cloud->cancelLogin = true;
    std::lock_guard<std::mutex> bridgeLock( m_cloud->bridgeMutex );
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

    // Path 1: prompt card (generation happens on cably.dev) -------------------------------
    m_card = new wxPanel( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE );
    wxBoxSizer* cardSizer = new wxBoxSizer( wxVERTICAL );

    m_promptLabel = new wxStaticText( m_card, wxID_ANY, wxEmptyString );
    m_prompt = new wxTextCtrl( m_card, wxID_ANY, wxEmptyString, wxDefaultPosition,
                               wxSize( -1, FromDIP( 90 ) ), wxTE_MULTILINE | wxTE_RICH2 );

    wxBoxSizer* generateRow = new wxBoxSizer( wxHORIZONTAL );
    m_generateHint = new wxStaticText( m_card, wxID_ANY, wxEmptyString );
    m_generate = new wxButton( m_card, wxID_ANY, wxEmptyString );
    generateRow->Add( m_generateHint, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 12 ) );
    generateRow->Add( m_generate, 0, wxALIGN_CENTER_VERTICAL );

    cardSizer->Add( m_promptLabel, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP( 16 ) );
    cardSizer->Add( m_prompt, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP( 16 ) );
    cardSizer->Add( generateRow, 0, wxEXPAND | wxALL, FromDIP( 16 ) );
    m_card->SetSizer( cardSizer );

    column->Add( m_card, 0, wxEXPAND | wxBOTTOM, FromDIP( 16 ) );

    // Path 2: your Cably projects (sign in, list, open) -----------------------------------
    m_cloudCard = new wxPanel( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE );
    wxBoxSizer* cloudSizer = new wxBoxSizer( wxVERTICAL );

    wxBoxSizer* cloudHeader = new wxBoxSizer( wxHORIZONTAL );
    m_cloudLabel = new wxStaticText( m_cloudCard, wxID_ANY, wxEmptyString );
    m_signIn = new wxButton( m_cloudCard, wxID_ANY, wxEmptyString );
    m_cancelSignIn = new wxButton( m_cloudCard, wxID_ANY, wxEmptyString );
    m_refreshProjects = new wxButton( m_cloudCard, wxID_ANY, wxEmptyString );
    m_signOut = new wxButton( m_cloudCard, wxID_ANY, wxEmptyString );
    cloudHeader->Add( m_cloudLabel, 1, wxALIGN_CENTER_VERTICAL );
    cloudHeader->Add( m_signIn, 0, wxALIGN_CENTER_VERTICAL );
    cloudHeader->Add( m_cancelSignIn, 0, wxALIGN_CENTER_VERTICAL );
    cloudHeader->Add( m_refreshProjects, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 6 ) );
    cloudHeader->Add( m_signOut, 0, wxALIGN_CENTER_VERTICAL );

    m_cloudStatus = new wxStaticText( m_cloudCard, wxID_ANY, wxEmptyString );
    m_cloudExplain = new wxStaticText( m_cloudCard, wxID_ANY, wxEmptyString );
    m_cloudExplain->Wrap( columnWidth - FromDIP( 40 ) );

    m_projects = new wxScrolledWindow( m_cloudCard, wxID_ANY, wxDefaultPosition,
                                       wxSize( -1, FromDIP( 190 ) ), wxVSCROLL );
    m_projects->SetScrollRate( 0, FromDIP( 8 ) );
    m_projects->SetSizer( new wxBoxSizer( wxVERTICAL ) );
    m_projectsEmpty = new wxStaticText( m_cloudCard, wxID_ANY, wxEmptyString );

    cloudSizer->Add( cloudHeader, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP( 16 ) );
    cloudSizer->Add( m_cloudStatus, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP( 16 ) );
    cloudSizer->Add( m_cloudExplain, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP( 16 ) );
    cloudSizer->Add( m_projects, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP( 16 ) );
    cloudSizer->Add( m_projectsEmpty, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP( 16 ) );

    // F5: the sync line ("Synced to Cably · 12:04 · …", or the error) + Details / Retry.
    wxBoxSizer* syncRow = new wxBoxSizer( wxHORIZONTAL );
    m_syncStatus = new wxStaticText( m_cloudCard, wxID_ANY, wxEmptyString );
    m_syncDetails = new wxButton( m_cloudCard, wxID_ANY, wxEmptyString );
    m_syncRetry = new wxButton( m_cloudCard, wxID_ANY, wxEmptyString );
    syncRow->Add( m_syncStatus, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 12 ) );
    syncRow->Add( m_syncDetails, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 6 ) );
    syncRow->Add( m_syncRetry, 0, wxALIGN_CENTER_VERTICAL );
    cloudSizer->Add( syncRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP( 16 ) );

    cloudSizer->AddSpacer( FromDIP( 16 ) );
    m_cloudCard->SetSizer( cloudSizer );

    column->Add( m_cloudCard, 0, wxEXPAND | wxBOTTOM, FromDIP( 20 ) );

    // Recent projects --------------------------------------------------------------------
    wxBoxSizer* recentRow = new wxBoxSizer( wxHORIZONTAL );
    m_recentLabel = new wxStaticText( this, wxID_ANY, wxEmptyString );
    m_open = new wxButton( this, wxID_ANY, wxEmptyString );
    recentRow->Add( m_recentLabel, 1, wxALIGN_CENTER_VERTICAL );
    recentRow->Add( m_open, 0, wxALIGN_CENTER_VERTICAL );
    column->Add( recentRow, 0, wxEXPAND | wxBOTTOM, FromDIP( 8 ) );

    m_recentList = new wxListBox( this, wxID_ANY, wxDefaultPosition,
                                  wxSize( -1, FromDIP( 130 ) ), 0, nullptr,
                                  wxLB_SINGLE | wxLB_NEEDED_SB );
    m_recentEmpty = new wxStaticText( this, wxID_ANY, wxEmptyString );
    column->Add( m_recentList, 0, wxEXPAND );
    column->Add( m_recentEmpty, 0, wxTOP, FromDIP( 6 ) );

    // Footer -----------------------------------------------------------------------------
    m_footer = new wxStaticText( this, wxID_ANY, wxEmptyString );
    m_footer->SetCursor( wxCursor( wxCURSOR_HAND ) );

    outer->AddStretchSpacer( 1 );
    outer->Add( column, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP( 32 ) );
    outer->AddStretchSpacer( 2 );
    outer->Add( m_footer, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP( 14 ) );
    SetSizer( outer );

    // Fonts (KIUI keeps the platform face and icon-scale rules).
    wxFont control = KIUI::GetControlFont( this );
    m_title->SetFont( control.Scaled( 2.2f ).Bold() );
    m_tagline->SetFont( control.Larger() );
    m_promptLabel->SetFont( control.Bold() );
    m_cloudLabel->SetFont( control.Larger().Bold() );
    m_recentLabel->SetFont( control.Larger().Bold() );
    m_generateHint->SetFont( KIUI::GetInfoFont( this ) );
    m_cloudStatus->SetFont( control );
    m_cloudExplain->SetFont( KIUI::GetInfoFont( this ) );
    m_projectsEmpty->SetFont( KIUI::GetInfoFont( this ) );
    m_syncStatus->SetFont( KIUI::GetInfoFont( this ) );
    m_recentEmpty->SetFont( KIUI::GetInfoFont( this ) );
    m_footer->SetFont( KIUI::GetInfoFont( this ) );

    m_syncDetails->Bind( wxEVT_BUTTON, &CABLY_HOME_PANEL::onSyncDetails, this );
    m_syncRetry->Bind( wxEVT_BUTTON, &CABLY_HOME_PANEL::onSyncRetry, this );
    m_generate->Bind( wxEVT_BUTTON, &CABLY_HOME_PANEL::onGenerate, this );
    m_signIn->Bind( wxEVT_BUTTON, &CABLY_HOME_PANEL::onSignIn, this );
    m_cancelSignIn->Bind( wxEVT_BUTTON, &CABLY_HOME_PANEL::onCancelSignIn, this );
    m_signOut->Bind( wxEVT_BUTTON, &CABLY_HOME_PANEL::onSignOut, this );
    m_refreshProjects->Bind( wxEVT_BUTTON, &CABLY_HOME_PANEL::onRefreshProjects, this );
    m_open->Bind( wxEVT_BUTTON, &CABLY_HOME_PANEL::onOpenProject, this );
    m_recentList->Bind( wxEVT_LISTBOX_DCLICK, &CABLY_HOME_PANEL::onRecentActivated, this );
    m_footer->Bind( wxEVT_LEFT_UP, &CABLY_HOME_PANEL::onAboutClicked, this );
}


void CABLY_HOME_PANEL::updateLabels()
{
    // Trademark: the product is "Cably Desktop", based on KiCad.
    m_title->SetLabel( _( "Cably Desktop" ) );
    m_tagline->SetLabel( _( "Describe a circuit on cably.dev, then open the schematic and board in the editors here." ) );

    m_promptLabel->SetLabel( _( "Describe the circuit you want" ) );
    m_prompt->SetHint( _( "e.g. a 5 V USB-powered blinking LED with a 555 timer" ) );
    m_generate->SetLabel( _( "Generate" ) );
    m_generate->SetToolTip( _( "Generates on cably.dev; open the result here afterwards" ) );
    m_generateHint->SetLabel( _( "Generates on cably.dev in your browser. Sign in below to open the result here." ) );

    m_cloudLabel->SetLabel( _( "Your Cably projects" ) );
    m_signIn->SetLabel( _( "Sign in to Cably" ) );
    m_signIn->SetToolTip( _( "Opens cably.dev in your browser; the sign-in comes back to this app on 127.0.0.1" ) );
    m_cancelSignIn->SetLabel( _( "Cancel" ) );
    m_refreshProjects->SetLabel( _( "Refresh" ) );
    m_signOut->SetLabel( _( "Sign out" ) );
    m_signOut->SetToolTip( _( "Forgets the Cably session stored in your keychain" ) );
    m_cloudExplain->SetLabel( _( "Sign in to open the circuits you generated on cably.dev: each one exports straight into the editors here, into ~/Documents/Cably Desktop." ) );
    m_cloudExplain->Wrap( FromDIP( 600 ) );
    m_projectsEmpty->SetLabel( _( "No projects yet. Generate one above, then Refresh." ) );
    m_syncDetails->SetLabel( _( "Details" ) );
    m_syncDetails->SetToolTip( _( "The full report of the last sync to Cably" ) );
    m_syncRetry->SetLabel( _( "Retry" ) );
    m_syncRetry->SetToolTip( _( "Send the last KiCad save to Cably again" ) );

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
    m_cloudCard->SetBackgroundColour( p.card );
    m_projects->SetBackgroundColour( p.card );

    m_title->SetForegroundColour( p.primary );
    m_tagline->SetForegroundColour( p.muted );
    m_promptLabel->SetForegroundColour( p.foreground );
    m_prompt->SetBackgroundColour( p.card );
    m_prompt->SetForegroundColour( p.foreground );
    m_generateHint->SetForegroundColour( p.accent );
    m_cloudLabel->SetForegroundColour( p.foreground );
    m_cloudStatus->SetForegroundColour( m_cloudStatusIsError ? p.destructive : p.foreground );
    m_cloudExplain->SetForegroundColour( p.muted );
    m_projectsEmpty->SetForegroundColour( p.muted );
    m_syncStatus->SetForegroundColour( m_syncStatusIsError ? p.destructive : p.muted );
    m_recentLabel->SetForegroundColour( p.foreground );
    m_recentList->SetBackgroundColour( p.card );
    m_recentList->SetForegroundColour( p.foreground );
    m_recentEmpty->SetForegroundColour( p.muted );
    m_footer->SetForegroundColour( p.primary );

    for( const CABLY_HOME_CLOUD_STATE::ROW& row : m_cloud->rows )
    {
        row.name->SetForegroundColour( p.foreground );
        row.updated->SetForegroundColour( p.muted );
    }

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
    setCloudUi( m_cloudUi );
    rebuildProjectRows();
    RefreshRecentProjects();
}


// --- cloud card ---------------------------------------------------------------------------

void CABLY_HOME_PANEL::setCloudUi( CLOUD_UI aUi )
{
    m_cloudUi = aUi;

    const bool out = aUi == CLOUD_UI::SIGNED_OUT;
    const bool waiting = aUi == CLOUD_UI::WAITING;
    const bool in = aUi == CLOUD_UI::SIGNED_IN;

    m_signIn->Show( out );
    m_cancelSignIn->Show( waiting );
    m_refreshProjects->Show( in );
    m_signOut->Show( in );
    m_cloudExplain->Show( out );
    m_projects->Show( in && !m_cloud->projects.empty() );
    m_projectsEmpty->Show( in && m_cloud->projects.empty() && !m_cloud->busy );

    switch( aUi )
    {
    case CLOUD_UI::SIGNED_OUT:
        setCloudStatus( wxEmptyString, false );
        break;

    case CLOUD_UI::WAITING:
        setCloudStatus( _( "Waiting for the browser…" ), false );
        break;

    case CLOUD_UI::SIGNED_IN:
        setCloudStatus( wxString::Format( _( "Signed in as %s" ),
                                          wxString::FromUTF8( m_cloud->email.c_str() ) ),
                        false );
        break;
    }

    Layout();
}


void CABLY_HOME_PANEL::setCloudStatus( const wxString& aText, bool aIsError )
{
    m_cloudStatusIsError = aIsError;
    m_cloudStatus->SetLabel( aText );
    m_cloudStatus->Show( !aText.IsEmpty() );

    const CABLY_PALETTE p = cablyPalette( KIPLATFORM::UI::IsDarkTheme() );
    m_cloudStatus->SetForegroundColour( aIsError ? p.destructive : p.foreground );
    m_cloudStatus->Refresh();
    Layout();
}


void CABLY_HOME_PANEL::rebuildProjectRows()
{
    const CABLY_PALETTE p = cablyPalette( KIPLATFORM::UI::IsDarkTheme() );
    wxSizer*            sizer = m_projects->GetSizer();
    const wxDateTime    now = wxDateTime::Now();

    sizer->Clear( true );   // destroys the previous row widgets
    m_cloud->rows.clear();

    for( size_t i = 0; i < m_cloud->projects.size(); ++i )
    {
        const CLOUD_PROJECT& project = m_cloud->projects[i];
        wxBoxSizer*          row = new wxBoxSizer( wxHORIZONTAL );

        wxString name = wxString::FromUTF8( project.name.c_str() );

        if( name.IsEmpty() )
            name = _( "Untitled project" );

        wxDateTime updated;
        wxString   when = wxString::FromUTF8( project.updatedAt.c_str() );

        if( CablyParseIsoUtc( when, updated ) )
            when = CablyFormatUpdated( updated, now );

        if( !when.IsEmpty() )
            when = wxString::Format( _( "updated %s" ), when );

        CABLY_HOME_CLOUD_STATE::ROW widgets;
        widgets.name = new wxStaticText( m_projects, wxID_ANY, name );
        widgets.updated = new wxStaticText( m_projects, wxID_ANY, when );
        widgets.open = new wxButton( m_projects, wxID_ANY, _( "Open" ) );

        widgets.name->SetFont( KIUI::GetControlFont( this ).Bold() );
        widgets.updated->SetFont( KIUI::GetInfoFont( this ) );
        widgets.name->SetForegroundColour( p.foreground );
        widgets.updated->SetForegroundColour( p.muted );
        widgets.open->SetToolTip( _( "Export from Cably and open it in the editors here" ) );
        widgets.open->Enable( !m_cloud->busy );

        widgets.open->Bind( wxEVT_BUTTON,
                            [this, i]( wxCommandEvent& )
                            {
                                openProject( i );
                            } );

        row->Add( widgets.name, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 12 ) );
        row->Add( widgets.updated, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 12 ) );
        row->Add( widgets.open, 0, wxALIGN_CENTER_VERTICAL );
        sizer->Add( row, 0, wxEXPAND | wxBOTTOM, FromDIP( 6 ) );

        m_cloud->rows.push_back( widgets );
    }

    m_projects->FitInside();
    m_projects->Layout();
    m_projects->Show( m_cloudUi == CLOUD_UI::SIGNED_IN && !m_cloud->projects.empty() );
    m_projectsEmpty->Show( m_cloudUi == CLOUD_UI::SIGNED_IN && m_cloud->projects.empty()
                           && !m_cloud->busy );
    Layout();
}


void CABLY_HOME_PANEL::setProjectsBusy( bool aBusy )
{
    m_cloud->busy = aBusy;

    for( const CABLY_HOME_CLOUD_STATE::ROW& row : m_cloud->rows )
        row.open->Enable( !aBusy );

    m_refreshProjects->Enable( !aBusy );
    m_signOut->Enable( !aBusy );
}


void CABLY_HOME_PANEL::restoreSession()
{
    {
        std::lock_guard<std::mutex> lock( m_cloud->bridgeMutex );

        if( !m_cloud->bridge.LoadSession() )
            return;

        m_cloud->email = m_cloud->bridge.Session().email;
    }

    // Show the remembered identity at once, then confirm it off the UI thread (the bridge
    // refreshes the token on a 401); a dead session falls back to the signed-out card, a
    // network failure keeps the identity and reports it.
    m_cloud->signedIn = true;
    setCloudUi( CLOUD_UI::SIGNED_IN );
    setProjectsBusy( true );
    setCloudStatus( wxString::Format( _( "Signed in as %s — checking the session…" ),
                                      wxString::FromUTF8( m_cloud->email.c_str() ) ),
                    false );

    const unsigned          serial = m_cloud->serial;
    CABLY_HOME_CLOUD_STATE* cloud = m_cloud.get();

    cablyRunAsync<LOGIN_RESULT>(
            m_cloud->live, this,
            [cloud]()
            {
                std::lock_guard<std::mutex> lock( cloud->bridgeMutex );
                LOGIN_RESULT                r;
                r.ok = cloud->bridge.ValidateSession();
                r.sessionGone = !cloud->bridge.HasSession();
                r.email = cloud->bridge.Session().email;
                r.error = cloud->bridge.LastError();
                return r;
            },
            [this, serial]( const LOGIN_RESULT& aResult )
            {
                if( serial != m_cloud->serial )
                    return;

                setProjectsBusy( false );

                if( aResult.ok )
                {
                    m_cloud->email = aResult.email;
                    setCloudUi( CLOUD_UI::SIGNED_IN );
                    loadProjects();
                    return;
                }

                if( aResult.sessionGone )
                {
                    m_cloud->signedIn = false;
                    m_cloud->email.clear();
                    m_cloud->projects.clear();
                    rebuildProjectRows();
                    setCloudUi( CLOUD_UI::SIGNED_OUT );
                    setCloudStatus( wxString::Format( _( "Your Cably session has expired (%s). Sign in again." ),
                                                      wxString::FromUTF8( aResult.error.c_str() ) ),
                                    true );
                    return;
                }

                setCloudUi( CLOUD_UI::SIGNED_IN );
                setCloudStatus( wxString::Format( _( "Couldn't check your Cably session: %s" ),
                                                  wxString::FromUTF8( aResult.error.c_str() ) ),
                                true );
            } );
}


void CABLY_HOME_PANEL::loadProjects()
{
    if( !m_cloud->signedIn || m_cloud->busy )
        return;

    setProjectsBusy( true );
    m_projectsEmpty->Hide();
    setCloudStatus( _( "Loading your Cably projects…" ), false );

    const unsigned          serial = m_cloud->serial;
    CABLY_HOME_CLOUD_STATE* cloud = m_cloud.get();

    cablyRunAsync<LIST_RESULT>(
            m_cloud->live, this,
            [cloud]()
            {
                std::lock_guard<std::mutex> lock( cloud->bridgeMutex );
                LIST_RESULT                 r;
                r.ok = cloud->bridge.ListProjects( r.projects );
                r.sessionGone = !cloud->bridge.HasSession();
                r.error = cloud->bridge.LastError();
                return r;
            },
            [this, serial]( const LIST_RESULT& aResult )
            {
                if( serial != m_cloud->serial )
                    return;

                setProjectsBusy( false );

                if( aResult.sessionGone )
                {
                    dropSession( wxString::FromUTF8( aResult.error.c_str() ) );
                    return;
                }

                if( aResult.ok )
                    m_cloud->projects = aResult.projects;   // server order: updated_at desc

                rebuildProjectRows();
                setCloudUi( CLOUD_UI::SIGNED_IN );

                if( !aResult.ok )
                {
                    setCloudStatus( wxString::Format( _( "Couldn't load your projects: %s" ),
                                                      wxString::FromUTF8( aResult.error.c_str() ) ),
                                    true );
                }
            } );
}


void CABLY_HOME_PANEL::dropSession( const wxString& aReason )
{
    ++m_cloud->serial;
    m_cloud->signedIn = false;
    m_cloud->email.clear();
    m_cloud->projects.clear();
    m_cloud->busy = false;
    rebuildProjectRows();
    setCloudUi( CLOUD_UI::SIGNED_OUT );

    if( !aReason.IsEmpty() )
    {
        setCloudStatus( wxString::Format( _( "Your Cably session has expired (%s). Sign in again." ), aReason ),
                        true );
    }
}


void CABLY_HOME_PANEL::openProject( size_t aIndex )
{
    if( m_cloud->busy || aIndex >= m_cloud->projects.size() )
        return;

    const CLOUD_PROJECT project = m_cloud->projects[aIndex];
    const wxString      name = wxString::FromUTF8( project.name.c_str() );
    const wxString      root = cablyProjectsRoot();
    const std::string   rootUtf8( root.ToUTF8() );
    const unsigned      serial = m_cloud->serial;
    CABLY_HOME_CLOUD_STATE* cloud = m_cloud.get();

    setProjectsBusy( true );
    setCloudStatus( wxString::Format( _( "Opening \"%s\": fetching, exporting through the Cably engine, writing %s…" ),
                                      name, root + wxFileName::GetPathSeparator() + CablyProjectStem( name ) ),
                    false );

    cablyRunAsync<OPEN_RESULT>(
            m_cloud->live, this,
            [cloud, project, rootUtf8]()
            {
                return cloudFetchExportWrite( cloud, project, rootUtf8, false );
            },
            [this, serial, name, project, rootUtf8]( const OPEN_RESULT& aResult )
            {
                if( serial != m_cloud->serial )
                    return;

                setProjectsBusy( false );

                // F5: remember which cloud project this folder is, so a save there can
                // be reloaded from Cably without a second lookup.
                auto remember = [this, project]( const wxString& aProPath )
                {
                    m_sync->opened[wxFileName( aProPath ).GetPath()] = project;
                };

                if( aResult.sessionGone )
                {
                    dropSession( wxString::FromUTF8( aResult.error.c_str() ) );
                    return;
                }

                if( !aResult.ok )
                {
                    setCloudStatus( wxString::Format( _( "Couldn't open \"%s\": %s" ), name,
                                                      wxString::FromUTF8( aResult.error.c_str() ) ),
                                    true );
                    return;
                }

                if( aResult.status == CLOUD_WRITE::WRITTEN )
                {
                    remember( aResult.proPath );
                    finishOpen( aResult.proPath );
                    return;
                }

                // CONFLICT: mirrors the web app's desktop button (Keep KiCad's copy / Replace).
                wxMessageDialog dlg( this,
                                     _( "This project was edited here since Cably last exported it." ),
                                     _( "Open Cably project" ),
                                     wxYES_NO | wxCANCEL | wxICON_QUESTION | wxCENTRE );
                dlg.SetExtendedMessage( wxString::Format(
                        _( "Keep opens the copy in %s as it is. Replace overwrites its board and schematic with Cably's latest export." ),
                        aResult.dir ) );
                dlg.SetYesNoCancelLabels( _( "&Replace" ), _( "&Keep" ), _( "Cancel" ) );

                const int answer = dlg.ShowModal();

                if( answer == wxID_NO )
                {
                    remember( aResult.proPath );
                    finishOpen( aResult.proPath );
                    return;
                }

                if( answer != wxID_YES )
                {
                    setCloudUi( CLOUD_UI::SIGNED_IN );
                    return;
                }

                std::shared_ptr<CLOUD_EXPORT> exported = aResult.exported;
                const unsigned                serial2 = m_cloud->serial;

                setProjectsBusy( true );
                setCloudStatus( wxString::Format( _( "Replacing \"%s\"…" ), name ), false );

                cablyRunAsync<OPEN_RESULT>(
                        m_cloud->live, this,
                        [exported, project, rootUtf8]()
                        {
                            OPEN_RESULT        r;
                            CABLY_WRITE_RESULT w = CABLY_BRIDGE::WriteProjectFolder(
                                    rootUtf8, project.name, exported->kicadPcb,
                                    exported->hasSch ? exported->kicadSch : std::string(), true );

                            r.status = cloudWriteStatus( w );
                            r.ok = r.status == CLOUD_WRITE::WRITTEN;
                            r.error = w.error;
                            r.proPath = wxString::FromUTF8( w.proPath.c_str() );
                            return r;
                        },
                        [this, serial2, name, remember]( const OPEN_RESULT& aReplace )
                        {
                            if( serial2 != m_cloud->serial )
                                return;

                            setProjectsBusy( false );

                            if( !aReplace.ok )
                            {
                                setCloudStatus( wxString::Format( _( "Couldn't replace \"%s\": %s" ), name,
                                                                  wxString::FromUTF8( aReplace.error.c_str() ) ),
                                                true );
                                return;
                            }

                            remember( aReplace.proPath );
                            finishOpen( aReplace.proPath );
                        } );
            } );
}


void CABLY_HOME_PANEL::finishOpen( const wxString& aProPath )
{
    setCloudUi( CLOUD_UI::SIGNED_IN );

    // Deferred: LoadProject hides this panel (updateCablyHomeVisibility) and the button
    // that started all this should finish its own event first.
    const wxFileName pro( aProPath );

    CallAfter(
            [this, pro]()
            {
                m_frame->LoadProject( pro );
            } );
}


// --- events -------------------------------------------------------------------------------

void CABLY_HOME_PANEL::onGenerate( wxCommandEvent& aEvent )
{
    // Generation stays on the web for now: the prompt travels as ?prompt= and the Index
    // page prefills its chat box.  The result is opened here from "Your Cably projects".
    const wxString url = CablyGenerateUrl( wxString( CABLY_APP_URL ), m_prompt->GetValue() );

    if( !wxLaunchDefaultBrowser( url ) )
        m_generateHint->SetLabel( wxString::Format( _( "Couldn't open a browser. Open %s yourself." ), url ) );
}


void CABLY_HOME_PANEL::onSignIn( wxCommandEvent& aEvent )
{
    if( m_cloudUi == CLOUD_UI::WAITING )
        return;

    CABLY_HOME_CLOUD_STATE* cloud = m_cloud.get();
    std::string             url;

    {
        // A just-cancelled wait may still hold the bridge for up to one slice.
        std::lock_guard<std::mutex> lock( cloud->bridgeMutex );

        if( cloud->bridge.StartLoopback() == 0 )
        {
            setCloudStatus( wxString::Format( _( "Couldn't start the sign-in: %s" ),
                                              wxString::FromUTF8( cloud->bridge.LastError().c_str() ) ),
                            true );
            return;
        }

        url = cloud->bridge.AuthUrl();
    }

    cloud->cancelLogin = false;
    setCloudUi( CLOUD_UI::WAITING );

    const wxString wxUrl = wxString::FromUTF8( url.c_str() );

    if( !wxLaunchDefaultBrowser( wxUrl ) )
    {
        setCloudStatus( wxString::Format( _( "Couldn't open a browser. Open %s yourself, then return here." ), wxUrl ),
                        true );
    }

    const unsigned serial = ++cloud->serial;

    cablyRunAsync<LOGIN_RESULT>(
            m_cloud->live, this,
            [cloud]()
            {
                std::lock_guard<std::mutex> lock( cloud->bridgeMutex );
                LOGIN_RESULT                r;
                CABLY_LOOPBACK_SERVER*      loopback = cloud->bridge.Loopback();

                if( !loopback )
                {
                    r.error = "No sign-in in progress.";
                    return r;
                }

                // Sliced wait so Cancel is honoured within LOGIN_SLICE_MS without another
                // thread touching the listener.
                for( int waited = 0; waited < LOGIN_TIMEOUT_MS; waited += LOGIN_SLICE_MS )
                {
                    if( cloud->cancelLogin )
                    {
                        cloud->bridge.CancelLoopback();
                        r.error = "Cancelled.";
                        return r;
                    }

                    CABLY_SESSION session;

                    if( loopback->WaitForSession( LOGIN_SLICE_MS, session ) )
                    {
                        // Already accepted: persists to the keychain and stops the listener.
                        r.ok = cloud->bridge.FinishLoopback( 0 );
                        r.email = cloud->bridge.Session().email;
                        r.error = cloud->bridge.LastError();
                        return r;
                    }
                }

                cloud->bridge.CancelLoopback();
                r.error = "The browser did not complete the sign-in in time.";
                return r;
            },
            [this, serial]( const LOGIN_RESULT& aResult )
            {
                if( serial != m_cloud->serial )
                    return;   // cancelled meanwhile

                if( !aResult.ok )
                {
                    setCloudUi( CLOUD_UI::SIGNED_OUT );
                    setCloudStatus( wxString::Format( _( "Sign-in failed: %s" ),
                                                      wxString::FromUTF8( aResult.error.c_str() ) ),
                                    true );
                    return;
                }

                m_cloud->signedIn = true;
                m_cloud->email = aResult.email;
                setCloudUi( CLOUD_UI::SIGNED_IN );
                loadProjects();
            } );
}


void CABLY_HOME_PANEL::onCancelSignIn( wxCommandEvent& aEvent )
{
    ++m_cloud->serial;
    m_cloud->cancelLogin = true;   // the waiting worker stops the listener itself
    setCloudUi( CLOUD_UI::SIGNED_OUT );
}


void CABLY_HOME_PANEL::onSignOut( wxCommandEvent& aEvent )
{
    if( m_cloud->busy )
        return;

    {
        std::lock_guard<std::mutex> lock( m_cloud->bridgeMutex );
        m_cloud->bridge.SignOut();   // memory + keychain
    }

    dropSession( wxEmptyString );
}


void CABLY_HOME_PANEL::onRefreshProjects( wxCommandEvent& aEvent )
{
    loadProjects();
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


// --- sync (F5) ----------------------------------------------------------------------------

void CABLY_HOME_PANEL::NotifyProjectState()
{
    wxString dir;

    if( m_frame->IsProjectActive() )
    {
        const wxString pro = m_frame->GetProjectFileName();

        if( !pro.IsEmpty() )
            dir = wxFileName( pro ).GetPath();
    }

    if( dir == m_sync->dir )
        return;

    stopSync();

    if( dir.IsEmpty() )
        return;

    // Only a folder the bridge exported into is a Cably project: no sidecar, no sync.
    if( !wxFileName::FileExists( CablySidecarPath( dir ) ) )
        return;

    startSync( dir );
}


void CABLY_HOME_PANEL::startSync( const wxString& aProjectDir )
{
    std::shared_ptr<CABLY_HOME_LIVENESS> live = m_cloud->live;
    std::string                          error;

    // The watcher calls back on its own thread with the whole saved document; hand it to
    // the UI thread under the liveness lock exactly like cablyRunAsync does.
    const bool ok = m_sync->watch.Start(
            std::string( aProjectDir.ToUTF8() ),
            [live, this]( const SAVE_EVENT& aEvent )
            {
                std::lock_guard<std::mutex> lock( live->mutex );

                if( !live->alive )
                    return;

                std::shared_ptr<const SAVE_EVENT> event = std::make_shared<const SAVE_EVENT>( aEvent );

                CallAfter(
                        [this, event]()
                        {
                            onProjectFileSaved( event );
                        } );
            },
            CABLY_WATCH_OPTIONS(), &error );

    if( !ok )
    {
        setSyncStatus( wxString::Format( _( "Couldn't watch %s for saves: %s" ), aProjectDir,
                                         wxString::FromUTF8( error.c_str() ) ),
                       true, false, false );
        return;
    }

    m_sync->dir = aProjectDir;
    m_sync->pending.clear();
    setSyncStatus( wxString::Format( _( "Saves in %s sync to Cably." ), aProjectDir ), false, false, false );
}


void CABLY_HOME_PANEL::stopSync()
{
    if( m_sync->dir.IsEmpty() )
        return;

    m_sync->watch.Stop();
    m_sync->dir.clear();
    m_sync->pending.clear();
}


void CABLY_HOME_PANEL::onProjectFileSaved( std::shared_ptr<const SAVE_EVENT> aEvent )
{
    if( m_sync->dir.IsEmpty() || !aEvent )
        return;   // stopped while the event was in flight

    if( m_sync->importing )
    {
        // Queue it for after the running import; a newer save of the same file
        // replaces the queued one (the watcher already coalesced bursts per path).
        for( std::shared_ptr<const SAVE_EVENT>& queued : m_sync->pending )
        {
            if( queued->path == aEvent->path )
            {
                queued = aEvent;
                return;
            }
        }

        m_sync->pending.push_back( aEvent );
        return;
    }

    m_sync->lastEvent = aEvent;
    runImport( m_sync->dir, false );
}


void CABLY_HOME_PANEL::runImport( const wxString& aProjectDir, bool aForce )
{
    if( m_sync->importing || !m_sync->lastEvent )
        return;

    m_sync->importing = true;
    m_sync->lastDir = aProjectDir;

    const unsigned                    serial = ++m_sync->serial;
    CABLY_HOME_CLOUD_STATE*           cloud = m_cloud.get();
    const std::string                 dirUtf8( aProjectDir.ToUTF8() );
    std::shared_ptr<const SAVE_EVENT> event = m_sync->lastEvent;

    setSyncStatus( aForce ? _( "Sending your KiCad edits to Cably (overwriting the cloud version)…" )
                          : _( "Syncing your save to Cably…" ),
                   false, false, false );

    cablyRunAsync<SYNC_RESULT>(
            m_cloud->live, this,
            [cloud, dirUtf8, event, aForce]()
            {
                std::lock_guard<std::mutex> lock( cloud->bridgeMutex );
                SYNC_RESULT                 r;

                if( !cloud->bridge.HasSession() )
                {
                    r.sessionGone = true;
                    r.result.error = "not signed in";
                    return r;
                }

                if( aForce && !syncForgetCloudVersion( dirUtf8, r.result.error ) )
                    return r;

                r.result = CablySyncSave( cloud->bridge, dirUtf8, *event );
                r.sessionGone = !cloud->bridge.HasSession();
                return r;
            },
            [this, serial, aProjectDir]( const SYNC_RESULT& aResult )
            {
                if( serial != m_sync->serial )
                    return;

                m_sync->importing = false;

                const wxString reason = wxString::FromUTF8( aResult.result.error.c_str() );

                switch( aResult.result.outcome )
                {
                case CABLY_SYNC_OUTCOME::SYNCED:
                    showSyncResult( aProjectDir, wxDateTime::Now(), aResult.result.pcbReport,
                                    aResult.result.schReport );
                    break;

                case CABLY_SYNC_OUTCOME::CLOUD_CHANGED:
                    askCloudChanged( aProjectDir );
                    break;

                case CABLY_SYNC_OUTCOME::NOT_CABLY_PROJECT:
                    // The sidecar names no cloud row (an export from before F5): nothing to
                    // sync to.  Stop watching; "Open" from the projects list writes a full one.
                    stopSync();
                    showSyncError( aProjectDir,
                                   _( "this folder doesn't say which Cably project it came from; open the project from the list on the home screen again" ) );
                    break;

                case CABLY_SYNC_OUTCOME::FAILED:
                    if( aResult.sessionGone && m_cloud->signedIn )
                        dropSession( reason );   // the bridge lost the session (dead refresh token)

                    showSyncError( aProjectDir,
                                   aResult.sessionGone ? _( "sign in on the home screen first" ) : reason );
                    break;
                }

                if( !m_sync->importing && !m_sync->dir.IsEmpty() && !m_sync->pending.empty() )
                {
                    std::shared_ptr<const SAVE_EVENT> next = m_sync->pending.front();
                    m_sync->pending.erase( m_sync->pending.begin() );
                    m_sync->lastEvent = next;
                    runImport( m_sync->dir, false );
                }
            } );
}


void CABLY_HOME_PANEL::showSyncResult( const wxString& aProjectDir, const wxDateTime& aWhen,
                                       const std::string& aPcbReportJson, const std::string& aSchReportJson )
{
    const CABLY_SYNC_SUMMARY summary = CablySummariseImport( aPcbReportJson, aSchReportJson );
    const wxString           line = CablySyncStatusLine( aWhen, summary.headline );
    const wxString           details = line + wxS( "\n" ) + aProjectDir + wxS( "\n\n" ) + summary.details;

    m_sync->lastDetails = details;
    setSyncStatus( line, !summary.ok, true, false );

    // The manager's non-modal notification: the line as its title, the dropped items
    // (or the headline) under it, and "View Details" opening the full report text,
    // written next to the sidecar (it is not a board or schematic, so the watcher
    // ignores it).
    wxString href;
    {
        const wxString reportPath = CablySyncReportPath( aProjectDir );
        wxFFile        file( reportPath, wxS( "w" ) );

        if( file.IsOpened() && file.Write( details ) )
            href = wxFileSystem::FileNameToURL( wxFileName( reportPath ) );
    }

    wxString description = CablySyncDroppedDigest( summary );

    if( description.IsEmpty() )
    {
        description = summary.headline.IsEmpty() ? _( "Everything in your save fits the Cably project." )
                                                 : summary.headline;
    }

    notifyManager( line, description, href );
}


void CABLY_HOME_PANEL::showSyncError( const wxString& aProjectDir, const wxString& aReason )
{
    const wxString line = wxString::Format( _( "Couldn't sync to Cably: %s" ), aReason );

    m_sync->lastDetails = line + wxS( "\n" ) + aProjectDir;
    setSyncStatus( line, true, false, true );
    notifyManager( _( "Couldn't sync to Cably" ),
                   wxString::Format( _( "%s — save again to retry, or use Retry on the home screen." ), aReason ),
                   wxEmptyString );
}


void CABLY_HOME_PANEL::askCloudChanged( const wxString& aProjectDir )
{
    setSyncStatus( _( "This project changed on cably.dev since it was opened here." ), true, false, true );

    // Parented to the manager frame: this panel is hidden while the project is loaded.
    wxMessageDialog dlg( m_frame, _( "This project changed on cably.dev since it was opened here." ),
                         _( "Sync to Cably" ), wxYES_NO | wxCANCEL | wxICON_QUESTION | wxCENTRE );
    dlg.SetExtendedMessage( wxString::Format(
            _( "Keep sends your KiCad edits in %s to Cably and overwrites the cloud version. "
               "Discard closes the project here, fetches Cably's version and replaces the board and schematic with it." ),
            aProjectDir ) );
    dlg.SetYesNoCancelLabels( _( "Keep my KiCad edits (overwrite cloud)" ),
                              _( "Discard my edits (reload from Cably)" ), _( "Cancel" ) );

    const int answer = dlg.ShowModal();

    if( answer == wxID_YES )
    {
        runImport( aProjectDir, true );   // forces the PATCH
        return;
    }

    if( answer == wxID_NO )
    {
        reloadFromCloud( aProjectDir );
        return;
    }

    setSyncStatus( _( "Not synced: this project changed on cably.dev since it was opened here. Save again to decide." ),
                   true, false, true );
}


void CABLY_HOME_PANEL::reloadFromCloud( const wxString& aProjectDir )
{
    // Which cloud project?  The one this folder was opened from in this session, else the
    // listed project whose folder stem matches the folder name.
    CLOUD_PROJECT project;
    bool          found = false;
    auto          it = m_sync->opened.find( aProjectDir );

    if( it != m_sync->opened.end() )
    {
        project = it->second;
        found = true;
    }
    else
    {
        const wxArrayString dirs = wxFileName::DirName( aProjectDir ).GetDirs();
        const wxString      folder = dirs.IsEmpty() ? wxString() : dirs.Last();

        for( const CLOUD_PROJECT& candidate : m_cloud->projects )
        {
            if( !folder.IsEmpty()
                && CablyProjectStem( wxString::FromUTF8( candidate.name.c_str() ) ) == folder )
            {
                project = candidate;
                found = true;
                break;
            }
        }
    }

    if( !m_cloud->signedIn )
    {
        showSyncError( aProjectDir, _( "sign in on the home screen first" ) );
        return;
    }

    if( !found )
    {
        showSyncError( aProjectDir,
                       _( "couldn't tell which Cably project this folder came from; Refresh the projects list and open it from there" ) );
        return;
    }

    stopSync();

    // The editors close (asking about unsaved changes) and this panel comes back.
    if( !m_frame->CloseProject( true ) )
    {
        setSyncStatus( _( "Reload from Cably cancelled: the project stayed open." ), true, false, false );
        NotifyProjectState();
        return;
    }

    const wxString          name = wxString::FromUTF8( project.name.c_str() );
    const std::string       rootUtf8( cablyProjectsRoot().ToUTF8() );
    const unsigned          serial = m_cloud->serial;
    CABLY_HOME_CLOUD_STATE* cloud = m_cloud.get();

    setProjectsBusy( true );
    setCloudStatus( wxString::Format( _( "Reloading \"%s\" from Cably…" ), name ), false );
    setSyncStatus( wxString::Format( _( "Replacing %s with Cably's version…" ), aProjectDir ), false, false, false );

    // The F4 open flow with Replace: Cably's export overwrites the board and schematic.
    cablyRunAsync<OPEN_RESULT>(
            m_cloud->live, this,
            [cloud, project, rootUtf8]()
            {
                return cloudFetchExportWrite( cloud, project, rootUtf8, true );
            },
            [this, serial, name, project]( const OPEN_RESULT& aResult )
            {
                if( serial != m_cloud->serial )
                    return;

                setProjectsBusy( false );

                if( aResult.sessionGone )
                {
                    dropSession( wxString::FromUTF8( aResult.error.c_str() ) );
                    return;
                }

                if( !aResult.ok || aResult.status != CLOUD_WRITE::WRITTEN )
                {
                    const wxString reason = wxString::FromUTF8( aResult.error.c_str() );
                    setCloudStatus( wxString::Format( _( "Couldn't reload \"%s\": %s" ), name, reason ), true );
                    setSyncStatus( wxString::Format( _( "Couldn't reload from Cably: %s" ), reason ), true, false, false );
                    return;
                }

                m_sync->opened[wxFileName( aResult.proPath ).GetPath()] = project;
                setSyncStatus( wxString::Format( _( "Reloaded \"%s\" from Cably." ), name ), false, false, false );
                finishOpen( aResult.proPath );
            } );
}


void CABLY_HOME_PANEL::setSyncStatus( const wxString& aText, bool aIsError, bool aShowDetails, bool aShowRetry )
{
    m_syncStatusIsError = aIsError;
    m_syncStatus->SetLabel( aText );
    m_syncStatus->Show( !aText.IsEmpty() );
    m_syncDetails->Show( aShowDetails );
    m_syncRetry->Show( aShowRetry );

    const CABLY_PALETTE p = cablyPalette( KIPLATFORM::UI::IsDarkTheme() );
    m_syncStatus->SetForegroundColour( aIsError ? p.destructive : p.muted );
    m_syncStatus->Refresh();
    Layout();
}


void CABLY_HOME_PANEL::notifyManager( const wxString& aTitle, const wxString& aDescription, const wxString& aHref )
{
    // One notification, updated in place (the status bar bell of the manager window).
    Pgm().GetNotificationsManager().CreateOrUpdate( wxS( "cably-sync" ), aTitle, aDescription, aHref );
}


void CABLY_HOME_PANEL::onSyncDetails( wxCommandEvent& aEvent )
{
    wxDialog dlg( this, wxID_ANY, _( "Last sync to Cably" ), wxDefaultPosition, FromDIP( wxSize( 560, 420 ) ),
                  wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER );
    wxBoxSizer* sizer = new wxBoxSizer( wxVERTICAL );
    wxTextCtrl* text = new wxTextCtrl( &dlg, wxID_ANY, m_sync->lastDetails, wxDefaultPosition, wxDefaultSize,
                                       wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP );

    sizer->Add( text, 1, wxEXPAND | wxALL, FromDIP( 10 ) );
    sizer->Add( dlg.CreateButtonSizer( wxOK ), 0, wxEXPAND | wxALL, FromDIP( 10 ) );
    dlg.SetSizer( sizer );
    dlg.ShowModal();
}


void CABLY_HOME_PANEL::onSyncRetry( wxCommandEvent& aEvent )
{
    if( m_sync->lastDir.IsEmpty() || m_sync->importing )
        return;

    runImport( m_sync->lastDir, false );
}


void CABLY_HOME_PANEL::onShow( wxShowEvent& aEvent )
{
    NotifyProjectState();
    aEvent.Skip();
}


void CABLY_HOME_PANEL::onSyncTimer( wxTimerEvent& aEvent )
{
    NotifyProjectState();
}

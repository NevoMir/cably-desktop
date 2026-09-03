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
 * F5 sync: see cably_sync.h.  Plain C++17 + POSIX stat(); no wx, no KiCad types.
 */

#include <cably_sync.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include <sys/stat.h>

namespace fs = std::filesystem;


// ---------------------------------------------------------------------------------------
// classify
// ---------------------------------------------------------------------------------------

static bool endsWith( const std::string& aText, const std::string& aSuffix )
{
    return aText.size() >= aSuffix.size()
           && aText.compare( aText.size() - aSuffix.size(), aSuffix.size(), aSuffix ) == 0;
}


static bool startsWith( const std::string& aText, const std::string& aPrefix )
{
    return aText.compare( 0, aPrefix.size(), aPrefix ) == 0;
}


bool CablyClassifySavePath( const std::string& aPath, CABLY_SAVE_KIND* aKind )
{
    // Mirrors desktop/src/main/watcher.ts classifyPath, clause for clause.
    std::vector<std::string> segments;
    std::string              current;

    for( char c : aPath )
    {
        if( c == '/' || c == '\\' )
        {
            segments.push_back( current );
            current.clear();
        }
        else
        {
            current.push_back( c );
        }
    }

    segments.push_back( current );

    for( size_t i = 0; i + 1 < segments.size(); ++i )
    {
        if( endsWith( segments[i], "-backups" ) )
            return false;
    }

    const std::string& name = segments.back();

    if( name.empty() )
        return false;

    if( startsWith( name, "~" ) || startsWith( name, "_autosave-" ) || startsWith( name, "_saved_" ) )
        return false;

    if( endsWith( name, "-bak" ) || endsWith( name, ".lck" ) )
        return false;

    if( endsWith( name, ".kicad_pcb" ) )
    {
        if( aKind )
            *aKind = CABLY_SAVE_KIND::BOARD;

        return true;
    }

    if( endsWith( name, ".kicad_sch" ) )
    {
        if( aKind )
            *aKind = CABLY_SAVE_KIND::SCHEMATIC;

        return true;
    }

    return false;
}


// ---------------------------------------------------------------------------------------
// watcher
// ---------------------------------------------------------------------------------------

static bool readWholeFile( const std::string& aPath, std::string& aOut )
{
    std::ifstream in( aPath, std::ios::binary );

    if( !in )
        return false;

    aOut.assign( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
    return true;
}


CABLY_PROJECT_WATCH::CABLY_PROJECT_WATCH()
{
}


CABLY_PROJECT_WATCH::~CABLY_PROJECT_WATCH()
{
    Stop();
}


CABLY_PROJECT_WATCH::SIGNATURE CABLY_PROJECT_WATCH::signatureOf( const std::string& aPath )
{
    SIGNATURE   sig;
    struct stat st;

    if( ::stat( aPath.c_str(), &st ) != 0 || !S_ISREG( st.st_mode ) )
        return sig;

    sig.exists = true;
    sig.inode = static_cast<unsigned long long>( st.st_ino );
    sig.size = static_cast<long long>( st.st_size );
#ifdef __APPLE__
    sig.mtimeNs = static_cast<long long>( st.st_mtimespec.tv_sec ) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    sig.mtimeNs = static_cast<long long>( st.st_mtim.tv_sec ) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    return sig;
}


bool CABLY_PROJECT_WATCH::Start( const std::string& aDir, CALLBACK aOnSave,
                                 const CABLY_WATCH_OPTIONS& aOptions, std::string* aError )
{
    if( m_running )
    {
        if( aError )
            *aError = "Already watching " + m_dir;

        return false;
    }

    std::error_code ec;

    if( !fs::is_directory( aDir, ec ) )
    {
        if( aError )
            *aError = "Not a folder: " + aDir;

        return false;
    }

    m_dir = aDir;
    m_onSave = std::move( aOnSave );
    m_options = aOptions;

    if( m_options.pollMs < 1 )
        m_options.pollMs = 1;

    if( m_options.debounceMs < 0 )
        m_options.debounceMs = 0;

    {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_known.clear();
        m_sigs.clear();
        m_pending.clear();

        // Baseline 1: what the cloud last wrote or synced, from the sidecar.
        CABLY_EXPORT_SIDECAR sidecar;

        if( CABLY_EXPORT_SIDECAR::Load( m_dir, sidecar ) )
        {
            for( const auto& f : sidecar.files )
            {
                std::string path = ( fs::path( m_dir ) / f.first ).string();

                if( CablyClassifySavePath( path ) )
                    m_known[path] = f.second;
            }
        }

        // Baseline 2: every other document already in the folder, from disk (so a
        // replayed event for an unchanged file has nothing to report).  Snapshot all.
        for( const auto& entry : fs::directory_iterator( m_dir, ec ) )
        {
            std::string path = entry.path().string();

            if( !CablyClassifySavePath( path ) )
                continue;

            m_sigs[path] = signatureOf( path );

            if( m_known.count( path ) )
                continue;

            std::string text;

            if( readWholeFile( path, text ) )
                m_known[path] = CABLY_BRIDGE::Sha256Hex( text );
        }
    }

    m_stopRequested = false;
    m_running = true;
    m_thread = std::thread( [this] { threadLoop(); } );
    return true;
}


void CABLY_PROJECT_WATCH::Stop()
{
    {
        std::lock_guard<std::mutex> lock( m_wakeMutex );
        m_stopRequested = true;
    }

    m_wake.notify_all();

    if( m_thread.joinable() )
        m_thread.join();

    {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_pending.clear();
    }

    m_running = false;
}


void CABLY_PROJECT_WATCH::Expect( const std::string& aPath, const std::string& aText )
{
    std::lock_guard<std::mutex> lock( m_mutex );
    m_known[aPath] = CABLY_BRIDGE::Sha256Hex( aText );
}


std::string CABLY_PROJECT_WATCH::KnownHash( const std::string& aPath ) const
{
    std::lock_guard<std::mutex> lock( m_mutex );
    auto                        it = m_known.find( aPath );
    return it == m_known.end() ? std::string() : it->second;
}


void CABLY_PROJECT_WATCH::threadLoop()
{
    while( !m_stopRequested )
    {
        Poll();

        std::unique_lock<std::mutex> lock( m_wakeMutex );
        m_wake.wait_for( lock, std::chrono::milliseconds( m_options.pollMs ),
                         [this] { return m_stopRequested.load(); } );
    }
}


void CABLY_PROJECT_WATCH::scan( std::chrono::steady_clock::time_point aNow )
{
    std::error_code ec;
    std::map<std::string, SIGNATURE> seen;

    for( const auto& entry : fs::directory_iterator( m_dir, ec ) )
    {
        std::string path = entry.path().string();

        if( !CablyClassifySavePath( path ) )
            continue;

        seen[path] = signatureOf( path );
    }

    if( ec )
        return; // the folder vanished or is unreadable right now; try again next pass

    std::lock_guard<std::mutex> lock( m_mutex );
    const auto deadline = aNow + std::chrono::milliseconds( m_options.debounceMs );

    for( const auto& s : seen )
    {
        auto it = m_sigs.find( s.first );

        if( it == m_sigs.end() || it->second != s.second )
        {
            m_sigs[s.first] = s.second;
            m_pending[s.first] = deadline; // (re)start this path's debounce
        }
    }

    // Documents that disappeared: forget the snapshot; a later reappearance is a change.
    for( auto it = m_sigs.begin(); it != m_sigs.end(); )
    {
        if( seen.count( it->first ) )
            ++it;
        else
            it = m_sigs.erase( it );
    }
}


void CABLY_PROJECT_WATCH::Poll()
{
    const auto now = std::chrono::steady_clock::now();
    scan( now );

    std::vector<std::string> due;

    {
        std::lock_guard<std::mutex> lock( m_mutex );

        for( auto it = m_pending.begin(); it != m_pending.end(); )
        {
            if( now >= it->second )
            {
                due.push_back( it->first );
                it = m_pending.erase( it );
            }
            else
            {
                ++it;
            }
        }
    }

    for( const std::string& path : due )
    {
        if( m_stopRequested )
            return;

        CABLY_SAVE_KIND kind;

        if( !CablyClassifySavePath( path, &kind ) )
            continue;

        std::string text;

        if( !readWholeFile( path, text ) )
            continue; // vanished between the change and the read (e.g. renamed away)

        std::string hash = CABLY_BRIDGE::Sha256Hex( text );

        {
            std::lock_guard<std::mutex> lock( m_mutex );
            auto                        known = m_known.find( path );

            // THE dedupe: a replay, a touch, or our own (Expect()ed) write is not a save.
            // (Mutation-tested in cably/tests/bridge.sh.)
            if( known != m_known.end() && known->second == hash )
                continue;

            m_known[path] = hash;
        }

        CABLY_SAVE_EVENT event;
        event.kind = kind;
        event.path = path;
        event.text = std::move( text );

        if( m_onSave )
            m_onSave( event );
    }
}


// ---------------------------------------------------------------------------------------
// sync one save
// ---------------------------------------------------------------------------------------

CABLY_SYNC_RESULT CablySyncSave( CABLY_BRIDGE& aBridge, const std::string& aDir,
                                 const CABLY_SAVE_EVENT& aEvent )
{
    CABLY_SYNC_RESULT    r;
    CABLY_EXPORT_SIDECAR sidecar;

    if( !CABLY_EXPORT_SIDECAR::Load( aDir, sidecar ) || sidecar.meta.projectId.empty() )
    {
        r.outcome = CABLY_SYNC_OUTCOME::NOT_CABLY_PROJECT;
        r.error = "This folder was not opened from Cably (no project id in "
                  + std::string( CABLY_EXPORT_SIDECAR::FileName() ) + ").";
        return r;
    }

    r.projectId = sidecar.meta.projectId;
    r.projectName = sidecar.meta.projectName;

    const std::string* pcb = aEvent.kind == CABLY_SAVE_KIND::BOARD ? &aEvent.text : nullptr;
    const std::string* sch = aEvent.kind == CABLY_SAVE_KIND::SCHEMATIC ? &aEvent.text : nullptr;

    CABLY_IMPORT_RESULT  imported;
    CABLY_IMPORT_OUTCOME outcome =
            aBridge.ImportProject( sidecar.meta.projectId, pcb, sch, sidecar.meta.cloudUpdatedAt, imported );

    switch( outcome )
    {
    case CABLY_IMPORT_OUTCOME::CLOUD_CHANGED:
        r.outcome = CABLY_SYNC_OUTCOME::CLOUD_CHANGED;
        r.cloudUpdatedAt = imported.cloudUpdatedAt;
        r.error = "The project changed in Cably since it was opened here.";
        return r;

    case CABLY_IMPORT_OUTCOME::FAILED:
        r.outcome = CABLY_SYNC_OUTCOME::FAILED;
        r.error = aBridge.LastError();
        return r;

    case CABLY_IMPORT_OUTCOME::SYNCED:
        break;
    }

    r.outcome = CABLY_SYNC_OUTCOME::SYNCED;
    r.cloudUpdatedAt = imported.updatedAt;
    r.pcbReport = imported.pcbReport;
    r.schReport = imported.schReport;
    r.engineVersion = imported.engineVersion;

    // The saved text is now what the cloud holds: it becomes the baseline of both the
    // never-clobber rule and the watcher's dedupe, under the row's new version.
    sidecar.files[fs::path( aEvent.path ).filename().string()] = CABLY_BRIDGE::Sha256Hex( aEvent.text );
    sidecar.meta.cloudUpdatedAt = imported.updatedAt;

    if( !imported.engineVersion.empty() )
        sidecar.meta.engineVersion = imported.engineVersion;

    if( !sidecar.Save( aDir ) )
    {
        r.error = "Synced, but " + std::string( CABLY_EXPORT_SIDECAR::FileName() ) + " could not be updated.";
    }

    return r;
}

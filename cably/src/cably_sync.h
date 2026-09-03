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

/**
 * @file cably_sync.h
 * F5: "the user pressed Save in KiCad" -> the project in the cloud is updated.
 *
 * CABLY_PROJECT_WATCH watches ONE project folder that was opened from Cably (identified
 * by the .cably-export.json sidecar, CABLY_EXPORT_SIDECAR) and turns a save of the board
 * or the schematic into one CABLY_SAVE_EVENT carrying the whole new document.  The rules
 * are the Electron companion's (desktop/src/main/watcher.ts), mirrored exactly:
 *
 *  - classify: *.kicad_pcb -> BOARD, *.kicad_sch -> SCHEMATIC, nothing else; KiCad's
 *    backups (-bak), locks (~x.lck), autosaves (_autosave-, _saved_) and anything under a
 *    *-backups/ folder are not a save and never leak through;
 *  - per-path debounce (default 500 ms): a burst of writes to one file (editors touch a
 *    file more than once per save, and a large file is read only once it stopped
 *    changing) becomes one event with the final text; a board and a schematic saved
 *    together still give one event each;
 *  - content-hash dedupe: only a file whose bytes differ from the last bytes we know
 *    about is a save.  The baseline is seeded from the sidecar's hashes (what the cloud
 *    last wrote/synced), documents the sidecar does not list are hashed from disk, and
 *    every accepted save updates it.  A replayed event, a touch that changes nothing and
 *    Cably's own hand-off write (announced through Expect()) are therefore silent.
 *
 * Mechanism: POLLING, on the watcher's own thread, every pollMs (default 250 ms): the
 * folder is listed (depth 0) and each document's (inode, size, mtime ns) is compared
 * with the last pass; a change starts/extends that path's debounce.  Measured on macOS
 * 26 (cably/tests/bridge.sh prints the numbers): an in-place write and a
 * write-temp-then-rename both surface as one event, debounce + at most two poll
 * periods after the last write (~0.6-1.0 s with the defaults).  wxFileSystemWatcher was
 * measured and rejected: on macOS wx 3.2 backs Add() with kqueue, which asserts unless a
 * wx event loop is already running and delivered NO event for an in-place write or a
 * rename-over in the console-loop harness (0 events in both Add and AddTree/FSEvents
 * modes, 2.5 s windows), and it would tie the sync core to the GUI thread - this file,
 * like the rest of the bridge, is plain C++17 so cably/tests/bridge.sh builds and runs it
 * standalone.  The callback runs ON THE WATCHER THREAD; a UI must marshal.
 *
 * CablySyncSave() is the other half: sidecar in -> CABLY_BRIDGE::ImportProject -> sidecar
 * out (new cloudUpdatedAt, new baseline hash, engine version).
 */

#ifndef CABLY_SYNC_H
#define CABLY_SYNC_H

#include <cably_bridge.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>


enum class CABLY_SAVE_KIND
{
    BOARD,    ///< *.kicad_pcb
    SCHEMATIC ///< *.kicad_sch
};


struct CABLY_SAVE_EVENT
{
    CABLY_SAVE_KIND kind = CABLY_SAVE_KIND::BOARD;
    std::string     path; ///< absolute path of the document
    std::string     text; ///< its whole new content
};


/**
 * The Electron classifyPath: true (and aKind set) for a board or schematic document;
 * false for backups, locks, autosaves, *-backups/ contents and every other file.
 * Both '/' and '\\' separate path segments.
 */
bool CablyClassifySavePath( const std::string& aPath, CABLY_SAVE_KIND* aKind = nullptr );


struct CABLY_WATCH_OPTIONS
{
    int debounceMs = 500; ///< quiet time after the last change of a path before it is read
    int pollMs = 250;     ///< folder scan period (keep it below debounceMs)
};


class CABLY_PROJECT_WATCH
{
public:
    using CALLBACK = std::function<void( const CABLY_SAVE_EVENT& )>;

    CABLY_PROJECT_WATCH();
    ~CABLY_PROJECT_WATCH();

    CABLY_PROJECT_WATCH( const CABLY_PROJECT_WATCH& ) = delete;
    CABLY_PROJECT_WATCH& operator=( const CABLY_PROJECT_WATCH& ) = delete;

    /**
     * Seed the baselines (sidecar first, then disk), snapshot the folder and start the
     * polling thread.  false (and aError) when aDir is not a directory or a watch is
     * already running.
     */
    bool Start( const std::string& aDir, CALLBACK aOnSave, const CABLY_WATCH_OPTIONS& aOptions = {},
                std::string* aError = nullptr );

    /// Stop the thread and drop every pending debounce; nothing is emitted afterwards.
    void Stop();

    bool               IsRunning() const { return m_running; }
    const std::string& Dir() const { return m_dir; }

    /**
     * Tell the watcher what Cably itself is about to write to aPath, so the resulting
     * change is recognised as ours and not reported as a KiCad save.  Without this every
     * hand-off would echo straight back.
     */
    void Expect( const std::string& aPath, const std::string& aText );

    /// The baseline hash currently known for aPath (empty when none) - for tests/diagnostics.
    std::string KnownHash( const std::string& aPath ) const;

    /**
     * One scan-and-fire pass.  The thread calls it every pollMs; it is public so a host
     * with its own timer could drive it instead.  Emits (on the calling thread) every
     * path whose debounce elapsed and whose content differs from its baseline.
     */
    void Poll();

private:
    struct SIGNATURE
    {
        bool               exists = false;
        unsigned long long inode = 0;
        long long          size = 0;
        long long          mtimeNs = 0;

        bool operator==( const SIGNATURE& o ) const
        {
            return exists == o.exists && inode == o.inode && size == o.size && mtimeNs == o.mtimeNs;
        }
        bool operator!=( const SIGNATURE& o ) const { return !( *this == o ); }
    };

    static SIGNATURE signatureOf( const std::string& aPath );
    void             scan( std::chrono::steady_clock::time_point aNow );
    void             threadLoop();

    std::string         m_dir;
    CALLBACK            m_onSave;
    CABLY_WATCH_OPTIONS m_options;

    mutable std::mutex                                         m_mutex;
    std::map<std::string, std::string>                         m_known;    ///< path -> sha256 of the current content
    std::map<std::string, SIGNATURE>                           m_sigs;     ///< path -> last seen stat
    std::map<std::string, std::chrono::steady_clock::time_point> m_pending; ///< path -> debounce deadline

    std::atomic<bool>       m_running{ false };
    std::atomic<bool>       m_stopRequested{ false };
    std::thread             m_thread;
    std::condition_variable m_wake;
    std::mutex              m_wakeMutex;
};


enum class CABLY_SYNC_OUTCOME
{
    SYNCED,            ///< the cloud row was updated; the sidecar carries the new version
    CLOUD_CHANGED,     ///< the row is newer than the sidecar's cloudUpdatedAt; nothing sent
    NOT_CABLY_PROJECT, ///< no sidecar, or one without a projectId (pre-F5 export)
    FAILED             ///< error set
};


struct CABLY_SYNC_RESULT
{
    CABLY_SYNC_OUTCOME outcome = CABLY_SYNC_OUTCOME::FAILED;
    std::string        projectId;
    std::string        projectName;
    std::string        cloudUpdatedAt; ///< SYNCED: the new version; CLOUD_CHANGED: the row's
    std::string        pcbReport;      ///< JSON text
    std::string        schReport;      ///< JSON text
    std::string        engineVersion;
    std::string        error;
};


/**
 * Send one save to the cloud: read aDir's sidecar, call aBridge.ImportProject() with the
 * document as kicadPcb or kicadSch and the sidecar's cloudUpdatedAt as the expected
 * version, and on success rewrite the sidecar (cloudUpdatedAt, files[<name>] = sha256 of
 * the saved text, engineVersion).  Nothing on disk is touched otherwise.
 */
CABLY_SYNC_RESULT CablySyncSave( CABLY_BRIDGE& aBridge, const std::string& aDir,
                                 const CABLY_SAVE_EVENT& aEvent );

#endif // CABLY_SYNC_H

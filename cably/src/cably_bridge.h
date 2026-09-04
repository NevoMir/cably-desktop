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
 * @file cably_bridge.h
 * F4: the cloud bridge between Cably Desktop and cably.dev.
 *
 * Everything here is plain C++17 (nlohmann/json, picosha2, POSIX sockets); no wxWidgets,
 * no KiCad types, so the whole thing is unit-tested standalone (cably/tests/bridge.sh).
 * The two things that touch the outside world sit behind interfaces:
 *
 *  - CABLY_HTTP           every HTTP call.  CABLY_HTTP_KICAD (cably_bridge_http_kicad.cpp)
 *                         is the KICAD_CURL_EASY implementation used by the app and the
 *                         CLI; tests inject a recording fake.
 *  - CABLY_SECRET_STORE   where the session lives.  CABLY_KEYCHAIN_SECRET_STORE
 *                         (cably_bridge_keychain.cpp) is the macOS Security.framework
 *                         implementation, and on Linux libsecret with a 0600 file as the
 *                         fallback (CABLY_SECRET_SERVICE is its test seam);
 *                         CABLY_MEMORY_SECRET_STORE is for tests/CLI.
 *
 * Sign-in is a loopback handoff: CABLY_LOOPBACK_SERVER listens on 127.0.0.1:<random>,
 * the system browser opens CABLY_DESKTOP_AUTH_URL?port=P&state=S, the web page performs
 * a TOP-LEVEL navigation to http://127.0.0.1:P/callback#state=S&access_token=...  (a
 * fragment: tokens never reach a server log), the served page's script POSTs
 * location.hash as JSON to /token on the same origin, and the server accepts it only if
 * the state matches - exactly once - then stops listening.
 *
 * The exact HTTP calls (paths, headers) are documented on each method and asserted by
 * cably/tests/unit/test_cably_bridge.cpp.
 *
 * F5 (sync, cably_sync.h): ImportProject() sends a KiCad save back to the cloud, and the
 * .cably-export.json sidecar (CABLY_EXPORT_SIDECAR) written next to the exported files
 * remembers which row the folder came from and the row's version at export time, so a
 * later save can be checked against the cloud before anything is overwritten.
 */

#ifndef CABLY_BRIDGE_H
#define CABLY_BRIDGE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>


// ---------------------------------------------------------------------------------------
// HTTP seam
// ---------------------------------------------------------------------------------------

struct CABLY_HTTP_REQUEST
{
    std::string                                      method; ///< "GET" or "POST"
    std::string                                      url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string                                      body;   ///< POST body (JSON)

    /// Case-insensitive header lookup; empty when absent.
    std::string Header( const std::string& aName ) const;
};


struct CABLY_HTTP_RESPONSE
{
    bool        transportOk = false; ///< false: no HTTP status at all (DNS, refused, timeout)
    int         status = 0;
    std::string body;
    std::string error; ///< transport error text when !transportOk
};


class CABLY_HTTP
{
public:
    virtual ~CABLY_HTTP() = default;
    virtual CABLY_HTTP_RESPONSE Perform( const CABLY_HTTP_REQUEST& aRequest ) = 0;
};


/// KICAD_CURL_EASY-backed transport (needs kicommon; KICAD_CURL::Init() must have run).
class CABLY_HTTP_KICAD : public CABLY_HTTP
{
public:
    CABLY_HTTP_RESPONSE Perform( const CABLY_HTTP_REQUEST& aRequest ) override;
};


// ---------------------------------------------------------------------------------------
// Session + secret store seam
// ---------------------------------------------------------------------------------------

struct CABLY_SESSION
{
    std::string accessToken;
    std::string refreshToken;
    std::string email;
    long long   expiresAt = 0; ///< Unix seconds

    bool Empty() const { return accessToken.empty() || refreshToken.empty(); }
};

std::string CablySessionToJson( const CABLY_SESSION& aSession );
bool        CablySessionFromJson( const std::string& aJson, CABLY_SESSION& aOut );


class CABLY_SECRET_STORE
{
public:
    virtual ~CABLY_SECRET_STORE() = default;
    virtual bool Load( CABLY_SESSION& aOut ) = 0;        ///< false when nothing stored
    virtual bool Save( const CABLY_SESSION& aSession ) = 0;
    virtual bool Clear() = 0;                            ///< true when nothing remains
    virtual std::string LastError() const { return std::string(); }
};


class CABLY_MEMORY_SECRET_STORE : public CABLY_SECRET_STORE
{
public:
    bool Load( CABLY_SESSION& aOut ) override;
    bool Save( const CABLY_SESSION& aSession ) override;
    bool Clear() override;

private:
    bool          m_has = false;
    CABLY_SESSION m_session;
};


/**
 * Linux: the Secret Service (libsecret) half of CABLY_KEYCHAIN_SECRET_STORE behind an
 * interface, so the unit test (cably/tests/unit/test_cably_secret_store.cpp) can stand in
 * a fake for the keyring that the container and CI do not have.  Lookup() returns false
 * with an EMPTY aError when there is simply no item.
 */
class CABLY_SECRET_SERVICE
{
public:
    virtual ~CABLY_SECRET_SERVICE() = default;

    /**
     * A Secret Service answers on the session bus right now (the probe is bounded: a bus
     * that never replies counts as unavailable).  On false, aReason says why - empty when
     * the machine simply has no session bus, else the D-Bus/libsecret error (a bus nobody
     * listens on, "The name org.freedesktop.secrets was not provided by any .service
     * files" on a bus without a keyring, a timeout ...).
     */
    virtual bool Available( std::string& aReason ) = 0;
    virtual bool Lookup( const std::string& aService, std::string& aJson, std::string& aError ) = 0;
    virtual bool Store( const std::string& aService, const std::string& aJson, std::string& aError ) = 0;
    virtual bool Clear( const std::string& aService, std::string& aError ) = 0;
};


/**
 * The persistent session store.
 *
 * macOS: one generic-password item, service = aService (CABLY_KEYCHAIN_SERVICE),
 * account "session", data = CablySessionToJson().
 *
 * Linux (and other Unix): the Secret Service through libsecret when one answers (schema
 * dev.cably.desktop, attributes service=aService, account="session", in the default
 * collection), else - documented fallback - the file
 * $XDG_CONFIG_HOME/cably-desktop/session.json (session.<service>.json for any other
 * service name; $HOME/.config when XDG_CONFIG_HOME is unset or relative), mode 0600 in a 0700
 * directory, written temp-then-rename.  The file is used whenever the service cannot be
 * used for ANY reason - no session bus, a bus nobody listens on, a bus without a keyring
 * (GitHub's runners), or a D-Bus error in the middle of a lookup/store/clear - with
 * LastError() empty and Note() saying why; a service that answers is always preferred,
 * and Load() also finds the file when the service is back so the next Save() moves the
 * session into it (the file is removed).  Only a stored item that is not a session is an
 * error.  Backend() says which was used.
 *
 * Windows: every call fails with LastError() "no secret store on this platform"
 * (Credential Manager is TODO; see CHANGES.md).
 */
class CABLY_KEYCHAIN_SECRET_STORE : public CABLY_SECRET_STORE
{
public:
    explicit CABLY_KEYCHAIN_SECRET_STORE( const std::string& aService );

    /**
     * Linux test seam.  aBackend nullptr = the platform's libsecret backend (none when
     * built without CABLY_HAVE_LIBSECRET); aFallbackDir empty = DefaultFallbackDir().
     * On macOS both are ignored (the keychain is always used).
     */
    CABLY_KEYCHAIN_SECRET_STORE( const std::string& aService, CABLY_SECRET_SERVICE* aBackend,
                                 const std::string& aFallbackDir );

    bool        Load( CABLY_SESSION& aOut ) override;
    bool        Save( const CABLY_SESSION& aSession ) override;
    bool        Clear() override;
    std::string LastError() const override { return m_error; }

    /// What the last operation used: "keychain" (macOS), "secret-service" or "file:<path>".
    std::string Backend() const { return m_backend; }

    /**
     * Linux: why the last operation did not use the Secret Service (it was unreachable,
     * or a call to it failed and the file was used instead); empty when it was used, when
     * the machine has no session bus at all, or on macOS.
     */
    std::string Note() const { return m_note; }

    /// Linux: $XDG_CONFIG_HOME/cably-desktop, else $HOME/.config/cably-desktop.
    static std::string DefaultFallbackDir();
    /// Linux: aDir/session.json for the default service name, aDir/session.<service>.json else.
    static std::string FallbackPath( const std::string& aDir, const std::string& aService );

private:
    /// Linux: the service is worth talking to (probed); on false, m_note carries the reason.
    bool serviceAnswers();

    std::string           m_service;
    std::string           m_error;
    std::string           m_backend;
    std::string           m_note;
    std::string           m_fallbackDir;
    CABLY_SECRET_SERVICE* m_secretService = nullptr;
};


// ---------------------------------------------------------------------------------------
// Loopback sign-in handoff
// ---------------------------------------------------------------------------------------

class CABLY_LOOPBACK_SERVER
{
public:
    CABLY_LOOPBACK_SERVER();
    ~CABLY_LOOPBACK_SERVER();

    CABLY_LOOPBACK_SERVER( const CABLY_LOOPBACK_SERVER& ) = delete;
    CABLY_LOOPBACK_SERVER& operator=( const CABLY_LOOPBACK_SERVER& ) = delete;

    /// Bind 127.0.0.1:0 (kernel-chosen port), start the accept thread.
    bool Start( std::string* aError = nullptr );
    /// Stop accepting and join the thread. Safe to call twice.
    void Stop();

    int                Port() const { return m_port; }
    const std::string& State() const { return m_state; }
    bool               IsListening() const { return m_listening; }
    bool               Accepted() const { return m_accepted; }

    /// Block up to aTimeoutMs for an accepted /token; true and aOut filled when it came.
    bool WaitForSession( int aTimeoutMs, CABLY_SESSION& aOut );

    /**
     * The request handler, socket-free so tests drive it directly.
     *  GET  /callback  -> 200 text/html, CallbackHtml()
     *  POST /token     -> 200 application/json {"ok":true} iff body is JSON with
     *                     state == State(), non-empty access_token + refresh_token,
     *                     and no token was accepted before; else 400.
     *  anything else   -> 404 (unknown path) / 405 (known path, wrong method)
     * @return the HTTP status; aResponseBody/aContentType filled.
     */
    int HandleRequest( const std::string& aMethod, const std::string& aPath,
                       const std::string& aBody, std::string& aResponseBody,
                       std::string& aContentType );

    /// The page served at /callback: reads location.hash, POSTs it as JSON to /token.
    static std::string CallbackHtml();

private:
    void serveLoop();
    void serveClient( int aClientFd );
    bool acceptToken( const std::string& aBody );

    std::string             m_state;
    std::atomic<int>        m_port{ 0 };
    std::atomic<bool>       m_listening{ false };
    std::atomic<bool>       m_stopRequested{ false };
    std::atomic<bool>       m_accepted{ false };
    int                     m_listenFd = -1;
    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    CABLY_SESSION           m_session;
};


// ---------------------------------------------------------------------------------------
// The bridge
// ---------------------------------------------------------------------------------------

struct CABLY_BRIDGE_CONFIG
{
    std::string supabaseUrl;     ///< CABLY_SUPABASE_URL
    std::string publishableKey;  ///< CABLY_SUPABASE_PUBLISHABLE_KEY (public)
    std::string engineUrl;       ///< CABLY_ENGINE_URL
    std::string authPageUrl;     ///< CABLY_DESKTOP_AUTH_URL

    /// The values from cably_config.h.
    static CABLY_BRIDGE_CONFIG Default();
};


struct CABLY_PROJECT_SUMMARY
{
    std::string id;
    std::string name;
    std::string updatedAt;
};


struct CABLY_EXPORT_RESULT
{
    bool        hasPcb = false;
    bool        hasSch = false;
    std::string kicadPcb;
    std::string kicadSch;
    std::string engineVersion;
};


/// F5: what the sidecar records about the cloud row an export came from.
struct CABLY_EXPORT_META
{
    std::string projectId;      ///< projects.id
    std::string projectName;    ///< projects.name at export
    std::string cloudUpdatedAt; ///< projects.updated_at of the row we exported (or last synced)
    std::string engineVersion;  ///< engineVersion of the export (or last import)
};


/**
 * The .cably-export.json sidecar in an exported project folder:
 *   { "stem", "files": { "<name>": "<sha256 hex>" }, "engine": "cably",
 *     "projectId", "projectName", "cloudUpdatedAt", "engineVersion" }
 * `files` are the baselines of the never-clobber rule (WriteProjectFolder) and of the
 * save watcher's content dedupe (cably_sync.h); the meta fields are absent in pre-F5
 * sidecars and then read back empty.
 */
struct CABLY_EXPORT_SIDECAR
{
    std::string                        stem;
    std::map<std::string, std::string> files;
    CABLY_EXPORT_META                  meta;

    static const char* FileName() { return ".cably-export.json"; }

    /// false when the folder has no readable, well-formed sidecar.
    static bool Load( const std::string& aDir, CABLY_EXPORT_SIDECAR& aOut );
    bool        Save( const std::string& aDir ) const;
};


/// F5: ImportProject's verdict.
enum class CABLY_IMPORT_OUTCOME
{
    SYNCED,        ///< the row's data.project now holds the engine's updated project
    CLOUD_CHANGED, ///< the row is newer than aExpectedUpdatedAt: nothing was written
    FAILED         ///< see LastError()
};


struct CABLY_IMPORT_RESULT
{
    CABLY_IMPORT_OUTCOME outcome = CABLY_IMPORT_OUTCOME::FAILED;
    std::string          cloudUpdatedAt; ///< the row's updated_at as fetched (set on CLOUD_CHANGED too)
    std::string          updatedAt;      ///< the row's updated_at after the PATCH (SYNCED only)
    std::string          project;        ///< the updated project JSON (SYNCED only)
    std::string          pcbReport;      ///< JSON text ("null" when the engine sent none)
    std::string          schReport;      ///< JSON text ("null" when the engine sent none)
    std::string          engineVersion;
};


struct CABLY_WRITE_RESULT
{
    bool                     written = false; ///< false => conflicts non-empty, nothing touched
    std::string              dir;
    std::string              proPath;
    std::string              pcbPath;
    std::string              schPath;         ///< empty when no schematic was given
    std::vector<std::string> conflicts;       ///< files KiCad edited since we wrote them
    std::string              error;           ///< I/O failure text (written == false)
};


class CABLY_BRIDGE
{
public:
    CABLY_BRIDGE( CABLY_HTTP& aHttp, CABLY_SECRET_STORE& aStore, const CABLY_BRIDGE_CONFIG& aConfig );
    ~CABLY_BRIDGE();

    // ---- sign-in -------------------------------------------------------------------
    /// aBaseUrl?port=<aPort>&state=<percent-encoded aState>
    static std::string BuildAuthUrl( const std::string& aBaseUrl, int aPort, const std::string& aState );
    static std::string UrlEncode( const std::string& aText );

    /// Start the loopback listener; returns the port (0 on failure, see LastError()).
    int StartLoopback();
    /// The running listener (null until StartLoopback()).
    CABLY_LOOPBACK_SERVER* Loopback() { return m_loopback.get(); }
    /// The URL to open in the system browser for the running listener.
    std::string AuthUrl() const;
    /// Wait for the handoff, persist the session, stop the listener. false on timeout.
    bool FinishLoopback( int aTimeoutMs );
    /// Stop the listener without waiting (user cancelled).
    void CancelLoopback();

    // ---- session -------------------------------------------------------------------
    bool                 LoadSession();  ///< from the store
    bool                 HasSession() const { return !m_session.Empty(); }
    const CABLY_SESSION& Session() const { return m_session; }
    bool                 SignOut();      ///< clear memory + store

    /**
     * POST {supabase}/auth/v1/token?grant_type=refresh_token
     *   headers: apikey, Content-Type: application/json   body: {"refresh_token": ...}
     * On success the new session replaces the old one (memory + store); on a 4xx the
     * session is cleared (the refresh token is dead).
     */
    bool RefreshSession();

    /**
     * GET {supabase}/auth/v1/user   headers: apikey, Authorization: Bearer <access>
     * A 401 triggers one RefreshSession() and one retry.
     */
    bool ValidateSession();

    // ---- projects ------------------------------------------------------------------
    /// GET {supabase}/rest/v1/projects?select=id,name,updated_at&order=updated_at.desc&limit=50
    bool ListProjects( std::vector<CABLY_PROJECT_SUMMARY>& aOut );

    /// GET {supabase}/rest/v1/projects?id=eq.<id>&select=data -> data.project as JSON text.
    /// With aUpdatedAt the select becomes `data,updated_at` and the row's version is
    /// returned too (F5: recorded in the sidecar as cloudUpdatedAt).
    bool FetchProject( const std::string& aId, std::string& aProjectJson,
                       std::string* aUpdatedAt = nullptr );

    /// POST {engine}/v1/export  headers: Authorization: Bearer, Content-Type: application/json
    /// body: {"apiVersion":1,"project":<aProjectJson>}
    bool ExportProject( const std::string& aProjectJson, CABLY_EXPORT_RESULT& aOut );

    /**
     * Write <aRoot>/<SafeStem(aStem)>/<stem>.{kicad_pro,kicad_pcb,kicad_sch} plus the
     * sidecar .cably-export.json (sha256 of each file as written).  Mirrors the web app's
     * desktop hand-off rule: a file that differs from BOTH its recorded baseline and the
     * new text was edited in KiCad and is never overwritten unless aForce; a file with no
     * baseline at all is treated as the user's.  The .kicad_pro is only ever created
     * (KiCad owns it afterwards).  An empty aSch means "no schematic".
     */
    static CABLY_WRITE_RESULT WriteProjectFolder( const std::string& aRoot, const std::string& aStem,
                                                  const std::string& aPcb, const std::string& aSch,
                                                  bool aForce = false,
                                                  const CABLY_EXPORT_META* aMeta = nullptr );

    /**
     * F5: send a KiCad save back to the cloud (mirrors the web app's F5 import path).
     *  1. GET  {supabase}/rest/v1/projects?id=eq.<id>&select=data,updated_at
     *     (apikey + bearer).  If aExpectedUpdatedAt is non-empty and the row's updated_at
     *     is NEWER (CompareIsoTimestamps > 0, or unparsable and different), the cloud
     *     changed since we exported: return CLOUD_CHANGED and touch nothing.
     *  2. POST {engine}/v1/import  (bearer only)  body exactly
     *     {"apiVersion":1,"project":<data.project>[,"kicadPcb":..][,"kicadSch":..]}
     *     -> {project, pcbReport, schReport, engineVersion, timings}
     *  3. PATCH {supabase}/rest/v1/projects?id=eq.<id>  (apikey + bearer,
     *     Content-Type: application/json, Prefer: return=minimal)  body {"data": <data>}
     *     where data is the fetched data with ONLY .project replaced (schema, chat kept;
     *     a legacy row whose data IS the project gets the new project as its data).
     *  4. GET  {supabase}/rest/v1/projects?id=eq.<id>&select=updated_at -> the version the
     *     projects.updated_at trigger stamped (return=minimal carries none).
     * At least one of aKicadPcb / aKicadSch must be given.  The token goes to those two
     * hosts and nowhere else.
     */
    CABLY_IMPORT_OUTCOME ImportProject( const std::string& aId, const std::string* aKicadPcb,
                                        const std::string* aKicadSch,
                                        const std::string& aExpectedUpdatedAt,
                                        CABLY_IMPORT_RESULT& aOut );

    /// [A-Za-z0-9_-] runs, no dots, max 64, never empty ("cably-project").
    static std::string SafeStem( const std::string& aName );

    /// Lower-case hex SHA-256 (the sidecar's and the watcher's content hash).
    static std::string Sha256Hex( const std::string& aText );

    /**
     * Order two ISO-8601 instants ("2026-09-03T10:00:00.123456+00:00", "...Z", "+02:00"),
     * to the microsecond: <0, 0, >0.  When either does not parse they compare as plain
     * strings (so a different string is never silently "equal").
     */
    static int CompareIsoTimestamps( const std::string& aA, const std::string& aB );

    const std::string& LastError() const { return m_lastError; }

private:
    /// Supabase call with apikey + bearer (+ aExtraHeaders); one refresh-and-retry on 401.
    bool supabaseRequest( const std::string& aMethod, const std::string& aPath,
                          const std::string& aBody, CABLY_HTTP_RESPONSE& aOut,
                          const std::vector<std::pair<std::string, std::string>>& aExtraHeaders = {} );
    bool fetchRow( const std::string& aId, bool aWithVersion, std::string& aDataJson,
                   std::string& aUpdatedAt );
    bool engineRequest( const std::string& aPath, const std::string& aBody, CABLY_HTTP_RESPONSE& aOut );
    bool failFrom( const CABLY_HTTP_RESPONSE& aResponse, const std::string& aWhat );
    void setSession( const CABLY_SESSION& aSession );

    CABLY_HTTP&                            m_http;
    CABLY_SECRET_STORE&                    m_store;
    CABLY_BRIDGE_CONFIG                    m_config;
    CABLY_SESSION                          m_session;
    std::string                            m_lastError;
    std::unique_ptr<CABLY_LOOPBACK_SERVER> m_loopback;
};

#endif // CABLY_BRIDGE_H

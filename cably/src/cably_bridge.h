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
 *                         implementation; CABLY_MEMORY_SECRET_STORE is for tests/CLI.
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
 */

#ifndef CABLY_BRIDGE_H
#define CABLY_BRIDGE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
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
 * macOS: one generic-password item, service = aService (CABLY_KEYCHAIN_SERVICE),
 * account "session", data = CablySessionToJson().  Other platforms: every call fails
 * with LastError() "no secret store on this platform" (Windows Credential Manager and
 * libsecret are TODO; see CHANGES.md).
 */
class CABLY_KEYCHAIN_SECRET_STORE : public CABLY_SECRET_STORE
{
public:
    explicit CABLY_KEYCHAIN_SECRET_STORE( const std::string& aService );
    bool        Load( CABLY_SESSION& aOut ) override;
    bool        Save( const CABLY_SESSION& aSession ) override;
    bool        Clear() override;
    std::string LastError() const override { return m_error; }

private:
    std::string m_service;
    std::string m_error;
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
    bool FetchProject( const std::string& aId, std::string& aProjectJson );

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
                                                  bool aForce = false );

    /// [A-Za-z0-9_-] runs, no dots, max 64, never empty ("cably-project").
    static std::string SafeStem( const std::string& aName );

    const std::string& LastError() const { return m_lastError; }

private:
    /// Supabase call with apikey + bearer; one refresh-and-retry on 401.
    bool supabaseRequest( const std::string& aMethod, const std::string& aPath,
                          const std::string& aBody, CABLY_HTTP_RESPONSE& aOut );
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

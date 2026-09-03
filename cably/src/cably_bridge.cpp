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
 * F4 cloud bridge core: see cably_bridge.h.  Pure C++17 + nlohmann/json + picosha2 +
 * POSIX sockets.  The Windows port needs winsock2 in the loopback server (marked below);
 * everything else is portable.
 */

#include <cably_bridge.h>
#include <cably_config.h>

#include <nlohmann/json.hpp>
#include <picosha2.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;


// ---------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------

static bool iequals( const std::string& a, const std::string& b )
{
    if( a.size() != b.size() )
        return false;

    for( size_t i = 0; i < a.size(); ++i )
    {
        if( std::tolower( static_cast<unsigned char>( a[i] ) )
            != std::tolower( static_cast<unsigned char>( b[i] ) ) )
            return false;
    }

    return true;
}


std::string CABLY_HTTP_REQUEST::Header( const std::string& aName ) const
{
    for( const auto& h : headers )
    {
        if( iequals( h.first, aName ) )
            return h.second;
    }

    return std::string();
}


static std::string sha256Hex( const std::string& aText )
{
    return picosha2::hash256_hex_string( aText.begin(), aText.end() );
}


static std::string randomHex( size_t aBytes )
{
    std::string bytes( aBytes, '\0' );
    bool        filled = false;

    // Prefer the kernel CSPRNG; fall back to std::random_device (arc4random-backed on macOS).
    if( FILE* f = std::fopen( "/dev/urandom", "rb" ) )
    {
        filled = std::fread( &bytes[0], 1, aBytes, f ) == aBytes;
        std::fclose( f );
    }

    if( !filled )
    {
        std::random_device rd;

        for( size_t i = 0; i < aBytes; ++i )
            bytes[i] = static_cast<char>( rd() & 0xff );
    }

    static const char* hex = "0123456789abcdef";
    std::string        out;
    out.reserve( aBytes * 2 );

    for( unsigned char c : bytes )
    {
        out.push_back( hex[c >> 4] );
        out.push_back( hex[c & 0x0f] );
    }

    return out;
}


static bool cloudIsNewer( const std::string& aRowUpdatedAt, const std::string& aExpected );


static long long nowSeconds()
{
    return static_cast<long long>( std::time( nullptr ) );
}


static bool readWholeFile( const fs::path& aPath, std::string& aOut )
{
    std::ifstream in( aPath, std::ios::binary );

    if( !in )
        return false;

    aOut.assign( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
    return true;
}


static bool writeWholeFile( const fs::path& aPath, const std::string& aText )
{
    std::ofstream out( aPath, std::ios::binary | std::ios::trunc );

    if( !out )
        return false;

    out << aText;
    return static_cast<bool>( out );
}


/// expires_at arrives as a number from GoTrue and as a numeric string from URLSearchParams.
static long long jsonSeconds( const json& aValue )
{
    if( aValue.is_number() )
        return aValue.get<long long>();

    if( aValue.is_string() )
    {
        try
        {
            return std::stoll( aValue.get<std::string>() );
        }
        catch( ... )
        {
        }
    }

    return 0;
}


// ---------------------------------------------------------------------------------------
// Session JSON + stores
// ---------------------------------------------------------------------------------------

std::string CablySessionToJson( const CABLY_SESSION& aSession )
{
    json j;
    j["access_token"] = aSession.accessToken;
    j["refresh_token"] = aSession.refreshToken;
    j["email"] = aSession.email;
    j["expires_at"] = aSession.expiresAt;
    return j.dump();
}


bool CablySessionFromJson( const std::string& aJson, CABLY_SESSION& aOut )
{
    try
    {
        json          j = json::parse( aJson );
        CABLY_SESSION s;
        s.accessToken = j.value( "access_token", "" );
        s.refreshToken = j.value( "refresh_token", "" );
        s.email = j.value( "email", "" );
        s.expiresAt = j.contains( "expires_at" ) ? jsonSeconds( j["expires_at"] ) : 0;

        if( s.Empty() )
            return false;

        aOut = s;
        return true;
    }
    catch( ... )
    {
        return false;
    }
}


bool CABLY_MEMORY_SECRET_STORE::Load( CABLY_SESSION& aOut )
{
    if( !m_has )
        return false;

    aOut = m_session;
    return true;
}


bool CABLY_MEMORY_SECRET_STORE::Save( const CABLY_SESSION& aSession )
{
    m_session = aSession;
    m_has = true;
    return true;
}


bool CABLY_MEMORY_SECRET_STORE::Clear()
{
    m_has = false;
    m_session = CABLY_SESSION();
    return true;
}


// ---------------------------------------------------------------------------------------
// Loopback server
// ---------------------------------------------------------------------------------------

CABLY_LOOPBACK_SERVER::CABLY_LOOPBACK_SERVER() :
        m_state( randomHex( 32 ) )
{
}


CABLY_LOOPBACK_SERVER::~CABLY_LOOPBACK_SERVER()
{
    Stop();
}


std::string CABLY_LOOPBACK_SERVER::CallbackHtml()
{
    // Served on http://127.0.0.1:<port>/callback.  The tokens are in the fragment, which
    // the browser never sends over the wire; this script is the only reader.  The POST
    // goes to the same origin, so no CORS is involved.
    return
        "<!doctype html>\n"
        "<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<title>Cably Desktop</title>\n"
        "<style>body{font-family:-apple-system,system-ui,sans-serif;background:#0b1220;color:#e6edf7;"
        "display:flex;align-items:center;justify-content:center;height:100vh;margin:0}"
        "main{text-align:center;max-width:32rem;padding:2rem}h1{font-size:1.4rem;margin:0 0 .5rem}"
        "p{opacity:.8}</style></head>\n"
        "<body><main><h1 id=\"title\">Connecting to Cably Desktop\xe2\x80\xa6</h1>"
        "<p id=\"msg\">One moment.</p></main>\n"
        "<script>\n"
        "(function(){\n"
        "  var t=document.getElementById('title'),m=document.getElementById('msg');\n"
        "  var h=location.hash.replace(/^#/,'');\n"
        "  var o={};\n"
        "  new URLSearchParams(h).forEach(function(v,k){o[k]=v;});\n"
        "  try{history.replaceState(null,'',location.pathname);}catch(e){}\n"
        "  if(!o.state||!o.access_token){t.textContent='Nothing to connect.';"
        "m.textContent='Open Cably Desktop and choose Sign in again.';return;}\n"
        "  fetch('/token',{method:'POST',headers:{'content-type':'application/json'},"
        "body:JSON.stringify(o)}).then(function(r){\n"
        "    if(r.ok){t.textContent='Connected \xe2\x80\x94 return to Cably Desktop.';"
        "m.textContent='You can close this tab.';}\n"
        "    else{t.textContent='Sign-in was not accepted.';"
        "m.textContent='Return to Cably Desktop and try again ('+r.status+').';}\n"
        "  }).catch(function(){t.textContent='Could not reach Cably Desktop.';"
        "m.textContent='Is it still running? Try Sign in again.';});\n"
        "})();\n"
        "</script></body></html>\n";
}


bool CABLY_LOOPBACK_SERVER::acceptToken( const std::string& aBody )
{
    json j;

    try
    {
        j = json::parse( aBody );
    }
    catch( ... )
    {
        return false;
    }

    if( !j.is_object() || !j.contains( "state" ) || !j["state"].is_string() )
        return false;

    // THE check: only the state we issued for this listener may hand us a session, and
    // only once.  (Mutation-tested in cably/tests/bridge.sh.)
    if( m_accepted )
        return false;

    if( j["state"].get<std::string>() != m_state )
        return false;

    CABLY_SESSION s;
    s.accessToken = j.value( "access_token", "" );
    s.refreshToken = j.value( "refresh_token", "" );
    s.email = j.value( "email", "" );
    s.expiresAt = j.contains( "expires_at" ) ? jsonSeconds( j["expires_at"] ) : 0;

    if( s.Empty() )
        return false;

    {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_session = s;
        m_accepted = true;
    }

    m_cv.notify_all();
    return true;
}


int CABLY_LOOPBACK_SERVER::HandleRequest( const std::string& aMethod, const std::string& aPath,
                                          const std::string& aBody, std::string& aResponseBody,
                                          std::string& aContentType )
{
    std::string path = aPath.substr( 0, aPath.find( '?' ) );

    if( path == "/callback" )
    {
        if( aMethod != "GET" )
        {
            aContentType = "text/plain; charset=utf-8";
            aResponseBody = "method not allowed\n";
            return 405;
        }

        aContentType = "text/html; charset=utf-8";
        aResponseBody = CallbackHtml();
        return 200;
    }

    if( path == "/token" )
    {
        aContentType = "application/json";

        if( aMethod != "POST" )
        {
            aResponseBody = "{\"ok\":false,\"error\":\"method not allowed\"}";
            return 405;
        }

        if( acceptToken( aBody ) )
        {
            aResponseBody = "{\"ok\":true}";
            return 200;
        }

        aResponseBody = "{\"ok\":false,\"error\":\"rejected\"}";
        return 400;
    }

    aContentType = "text/plain; charset=utf-8";
    aResponseBody = "not found\n";
    return 404;
}


bool CABLY_LOOPBACK_SERVER::WaitForSession( int aTimeoutMs, CABLY_SESSION& aOut )
{
    std::unique_lock<std::mutex> lock( m_mutex );

    if( !m_accepted )
    {
        m_cv.wait_for( lock, std::chrono::milliseconds( std::max( 0, aTimeoutMs ) ),
                       [this] { return m_accepted.load(); } );
    }

    if( !m_accepted )
        return false;

    aOut = m_session;
    return true;
}


// -- sockets (POSIX; TODO Windows: winsock2 + closesocket) ------------------------------

bool CABLY_LOOPBACK_SERVER::Start( std::string* aError )
{
    if( m_listening )
        return true;

    int fd = ::socket( AF_INET, SOCK_STREAM, 0 );

    if( fd < 0 )
    {
        if( aError )
            *aError = std::string( "socket(): " ) + std::strerror( errno );

        return false;
    }

    sockaddr_in addr;
    std::memset( &addr, 0, sizeof( addr ) );
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK ); // 127.0.0.1 ONLY, never 0.0.0.0
    addr.sin_port = 0;                               // kernel-chosen random port

    if( ::bind( fd, reinterpret_cast<sockaddr*>( &addr ), sizeof( addr ) ) < 0
        || ::listen( fd, 8 ) < 0 )
    {
        if( aError )
            *aError = std::string( "bind/listen(): " ) + std::strerror( errno );

        ::close( fd );
        return false;
    }

    socklen_t len = sizeof( addr );

    if( ::getsockname( fd, reinterpret_cast<sockaddr*>( &addr ), &len ) < 0 )
    {
        if( aError )
            *aError = std::string( "getsockname(): " ) + std::strerror( errno );

        ::close( fd );
        return false;
    }

    m_listenFd = fd;
    m_port = ntohs( addr.sin_port );
    m_stopRequested = false;
    m_listening = true;
    m_thread = std::thread( [this] { serveLoop(); } );
    return true;
}


void CABLY_LOOPBACK_SERVER::Stop()
{
    m_stopRequested = true;

    if( m_thread.joinable() )
        m_thread.join();

    if( m_listenFd >= 0 )
    {
        ::close( m_listenFd );
        m_listenFd = -1;
    }

    m_listening = false;
}


void CABLY_LOOPBACK_SERVER::serveLoop()
{
    // poll() with a short timeout so Stop() can end the loop (macOS does not reliably
    // wake a blocked accept() when another thread closes the listening socket).
    while( !m_stopRequested )
    {
        pollfd pfd;
        pfd.fd = m_listenFd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int rc = ::poll( &pfd, 1, 100 );

        if( rc <= 0 )
            continue;

        sockaddr_in peer;
        socklen_t   peerLen = sizeof( peer );
        int client = ::accept( m_listenFd, reinterpret_cast<sockaddr*>( &peer ), &peerLen );

        if( client < 0 )
            continue;

        serveClient( client );
        ::close( client );

        if( m_accepted )
            break; // one session per listener: stop answering the moment we have it
    }

    // Close the listening socket from the serving thread so a client connecting after the
    // handoff is refused immediately (bridge.sh asserts "connection refused").
    if( m_listenFd >= 0 )
    {
        ::close( m_listenFd );
        m_listenFd = -1;
    }

    m_listening = false;
}


void CABLY_LOOPBACK_SERVER::serveClient( int aClientFd )
{
    timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    ::setsockopt( aClientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );
    ::setsockopt( aClientFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof( tv ) );

    // Read the head, then Content-Length bytes of body (bounded: a token payload is tiny).
    const size_t maxBytes = 64 * 1024;
    std::string  buf;
    size_t       headEnd = std::string::npos;
    char         chunk[4096];

    while( buf.size() < maxBytes )
    {
        ssize_t n = ::recv( aClientFd, chunk, sizeof( chunk ), 0 );

        if( n <= 0 )
            break;

        buf.append( chunk, static_cast<size_t>( n ) );
        headEnd = buf.find( "\r\n\r\n" );

        if( headEnd == std::string::npos )
            continue;

        size_t      contentLength = 0;
        std::string head = buf.substr( 0, headEnd );
        std::string lower = head;
        std::transform( lower.begin(), lower.end(), lower.begin(),
                        []( unsigned char c ) { return std::tolower( c ); } );
        size_t cl = lower.find( "\r\ncontent-length:" );

        if( cl != std::string::npos )
            contentLength = static_cast<size_t>( std::atol( head.c_str() + cl + 17 ) );

        if( contentLength > maxBytes )
            break;

        if( buf.size() - ( headEnd + 4 ) >= contentLength )
            break;
    }

    std::string method, target, body;

    if( headEnd != std::string::npos )
    {
        std::istringstream requestLine( buf.substr( 0, buf.find( "\r\n" ) ) );
        requestLine >> method >> target;
        body = buf.substr( headEnd + 4 );
    }

    std::string responseBody, contentType;
    int         status = 400;

    if( method.empty() || target.empty() )
    {
        contentType = "text/plain; charset=utf-8";
        responseBody = "bad request\n";
    }
    else
    {
        status = HandleRequest( method, target, body, responseBody, contentType );
    }

    const char* reason = "Bad Request";

    switch( status )
    {
    case 200: reason = "OK"; break;
    case 404: reason = "Not Found"; break;
    case 405: reason = "Method Not Allowed"; break;
    default: break;
    }

    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << responseBody.size() << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "Referrer-Policy: no-referrer\r\n"
        << "Connection: close\r\n\r\n"
        << responseBody;

    std::string response = out.str();
    size_t      sent = 0;

    while( sent < response.size() )
    {
        ssize_t n = ::send( aClientFd, response.data() + sent, response.size() - sent, 0 );

        if( n <= 0 )
            break;

        sent += static_cast<size_t>( n );
    }

    ::shutdown( aClientFd, SHUT_WR );
}


// ---------------------------------------------------------------------------------------
// CABLY_BRIDGE
// ---------------------------------------------------------------------------------------

CABLY_BRIDGE_CONFIG CABLY_BRIDGE_CONFIG::Default()
{
    CABLY_BRIDGE_CONFIG c;
    c.supabaseUrl = CABLY_SUPABASE_URL;
    c.publishableKey = CABLY_SUPABASE_PUBLISHABLE_KEY;
    c.engineUrl = CABLY_ENGINE_URL;
    c.authPageUrl = CABLY_DESKTOP_AUTH_URL;
    return c;
}


CABLY_BRIDGE::CABLY_BRIDGE( CABLY_HTTP& aHttp, CABLY_SECRET_STORE& aStore,
                            const CABLY_BRIDGE_CONFIG& aConfig ) :
        m_http( aHttp ),
        m_store( aStore ),
        m_config( aConfig )
{
}


CABLY_BRIDGE::~CABLY_BRIDGE()
{
    CancelLoopback();
}


std::string CABLY_BRIDGE::UrlEncode( const std::string& aText )
{
    static const char* hex = "0123456789ABCDEF";
    std::string        out;

    for( unsigned char c : aText )
    {
        if( std::isalnum( c ) || c == '-' || c == '_' || c == '.' || c == '~' )
        {
            out.push_back( static_cast<char>( c ) );
        }
        else
        {
            out.push_back( '%' );
            out.push_back( hex[c >> 4] );
            out.push_back( hex[c & 0x0f] );
        }
    }

    return out;
}


std::string CABLY_BRIDGE::BuildAuthUrl( const std::string& aBaseUrl, int aPort, const std::string& aState )
{
    return aBaseUrl + "?port=" + std::to_string( aPort ) + "&state=" + UrlEncode( aState );
}


int CABLY_BRIDGE::StartLoopback()
{
    CancelLoopback();
    m_loopback = std::make_unique<CABLY_LOOPBACK_SERVER>();
    std::string err;

    if( !m_loopback->Start( &err ) )
    {
        m_lastError = "Could not start the sign-in listener: " + err;
        m_loopback.reset();
        return 0;
    }

    return m_loopback->Port();
}


std::string CABLY_BRIDGE::AuthUrl() const
{
    if( !m_loopback )
        return std::string();

    return BuildAuthUrl( m_config.authPageUrl, m_loopback->Port(), m_loopback->State() );
}


bool CABLY_BRIDGE::FinishLoopback( int aTimeoutMs )
{
    if( !m_loopback )
    {
        m_lastError = "No sign-in in progress.";
        return false;
    }

    CABLY_SESSION s;
    bool          got = m_loopback->WaitForSession( aTimeoutMs, s );
    CancelLoopback();

    if( !got )
    {
        m_lastError = "Sign-in timed out.";
        return false;
    }

    setSession( s );
    return true;
}


void CABLY_BRIDGE::CancelLoopback()
{
    if( m_loopback )
    {
        m_loopback->Stop();
        m_loopback.reset();
    }
}


bool CABLY_BRIDGE::LoadSession()
{
    CABLY_SESSION s;

    if( !m_store.Load( s ) || s.Empty() )
    {
        m_session = CABLY_SESSION();
        return false;
    }

    m_session = s;
    return true;
}


bool CABLY_BRIDGE::SignOut()
{
    m_session = CABLY_SESSION();
    return m_store.Clear();
}


void CABLY_BRIDGE::setSession( const CABLY_SESSION& aSession )
{
    m_session = aSession;

    if( !m_store.Save( aSession ) )
        m_lastError = "Signed in, but the session could not be stored: " + m_store.LastError();
}


bool CABLY_BRIDGE::failFrom( const CABLY_HTTP_RESPONSE& aResponse, const std::string& aWhat )
{
    if( !aResponse.transportOk )
    {
        m_lastError = aWhat + ": " + ( aResponse.error.empty() ? "network error" : aResponse.error );
        return false;
    }

    std::string detail;

    try
    {
        json j = json::parse( aResponse.body );

        if( j.is_object() )
        {
            // Engine: {error:{code,message}}; Supabase auth: {message}/{error_description}/{msg}
            if( j.contains( "error" ) && j["error"].is_object() )
                detail = j["error"].value( "message", j["error"].value( "code", "" ) );
            else if( j.contains( "message" ) && j["message"].is_string() )
                detail = j["message"].get<std::string>();
            else if( j.contains( "error_description" ) && j["error_description"].is_string() )
                detail = j["error_description"].get<std::string>();
            else if( j.contains( "msg" ) && j["msg"].is_string() )
                detail = j["msg"].get<std::string>();
            else if( j.contains( "error" ) && j["error"].is_string() )
                detail = j["error"].get<std::string>();
        }
    }
    catch( ... )
    {
    }

    m_lastError = aWhat + ": HTTP " + std::to_string( aResponse.status )
                  + ( detail.empty() ? "" : " (" + detail + ")" );
    return false;
}


bool CABLY_BRIDGE::RefreshSession()
{
    if( m_session.refreshToken.empty() )
    {
        m_lastError = "Not signed in.";
        return false;
    }

    CABLY_HTTP_REQUEST req;
    req.method = "POST";
    req.url = m_config.supabaseUrl + "/auth/v1/token?grant_type=refresh_token";
    req.headers = { { "apikey", m_config.publishableKey },
                    { "Content-Type", "application/json" },
                    { "Accept", "application/json" } };
    req.body = json{ { "refresh_token", m_session.refreshToken } }.dump();

    CABLY_HTTP_RESPONSE res = m_http.Perform( req );

    if( !res.transportOk )
        return failFrom( res, "Refreshing the session" );

    if( res.status >= 400 && res.status < 500 )
    {
        // The refresh token is dead (revoked, rotated elsewhere, expired): forget it.
        failFrom( res, "Refreshing the session" );
        m_session = CABLY_SESSION();
        m_store.Clear();
        return false;
    }

    if( res.status != 200 )
        return failFrom( res, "Refreshing the session" );

    try
    {
        json          j = json::parse( res.body );
        CABLY_SESSION s;
        s.accessToken = j.value( "access_token", "" );
        s.refreshToken = j.value( "refresh_token", "" );
        s.email = m_session.email;

        if( j.contains( "user" ) && j["user"].is_object() && j["user"].contains( "email" )
            && j["user"]["email"].is_string() )
            s.email = j["user"]["email"].get<std::string>();

        if( j.contains( "expires_at" ) )
            s.expiresAt = jsonSeconds( j["expires_at"] );
        else if( j.contains( "expires_in" ) )
            s.expiresAt = nowSeconds() + jsonSeconds( j["expires_in"] );

        if( s.Empty() )
        {
            m_lastError = "Refreshing the session: the reply carried no tokens.";
            return false;
        }

        setSession( s );
        return true;
    }
    catch( ... )
    {
        m_lastError = "Refreshing the session: the reply was not JSON.";
        return false;
    }
}


bool CABLY_BRIDGE::supabaseRequest( const std::string& aMethod, const std::string& aPath,
                                    const std::string& aBody, CABLY_HTTP_RESPONSE& aOut,
                                    const std::vector<std::pair<std::string, std::string>>& aExtraHeaders )
{
    if( !HasSession() )
    {
        m_lastError = "Not signed in.";
        return false;
    }

    auto perform = [&]()
    {
        CABLY_HTTP_REQUEST req;
        req.method = aMethod;
        req.url = m_config.supabaseUrl + aPath;
        req.headers = { { "apikey", m_config.publishableKey },
                        { "Authorization", "Bearer " + m_session.accessToken },
                        { "Accept", "application/json" } };

        if( !aBody.empty() )
        {
            req.headers.push_back( { "Content-Type", "application/json" } );
            req.body = aBody;
        }

        for( const auto& h : aExtraHeaders )
            req.headers.push_back( h );

        return m_http.Perform( req );
    };

    aOut = perform();

    if( aOut.transportOk && aOut.status == 401 )
    {
        if( !RefreshSession() )
            return false;

        aOut = perform();
    }

    return aOut.transportOk;
}


bool CABLY_BRIDGE::engineRequest( const std::string& aPath, const std::string& aBody,
                                  CABLY_HTTP_RESPONSE& aOut )
{
    if( !HasSession() )
    {
        m_lastError = "Not signed in.";
        return false;
    }

    auto perform = [&]()
    {
        CABLY_HTTP_REQUEST req;
        req.method = "POST";
        req.url = m_config.engineUrl + aPath;
        req.headers = { { "Authorization", "Bearer " + m_session.accessToken },
                        { "Content-Type", "application/json" },
                        { "Accept", "application/json" } };
        req.body = aBody;
        return m_http.Perform( req );
    };

    aOut = perform();

    if( aOut.transportOk && aOut.status == 401 )
    {
        CABLY_HTTP_RESPONSE first = aOut;

        if( !RefreshSession() )
        {
            // Report the engine's own message, not the refresh's.
            failFrom( first, "Cloud engine" );
            return false;
        }

        aOut = perform();
    }

    return aOut.transportOk;
}


bool CABLY_BRIDGE::ValidateSession()
{
    CABLY_HTTP_RESPONSE res;

    if( !supabaseRequest( "GET", "/auth/v1/user", "", res ) )
    {
        if( !res.transportOk && !res.error.empty() )
            failFrom( res, "Checking the session" );

        return false;
    }

    if( res.status != 200 )
    {
        failFrom( res, "Checking the session" );

        if( res.status == 401 || res.status == 403 )
        {
            m_session = CABLY_SESSION();
            m_store.Clear();
        }

        return false;
    }

    try
    {
        json j = json::parse( res.body );

        if( j.contains( "email" ) && j["email"].is_string() )
        {
            std::string email = j["email"].get<std::string>();

            if( !email.empty() && email != m_session.email )
            {
                m_session.email = email;
                m_store.Save( m_session );
            }
        }
    }
    catch( ... )
    {
    }

    return true;
}


bool CABLY_BRIDGE::ListProjects( std::vector<CABLY_PROJECT_SUMMARY>& aOut )
{
    CABLY_HTTP_RESPONSE res;

    if( !supabaseRequest( "GET",
                          "/rest/v1/projects?select=id,name,updated_at&order=updated_at.desc&limit=50",
                          "", res ) )
    {
        if( !res.transportOk && !res.error.empty() )
            failFrom( res, "Listing projects" );

        return false;
    }

    if( res.status != 200 )
        return failFrom( res, "Listing projects" );

    try
    {
        json j = json::parse( res.body );

        if( !j.is_array() )
        {
            m_lastError = "Listing projects: unexpected reply.";
            return false;
        }

        aOut.clear();

        for( const json& row : j )
        {
            CABLY_PROJECT_SUMMARY p;
            p.id = row.value( "id", "" );
            p.name = row.value( "name", "" );
            p.updatedAt = row.value( "updated_at", "" );

            if( !p.id.empty() )
                aOut.push_back( p );
        }

        return true;
    }
    catch( ... )
    {
        m_lastError = "Listing projects: the reply was not JSON.";
        return false;
    }
}


bool CABLY_BRIDGE::fetchRow( const std::string& aId, bool aWithVersion, std::string& aDataJson,
                             std::string& aUpdatedAt )
{
    CABLY_HTTP_RESPONSE res;
    std::string         path = "/rest/v1/projects?id=eq." + UrlEncode( aId )
                       + ( aWithVersion ? "&select=data,updated_at" : "&select=data" );

    if( !supabaseRequest( "GET", path, "", res ) )
    {
        if( !res.transportOk && !res.error.empty() )
            failFrom( res, "Fetching the project" );

        return false;
    }

    if( res.status != 200 )
        return failFrom( res, "Fetching the project" );

    try
    {
        json j = json::parse( res.body );

        if( !j.is_array() || j.empty() || !j[0].is_object() || !j[0].contains( "data" ) )
        {
            m_lastError = "Fetching the project: no such project (or not yours).";
            return false;
        }

        aDataJson = j[0]["data"].dump();
        aUpdatedAt = j[0].value( "updated_at", "" );
        return true;
    }
    catch( ... )
    {
        m_lastError = "Fetching the project: the reply was not JSON.";
        return false;
    }
}


/// PersistedProjectSession { schema, project, chat } or a legacy row that IS the project.
static const json* projectOfData( const json& aData )
{
    if( aData.is_object() && aData.contains( "project" ) && aData["project"].is_object() )
        return &aData["project"];

    if( aData.is_object() && aData.contains( "project_name" ) )
        return &aData;

    return nullptr;
}


bool CABLY_BRIDGE::FetchProject( const std::string& aId, std::string& aProjectJson,
                                 std::string* aUpdatedAt )
{
    std::string dataJson, updatedAt;

    if( !fetchRow( aId, aUpdatedAt != nullptr, dataJson, updatedAt ) )
        return false;

    json        data = json::parse( dataJson );
    const json* project = projectOfData( data );

    if( !project )
    {
        m_lastError = "Fetching the project: the row carries no project.";
        return false;
    }

    aProjectJson = project->dump();

    if( aUpdatedAt )
        *aUpdatedAt = updatedAt;

    return true;
}


CABLY_IMPORT_OUTCOME CABLY_BRIDGE::ImportProject( const std::string& aId, const std::string* aKicadPcb,
                                                  const std::string* aKicadSch,
                                                  const std::string& aExpectedUpdatedAt,
                                                  CABLY_IMPORT_RESULT& aOut )
{
    aOut = CABLY_IMPORT_RESULT();

    if( !aKicadPcb && !aKicadSch )
    {
        m_lastError = "Syncing: nothing to import (no board, no schematic).";
        return CABLY_IMPORT_OUTCOME::FAILED;
    }

    if( !HasSession() )
    {
        m_lastError = "Not signed in.";
        return CABLY_IMPORT_OUTCOME::FAILED;
    }

    // 1. the row and its version
    std::string dataJson, rowUpdatedAt;

    if( !fetchRow( aId, true, dataJson, rowUpdatedAt ) )
        return CABLY_IMPORT_OUTCOME::FAILED;

    aOut.cloudUpdatedAt = rowUpdatedAt;

    // THE RULE: the cloud moved on since we exported -> never overwrite; let the UI ask.
    // (Mutation-tested in cably/tests/bridge.sh: the inverted comparison fails the tests.)
    if( !aExpectedUpdatedAt.empty() && cloudIsNewer( rowUpdatedAt, aExpectedUpdatedAt ) )
    {
        aOut.outcome = CABLY_IMPORT_OUTCOME::CLOUD_CHANGED;
        return aOut.outcome;
    }

    json        data = json::parse( dataJson );
    const json* project = projectOfData( data );

    if( !project )
    {
        m_lastError = "Syncing: the row carries no project.";
        return CABLY_IMPORT_OUTCOME::FAILED;
    }

    // 2. the engine applies the KiCad files to the project
    json body;
    body["apiVersion"] = 1;
    body["project"] = *project;

    if( aKicadPcb )
        body["kicadPcb"] = *aKicadPcb;

    if( aKicadSch )
        body["kicadSch"] = *aKicadSch;

    CABLY_HTTP_RESPONSE res;

    if( !engineRequest( "/v1/import", body.dump(), res ) )
    {
        if( !res.transportOk && !res.error.empty() )
            failFrom( res, "Cloud engine" );

        return CABLY_IMPORT_OUTCOME::FAILED;
    }

    if( res.status != 200 )
    {
        failFrom( res, "Cloud engine" );
        return CABLY_IMPORT_OUTCOME::FAILED;
    }

    json reply;

    try
    {
        reply = json::parse( res.body );
    }
    catch( ... )
    {
        m_lastError = "Cloud engine: the reply was not JSON.";
        return CABLY_IMPORT_OUTCOME::FAILED;
    }

    if( !reply.is_object() || !reply.contains( "project" ) || !reply["project"].is_object() )
    {
        m_lastError = "Cloud engine: the reply carried no project.";
        return CABLY_IMPORT_OUTCOME::FAILED;
    }

    aOut.project = reply["project"].dump();
    aOut.pcbReport = reply.contains( "pcbReport" ) ? reply["pcbReport"].dump() : "null";
    aOut.schReport = reply.contains( "schReport" ) ? reply["schReport"].dump() : "null";
    aOut.engineVersion = reply.value( "engineVersion", "" );

    // 3. persist: ONLY data.project changes; schema and chat go back exactly as fetched
    if( data.is_object() && data.contains( "project" ) )
        data["project"] = reply["project"];
    else
        data = reply["project"]; // legacy row: the data IS the project

    json patch;
    patch["data"] = data;

    if( !supabaseRequest( "PATCH", "/rest/v1/projects?id=eq." + UrlEncode( aId ), patch.dump(), res,
                          { { "Prefer", "return=minimal" } } ) )
    {
        if( !res.transportOk && !res.error.empty() )
            failFrom( res, "Saving to the cloud" );

        return CABLY_IMPORT_OUTCOME::FAILED;
    }

    if( res.status != 204 && res.status != 200 )
    {
        failFrom( res, "Saving to the cloud" );
        return CABLY_IMPORT_OUTCOME::FAILED;
    }

    // 4. the version the trigger stamped
    if( !supabaseRequest( "GET", "/rest/v1/projects?id=eq." + UrlEncode( aId ) + "&select=updated_at", "", res ) )
    {
        if( !res.transportOk && !res.error.empty() )
            failFrom( res, "Reading the saved version" );

        return CABLY_IMPORT_OUTCOME::FAILED;
    }

    if( res.status != 200 )
    {
        failFrom( res, "Reading the saved version" );
        return CABLY_IMPORT_OUTCOME::FAILED;
    }

    try
    {
        json j = json::parse( res.body );

        if( j.is_array() && !j.empty() && j[0].is_object() )
            aOut.updatedAt = j[0].value( "updated_at", "" );
    }
    catch( ... )
    {
    }

    if( aOut.updatedAt.empty() )
    {
        m_lastError = "Reading the saved version: the reply carried no updated_at.";
        return CABLY_IMPORT_OUTCOME::FAILED;
    }

    aOut.outcome = CABLY_IMPORT_OUTCOME::SYNCED;
    return aOut.outcome;
}


bool CABLY_BRIDGE::ExportProject( const std::string& aProjectJson, CABLY_EXPORT_RESULT& aOut )
{
    json body;

    try
    {
        body["apiVersion"] = 1;
        body["project"] = json::parse( aProjectJson );
    }
    catch( ... )
    {
        m_lastError = "Exporting: the project is not valid JSON.";
        return false;
    }

    CABLY_HTTP_RESPONSE res;

    if( !engineRequest( "/v1/export", body.dump(), res ) )
    {
        if( !res.transportOk && !res.error.empty() )
            failFrom( res, "Cloud engine" );

        return false;
    }

    if( res.status != 200 )
        return failFrom( res, "Cloud engine" );

    try
    {
        json j = json::parse( res.body );

        if( !j.is_object() || !j.contains( "kicadPcb" ) || !j.contains( "kicadSch" ) )
        {
            m_lastError = "Cloud engine: the reply carried no KiCad files.";
            return false;
        }

        aOut = CABLY_EXPORT_RESULT();
        aOut.hasPcb = j["kicadPcb"].is_string();
        aOut.hasSch = j["kicadSch"].is_string();

        if( aOut.hasPcb )
            aOut.kicadPcb = j["kicadPcb"].get<std::string>();

        if( aOut.hasSch )
            aOut.kicadSch = j["kicadSch"].get<std::string>();

        aOut.engineVersion = j.value( "engineVersion", "" );
        return true;
    }
    catch( ... )
    {
        m_lastError = "Cloud engine: the reply was not JSON.";
        return false;
    }
}


std::string CABLY_BRIDGE::Sha256Hex( const std::string& aText )
{
    return sha256Hex( aText );
}


/// "YYYY-MM-DDTHH:MM:SS[.frac][Z|+HH:MM|-HH:MM]" -> microseconds since the epoch, UTC.
static bool parseIsoMicros( const std::string& aText, long long& aMicros )
{
    size_t p = 0;

    auto digits = [&]( int aCount, long long& aOut )
    {
        if( p + aCount > aText.size() )
            return false;

        aOut = 0;

        for( int i = 0; i < aCount; ++i )
        {
            char c = aText[p + i];

            if( c < '0' || c > '9' )
                return false;

            aOut = aOut * 10 + ( c - '0' );
        }

        p += aCount;
        return true;
    };

    auto expect = [&]( char aChar )
    {
        if( p < aText.size() && aText[p] == aChar )
        {
            ++p;
            return true;
        }

        return false;
    };

    long long year, month, day, hour, minute, second;

    if( !digits( 4, year ) || !expect( '-' ) || !digits( 2, month ) || !expect( '-' ) || !digits( 2, day ) )
        return false;

    if( !( expect( 'T' ) || expect( 't' ) || expect( ' ' ) ) )
        return false;

    if( !digits( 2, hour ) || !expect( ':' ) || !digits( 2, minute ) || !expect( ':' ) || !digits( 2, second ) )
        return false;

    if( month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60 )
        return false;

    long long micros = 0;

    if( expect( '.' ) || expect( ',' ) )
    {
        int  n = 0;
        long long scale = 100000;

        while( p < aText.size() && aText[p] >= '0' && aText[p] <= '9' )
        {
            if( n < 6 )
                micros += ( aText[p] - '0' ) * scale;

            scale /= 10;
            ++n;
            ++p;
        }

        if( n == 0 )
            return false;
    }

    long long offsetMinutes = 0;

    if( expect( 'Z' ) || expect( 'z' ) )
    {
        // UTC
    }
    else if( p < aText.size() && ( aText[p] == '+' || aText[p] == '-' ) )
    {
        int sign = aText[p] == '-' ? -1 : 1;
        ++p;
        long long oh, om = 0;

        if( !digits( 2, oh ) )
            return false;

        if( expect( ':' ) || ( p < aText.size() && aText[p] >= '0' && aText[p] <= '9' ) )
        {
            if( !digits( 2, om ) )
                return false;
        }

        offsetMinutes = sign * ( oh * 60 + om );
    }

    if( p != aText.size() )
        return false;

    // days from civil (Howard Hinnant), proleptic Gregorian
    long long y = year - ( month <= 2 ? 1 : 0 );
    long long era = ( y >= 0 ? y : y - 399 ) / 400;
    long long yoe = y - era * 400;
    long long doy = ( 153 * ( month + ( month > 2 ? -3 : 9 ) ) + 2 ) / 5 + day - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;

    long long seconds = days * 86400 + hour * 3600 + minute * 60 + second - offsetMinutes * 60;
    aMicros = seconds * 1000000 + micros;
    return true;
}


int CABLY_BRIDGE::CompareIsoTimestamps( const std::string& aA, const std::string& aB )
{
    long long a, b;

    if( parseIsoMicros( aA, a ) && parseIsoMicros( aB, b ) )
        return a < b ? -1 : ( a > b ? 1 : 0 );

    int c = aA.compare( aB );
    return c < 0 ? -1 : ( c > 0 ? 1 : 0 );
}


/// The cloud-changed test: newer when both stamps parse; when either does not, ANY
/// difference counts (never overwrite on doubt).
static bool cloudIsNewer( const std::string& aRowUpdatedAt, const std::string& aExpected )
{
    long long row, expected;

    if( parseIsoMicros( aRowUpdatedAt, row ) && parseIsoMicros( aExpected, expected ) )
        return row > expected;

    return aRowUpdatedAt != aExpected;
}


bool CABLY_EXPORT_SIDECAR::Load( const std::string& aDir, CABLY_EXPORT_SIDECAR& aOut )
{
    std::string text;

    if( !readWholeFile( fs::path( aDir ) / FileName(), text ) )
        return false;

    try
    {
        json j = json::parse( text );

        if( !j.is_object() )
            return false;

        CABLY_EXPORT_SIDECAR sc;
        sc.stem = j.value( "stem", "" );

        if( j.contains( "files" ) && j["files"].is_object() )
        {
            for( auto it = j["files"].begin(); it != j["files"].end(); ++it )
            {
                if( it.value().is_string() )
                    sc.files[it.key()] = it.value().get<std::string>();
            }
        }

        sc.meta.projectId = j.value( "projectId", "" );
        sc.meta.projectName = j.value( "projectName", "" );
        sc.meta.cloudUpdatedAt = j.value( "cloudUpdatedAt", "" );
        sc.meta.engineVersion = j.value( "engineVersion", "" );
        aOut = sc;
        return true;
    }
    catch( ... )
    {
        return false;
    }
}


bool CABLY_EXPORT_SIDECAR::Save( const std::string& aDir ) const
{
    json j;
    j["stem"] = stem;
    j["files"] = json::object();

    for( const auto& f : files )
        j["files"][f.first] = f.second;

    j["engine"] = "cably";
    j["projectId"] = meta.projectId;
    j["projectName"] = meta.projectName;
    j["cloudUpdatedAt"] = meta.cloudUpdatedAt;
    j["engineVersion"] = meta.engineVersion;
    return writeWholeFile( fs::path( aDir ) / FileName(), j.dump( 2 ) + "\n" );
}


std::string CABLY_BRIDGE::SafeStem( const std::string& aName )
{
    std::string out;
    bool        pendingUnderscore = false;

    for( unsigned char c : aName )
    {
        bool keep = std::isalnum( c ) || c == '_' || c == '-';

        if( keep )
        {
            if( pendingUnderscore && !out.empty() )
                out.push_back( '_' );

            pendingUnderscore = false;
            out.push_back( static_cast<char>( c ) );
        }
        else
        {
            pendingUnderscore = true;
        }
    }

    auto trim = []( std::string& s )
    {
        while( !s.empty() && s.front() == '_' )
            s.erase( s.begin() );

        while( !s.empty() && s.back() == '_' )
            s.pop_back();
    };

    trim( out );

    if( out.size() > 64 )
        out.resize( 64 );

    trim( out );
    return out.empty() ? std::string( "cably-project" ) : out;
}


CABLY_WRITE_RESULT CABLY_BRIDGE::WriteProjectFolder( const std::string& aRoot, const std::string& aStem,
                                                     const std::string& aPcb, const std::string& aSch,
                                                     bool aForce, const CABLY_EXPORT_META* aMeta )
{
    CABLY_WRITE_RESULT r;
    fs::path           dir = fs::path( aRoot ) / SafeStem( aStem );
    r.dir = dir.string();

    std::error_code ec;
    fs::create_directories( dir, ec );

    if( ec )
    {
        r.error = "Could not create " + r.dir + ": " + ec.message();
        return r;
    }

    // Previous manifest: keeps the stem KiCad already knows, the baselines we wrote and
    // (F5) the cloud row it came from.  An unreadable manifest gives no baselines: every
    // existing file then counts as the user's.
    CABLY_EXPORT_SIDECAR previous;
    std::string          stem = SafeStem( aStem );

    if( CABLY_EXPORT_SIDECAR::Load( dir.string(), previous ) && !previous.stem.empty() )
        stem = previous.stem;

    json previousFiles = json::object();

    for( const auto& f : previous.files )
        previousFiles[f.first] = f.second;

    fs::path proPath = dir / ( stem + ".kicad_pro" );
    fs::path pcbPath = dir / ( stem + ".kicad_pcb" );
    fs::path schPath = dir / ( stem + ".kicad_sch" );
    r.proPath = proPath.string();
    r.pcbPath = pcbPath.string();
    r.schPath = aSch.empty() ? std::string() : schPath.string();

    std::vector<std::pair<fs::path, const std::string*>> targets;
    targets.push_back( { pcbPath, &aPcb } );

    if( !aSch.empty() )
        targets.push_back( { schPath, &aSch } );

    // THE RULE: a file that differs from both its baseline and the new text was edited in
    // KiCad; a file without a baseline is of unknown provenance - both are the user's.
    // (Mutation-tested in cably/tests/bridge.sh.)
    for( const auto& t : targets )
    {
        if( !fs::exists( t.first ) )
            continue;

        std::string current;

        if( !readWholeFile( t.first, current ) )
        {
            r.conflicts.push_back( t.first.string() );
            continue;
        }

        std::string currentHash = sha256Hex( current );

        if( currentHash == sha256Hex( *t.second ) )
            continue; // already what we want

        std::string name = t.first.filename().string();
        bool        hasBaseline = previousFiles.contains( name ) && previousFiles[name].is_string();

        if( !hasBaseline || previousFiles[name].get<std::string>() != currentHash )
            r.conflicts.push_back( t.first.string() );
    }

    if( !r.conflicts.empty() && !aForce )
        return r;

    r.conflicts.clear();
    json files = previousFiles;

    for( const auto& t : targets )
    {
        if( !writeWholeFile( t.first, *t.second ) )
        {
            r.error = "Could not write " + t.first.string();
            return r;
        }

        files[t.first.filename().string()] = sha256Hex( *t.second );
    }

    if( !fs::exists( proPath ) )
    {
        // Minimal project file; KiCad fills in defaults and then OWNS it (board settings
        // live there), which is why it is only ever created, never rewritten.
        json pro;
        pro["meta"]["filename"] = stem + ".kicad_pro";
        pro["meta"]["version"] = 3;

        if( !writeWholeFile( proPath, pro.dump( 2 ) + "\n" ) )
        {
            r.error = "Could not write " + proPath.string();
            return r;
        }
    }

    CABLY_EXPORT_SIDECAR sidecar;
    sidecar.stem = stem;
    sidecar.meta = aMeta ? *aMeta : previous.meta;

    for( auto it = files.begin(); it != files.end(); ++it )
        sidecar.files[it.key()] = it.value().get<std::string>();

    if( !sidecar.Save( dir.string() ) )
    {
        r.error = "Could not write " + ( dir / CABLY_EXPORT_SIDECAR::FileName() ).string();
        return r;
    }

    r.written = true;
    return r;
}

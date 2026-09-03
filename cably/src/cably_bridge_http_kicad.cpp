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
 * CABLY_HTTP_KICAD: the CABLY_HTTP transport on top of KiCad's own curl wrapper
 * (include/kicad_curl/kicad_curl_easy.h, target kicommon).  Requires KICAD_CURL::Init()
 * to have run (PGM_BASE does it for the app; the CLI does it itself).
 */

// kicad_curl headers must precede any wxWidgets header (see kicad_curl.h).
#include <kicad_curl/kicad_curl.h>
#include <kicad_curl/kicad_curl_easy.h>

#include <cably_bridge.h>

#include <exception>


CABLY_HTTP_RESPONSE CABLY_HTTP_KICAD::Perform( const CABLY_HTTP_REQUEST& aRequest )
{
    CABLY_HTTP_RESPONSE res;

    try
    {
        KICAD_CURL_EASY curl;
        curl.SetURL( aRequest.url );
        curl.SetFollowRedirects( false ); // never let a redirect carry the bearer elsewhere
        curl.SetConnectTimeout( 15L );
        curl.SetTimeout( 120L );          // /v1/export renders a board; bounded but generous

        for( const auto& h : aRequest.headers )
            curl.SetHeader( h.first, h.second );

        if( aRequest.method == "POST" || aRequest.method == "PATCH" )
            curl.SetPostFields( aRequest.body ); // sets CURLOPT_COPYPOSTFIELDS => POST

        if( aRequest.method == "PATCH" ) // F5: PostgREST row update; a POST on the wire otherwise
            curl_easy_setopt( curl.GetCurl(), CURLOPT_CUSTOMREQUEST, "PATCH" );

        int code = curl.Perform();

        if( code != 0 )
        {
            res.transportOk = false;
            res.error = curl.GetErrorText( code );
            return res;
        }

        res.transportOk = true;
        res.status = curl.GetResponseStatusCode();
        res.body = curl.GetBuffer();
        return res;
    }
    catch( const std::exception& e )
    {
        res.transportOk = false;
        res.error = e.what();
        return res;
    }
    catch( ... )
    {
        res.transportOk = false;
        res.error = "curl failed";
        return res;
    }
}

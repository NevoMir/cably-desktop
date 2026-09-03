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
 * Standalone unit test for the pure helpers in cably/src/cably_home_cloud.h
 * (F4: the home panel's cloud side).  Built and run by cably/tests/home.sh with
 * the toolchain's wx-config; wxBase only, no GUI, no KiCad libraries.
 */

#include <cably_home_cloud.h>

#include <wx/init.h>

#include <cstdio>
#include <cstdlib>
#include <string>

static int g_checks = 0;

#define CHECK( cond )                                                                    \
    do                                                                                   \
    {                                                                                    \
        ++g_checks;                                                                      \
        if( !( cond ) )                                                                  \
        {                                                                                \
            std::fprintf( stderr, "CHECK failed at line %d: %s\n", __LINE__, #cond );    \
            std::exit( 1 );                                                              \
        }                                                                                \
    } while( 0 )


int main()
{
    wxInitializer wxinit;

    // --- Project stem: mirrors the web app's safeProjectFileStem (desktop/handoff.ts) ---
    // Runs of anything but [A-Za-z0-9_-] collapse to one '_', edge underscores go, no dots
    // (KiCad reads the last one as the extension), 64 chars max, never empty.
    CHECK( CablyProjectStem( wxS( "Blink LED" ) ) == wxS( "Blink_LED" ) );
    CHECK( CablyProjectStem( wxS( "  v2.1 / amp!  " ) ) == wxS( "v2_1_amp" ) );
    CHECK( CablyProjectStem( wxS( "Café timer" ) ) == wxS( "Caf_timer" ) );
    CHECK( CablyProjectStem( wxS( "keep-this_one" ) ) == wxS( "keep-this_one" ) );
    CHECK( CablyProjectStem( wxS( "" ) ) == wxS( "cably-project" ) );
    CHECK( CablyProjectStem( wxS( "..." ) ) == wxS( "cably-project" ) );
    CHECK( CablyProjectStem( wxS( "☃☃" ) ) == wxS( "cably-project" ) );
    {
        wxString longName( 'a', 70 );
        longName += wxS( "_" );
        wxString stem = CablyProjectStem( longName );
        CHECK( stem.length() == 64 );
        CHECK( stem == wxString( 'a', 64 ) );
        // a slice that ends in '_' is trimmed again, like the web app
        CHECK( CablyProjectStem( wxString( 'b', 63 ) + wxS( "_xyz" ) ) == wxString( 'b', 63 ) );
    }

    // --- ISO-8601 (Postgres / Supabase) timestamps -> UTC wxDateTime ---
    wxDateTime dt;
    CHECK( CablyParseIsoUtc( wxS( "2026-09-03T14:05:07.123456+00:00" ), dt ) );
    CHECK( dt.Format( wxS( "%Y-%m-%d %H:%M:%S" ), wxDateTime::UTC ) == wxS( "2026-09-03 14:05:07" ) );
    CHECK( CablyParseIsoUtc( wxS( "2026-09-03T14:05:07Z" ), dt ) );
    CHECK( dt.Format( wxS( "%Y-%m-%d %H:%M:%S" ), wxDateTime::UTC ) == wxS( "2026-09-03 14:05:07" ) );
    CHECK( CablyParseIsoUtc( wxS( "2026-09-03T14:05:07" ), dt ) );   // no zone = UTC
    CHECK( dt.Format( wxS( "%Y-%m-%d %H:%M:%S" ), wxDateTime::UTC ) == wxS( "2026-09-03 14:05:07" ) );
    CHECK( CablyParseIsoUtc( wxS( "2026-09-03 01:30:00+03:00" ), dt ) );   // space separator, +03
    CHECK( dt.Format( wxS( "%Y-%m-%d %H:%M:%S" ), wxDateTime::UTC ) == wxS( "2026-09-02 22:30:00" ) );
    CHECK( CablyParseIsoUtc( wxS( "2026-01-01T00:10:00-0130" ), dt ) );   // -HHMM form
    CHECK( dt.Format( wxS( "%Y-%m-%d %H:%M:%S" ), wxDateTime::UTC ) == wxS( "2026-01-01 01:40:00" ) );
    CHECK( CablyParseIsoUtc( wxS( "2024-02-29T12:00:00Z" ), dt ) );        // leap day
    CHECK( dt.Format( wxS( "%Y-%m-%d" ), wxDateTime::UTC ) == wxS( "2024-02-29" ) );
    CHECK( !CablyParseIsoUtc( wxS( "" ), dt ) );
    CHECK( !CablyParseIsoUtc( wxS( "yesterday" ), dt ) );
    CHECK( !CablyParseIsoUtc( wxS( "2026-13-01T00:00:00Z" ), dt ) );
    CHECK( !CablyParseIsoUtc( wxS( "2026-09-03T25:00:00Z" ), dt ) );
    CHECK( !CablyParseIsoUtc( wxS( "2026-09-3T14:05:07Z" ), dt ) );

    // --- "Updated ..." wording, relative to an injected 'now' ---
    wxDateTime now;
    CHECK( CablyParseIsoUtc( wxS( "2026-09-03T12:00:00Z" ), now ) );
    auto at = []( const char* aIso )
    {
        wxDateTime d;
        CablyParseIsoUtc( wxString::FromUTF8( aIso ), d );
        return d;
    };
    const wxDateTime::TimeZone utc( wxDateTime::UTC );
    CHECK( CablyFormatUpdated( at( "2026-09-03T11:59:30Z" ), now, utc ) == wxS( "just now" ) );
    CHECK( CablyFormatUpdated( at( "2026-09-03T12:00:05Z" ), now, utc ) == wxS( "just now" ) );  // clock skew
    CHECK( CablyFormatUpdated( at( "2026-09-03T11:55:00Z" ), now, utc ) == wxS( "5 min ago" ) );
    CHECK( CablyFormatUpdated( at( "2026-09-03T09:00:00Z" ), now, utc ) == wxS( "3 h ago" ) );
    CHECK( CablyFormatUpdated( at( "2026-09-02T14:00:00Z" ), now, utc ) == wxS( "22 h ago" ) );
    CHECK( CablyFormatUpdated( at( "2026-09-02T09:00:00Z" ), now, utc ) == wxS( "yesterday" ) );
    CHECK( CablyFormatUpdated( at( "2026-08-31T12:00:00Z" ), now, utc ) == wxS( "3 days ago" ) );
    CHECK( CablyFormatUpdated( at( "2026-08-20T12:00:00Z" ), now, utc ) == wxS( "20 Aug 2026" ) );
    CHECK( CablyFormatUpdated( at( "2025-12-24T12:00:00Z" ), now, utc ) == wxS( "24 Dec 2025" ) );
    CHECK( CablyFormatUpdated( wxDateTime(), now, utc ) == wxEmptyString );   // invalid -> empty

    // --- Percent-encoding (RFC 3986 unreserved kept, UTF-8 bytes otherwise) ---
    CHECK( CablyPercentEncode( "abc-XYZ_0.9~" ) == wxS( "abc-XYZ_0.9~" ) );
    CHECK( CablyPercentEncode( "a b&c=d/e?f#g" ) == wxS( "a%20b%26c%3Dd%2Fe%3Ff%23g" ) );
    CHECK( CablyPercentEncode( "\xc3\xa9" ) == wxS( "%C3%A9" ) );
    CHECK( CablyPercentEncode( "" ) == wxEmptyString );
    CHECK( CablyPercentEncode( "\n+" ) == wxS( "%0A%2B" ) );

    // --- Generate URL: the prompt goes to cably.dev/app?prompt=... ---
    CHECK( CablyGenerateUrl( wxS( "https://cably.dev/app" ), wxS( "a 555 blinker" ) )
           == wxS( "https://cably.dev/app?prompt=a%20555%20blinker" ) );
    CHECK( CablyGenerateUrl( wxS( "https://cably.dev/app/" ), wxS( "  x  " ) )
           == wxS( "https://cably.dev/app?prompt=x" ) );
    CHECK( CablyGenerateUrl( wxS( "https://cably.dev/app" ), wxS( "   " ) ) == wxS( "https://cably.dev/app" ) );
    CHECK( CablyGenerateUrl( wxS( "https://cably.dev/app" ), wxS( "5 V → 3.3 V LDO" ) )
           == wxS( "https://cably.dev/app?prompt=5%20V%20%E2%86%92%203.3%20V%20LDO" ) );

    std::printf( "test_cably_home_cloud: %d checks passed\n", g_checks );
    return 0;
}

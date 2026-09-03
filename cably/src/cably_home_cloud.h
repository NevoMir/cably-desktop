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

#ifndef CABLY_HOME_CLOUD_H
#define CABLY_HOME_CLOUD_H

/**
 * Pure helpers behind the cloud side of the Cably home screen (F4): the project
 * folder stem, Supabase timestamps, the "updated ..." wording of the projects list
 * and the generate-on-web URL.
 *
 * wxBase only (no GUI, no KiCad libraries, no network) so that
 * cably/tests/unit/test_cably_home_cloud.cpp builds it standalone.
 */

#include <wx/datetime.h>
#include <wx/string.h>

#include <cstdint>
#include <string>


/**
 * A project's display name -> the file stem KiCad and every filesystem accept.
 *
 * Mirrors the web app's desktop hand-off (safeProjectFileStem): runs of anything
 * outside [A-Za-z0-9_-] become one '_', leading/trailing underscores are dropped, the
 * result is cut to 64 characters (and trimmed again), and an empty result falls back
 * to "cably-project".  No dots: KiCad reads the last one as the extension.
 */
inline wxString CablyProjectStem( const wxString& aName )
{
    std::string out;
    bool        pendingUnderscore = false;

    for( wxUniChar ch : aName )
    {
        const bool keep = ( ch >= 'A' && ch <= 'Z' ) || ( ch >= 'a' && ch <= 'z' )
                          || ( ch >= '0' && ch <= '9' ) || ch == '_' || ch == '-';

        if( keep )
        {
            if( pendingUnderscore && !out.empty() )
                out += '_';

            pendingUnderscore = false;
            out += (char) ch.GetValue();
        }
        else
        {
            pendingUnderscore = true;   // collapse the run; emitted only between kept chars
        }
    }

    auto trim = []( std::string& s )
    {
        size_t b = s.find_first_not_of( '_' );

        if( b == std::string::npos )
        {
            s.clear();
            return;
        }

        size_t e = s.find_last_not_of( '_' );
        s = s.substr( b, e - b + 1 );
    };

    trim( out );

    if( out.size() > 64 )
    {
        out.resize( 64 );
        trim( out );
    }

    if( out.empty() )
        out = "cably-project";

    return wxString::FromUTF8( out.c_str() );
}


namespace CABLY_CLOUD_DETAIL
{
/// Days since 1970-01-01 for a proleptic Gregorian civil date (Howard Hinnant's
/// days_from_civil); keeps the parser independent of the local time zone and DST.
inline int64_t DaysFromCivil( int aYear, unsigned aMonth, unsigned aDay )
{
    aYear -= aMonth <= 2;
    const int64_t  era = ( aYear >= 0 ? aYear : aYear - 399 ) / 400;
    const unsigned yoe = (unsigned) ( aYear - era * 400 );
    const unsigned doy = ( 153 * ( aMonth + ( aMonth > 2 ? -3 : 9 ) ) + 2 ) / 5 + aDay - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t) doe - 719468;
}


inline bool ReadDigits( const std::string& aText, size_t& aPos, size_t aCount, int& aOut )
{
    if( aPos + aCount > aText.size() )
        return false;

    int value = 0;

    for( size_t i = 0; i < aCount; ++i )
    {
        char c = aText[aPos + i];

        if( c < '0' || c > '9' )
            return false;

        value = value * 10 + ( c - '0' );
    }

    aPos += aCount;
    aOut = value;
    return true;
}
} // namespace CABLY_CLOUD_DETAIL


/**
 * Parse a Supabase/Postgres ISO-8601 timestamp ("2026-09-03T14:05:07.123456+00:00",
 * "...Z", "...+HH:MM", "...+HHMM", 'T' or space separator, fraction optional, no zone
 * meaning UTC) into @a aOut as an instant.  Read it back with wxDateTime::UTC or
 * wxDateTime::Local as needed.  Returns false (aOut untouched) on anything malformed.
 */
inline bool CablyParseIsoUtc( const wxString& aIso, wxDateTime& aOut )
{
    using namespace CABLY_CLOUD_DETAIL;

    const std::string s( aIso.ToUTF8() );
    size_t            p = 0;
    int               year, month, day, hour, minute, second;

    if( !ReadDigits( s, p, 4, year ) || p >= s.size() || s[p++] != '-' )
        return false;

    if( !ReadDigits( s, p, 2, month ) || p >= s.size() || s[p++] != '-' )
        return false;

    if( !ReadDigits( s, p, 2, day ) || p >= s.size() || ( s[p] != 'T' && s[p] != 't' && s[p] != ' ' ) )
        return false;

    ++p;

    if( !ReadDigits( s, p, 2, hour ) || p >= s.size() || s[p++] != ':' )
        return false;

    if( !ReadDigits( s, p, 2, minute ) || p >= s.size() || s[p++] != ':' )
        return false;

    if( !ReadDigits( s, p, 2, second ) )
        return false;

    if( month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60 )
        return false;

    // fractional seconds: ignored
    if( p < s.size() && ( s[p] == '.' || s[p] == ',' ) )
    {
        ++p;
        size_t start = p;

        while( p < s.size() && s[p] >= '0' && s[p] <= '9' )
            ++p;

        if( p == start )
            return false;
    }

    int offsetMinutes = 0;

    if( p < s.size() )
    {
        char z = s[p];

        if( z == 'Z' || z == 'z' )
        {
            ++p;
        }
        else if( z == '+' || z == '-' )
        {
            ++p;
            int oh, om = 0;

            if( !ReadDigits( s, p, 2, oh ) )
                return false;

            if( p < s.size() && s[p] == ':' )
                ++p;

            if( p < s.size() && !ReadDigits( s, p, 2, om ) )
                return false;

            offsetMinutes = ( oh * 60 + om ) * ( z == '-' ? -1 : 1 );
        }
        else
        {
            return false;
        }
    }

    if( p != s.size() )
        return false;

    const int64_t days = DaysFromCivil( year, (unsigned) month, (unsigned) day );
    const int64_t epoch = days * 86400 + hour * 3600 + minute * 60 + second - offsetMinutes * 60;

    aOut = wxDateTime( (time_t) epoch );
    return aOut.IsValid();
}


/**
 * The "updated ..." wording of a projects-list row, relative to @a aNow.
 *
 * "just now" (< 1 min, also for a slightly future stamp: clock skew), "N min ago",
 * "N h ago", "yesterday", "N days ago" (< 7 days), otherwise the date "3 Sep 2026"
 * rendered in @a aTz.  An invalid @a aUpdated gives an empty string.
 */
inline wxString CablyFormatUpdated( const wxDateTime& aUpdated, const wxDateTime& aNow,
                                    const wxDateTime::TimeZone& aTz = wxDateTime::Local )
{
    if( !aUpdated.IsValid() || !aNow.IsValid() )
        return wxEmptyString;

    const long long diff = (long long) aNow.GetTicks() - (long long) aUpdated.GetTicks();

    if( diff < 60 )
        return wxS( "just now" );

    if( diff < 3600 )
        return wxString::Format( wxS( "%lld min ago" ), diff / 60 );

    if( diff < 86400 )
        return wxString::Format( wxS( "%lld h ago" ), diff / 3600 );

    if( diff < 2 * 86400 )
        return wxS( "yesterday" );

    if( diff < 7 * 86400 )
        return wxString::Format( wxS( "%lld days ago" ), diff / 86400 );

    // %e is space-padded; format the day by hand so "3 Sep 2026" has no leading blank.
    wxDateTime::Tm tm = aUpdated.GetTm( aTz );
    return wxString::Format( wxS( "%d %s %d" ), (int) tm.mday,
                             wxDateTime::GetEnglishMonthName( tm.mon, wxDateTime::Name_Abbr ),
                             (int) tm.year );
}


/// RFC 3986 percent-encoding of UTF-8 text: unreserved characters kept, every other
/// byte becomes %XX (upper-case hex); a space is %20, never '+'.
inline wxString CablyPercentEncode( const std::string& aUtf8 )
{
    static const char* hex = "0123456789ABCDEF";
    std::string        out;
    out.reserve( aUtf8.size() * 3 );

    for( unsigned char c : aUtf8 )
    {
        const bool unreserved = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' )
                                || ( c >= '0' && c <= '9' ) || c == '-' || c == '_' || c == '.'
                                || c == '~';

        if( unreserved )
        {
            out += (char) c;
        }
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }

    return wxString::FromAscii( out.c_str() );
}


/**
 * Where "Generate" sends the prompt: <app>?prompt=<percent-encoded prompt>, or plain
 * <app> when the (trimmed) prompt is empty.  @a aAppUrl is the web app (CABLY_APP_URL,
 * https://cably.dev/app), with or without a trailing slash.
 */
inline wxString CablyGenerateUrl( const wxString& aAppUrl, const wxString& aPrompt )
{
    wxString url = aAppUrl;

    while( url.EndsWith( wxS( "/" ) ) )
        url.RemoveLast();

    wxString prompt = aPrompt;
    prompt.Trim( true ).Trim( false );

    if( prompt.IsEmpty() )
        return url;

    return url + wxS( "?prompt=" ) + CablyPercentEncode( std::string( prompt.ToUTF8() ) );
}

#endif // CABLY_HOME_CLOUD_H

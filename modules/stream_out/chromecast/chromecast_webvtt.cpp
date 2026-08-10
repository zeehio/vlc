/*****************************************************************************
 * chromecast_webvtt.cpp: SRT/WebVTT helpers for the Chromecast sidecar track
 *****************************************************************************
 * Copyright © 2015-2016 VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "chromecast_webvtt.h"

#include <cstdio>
#include <cstring>
#include <vector>

bool chromecast_HasExtension( const char *psz_uri, const char *psz_ext )
{
    size_t i_uri = strlen( psz_uri );
    size_t i_ext = strlen( psz_ext );
    if( i_uri < i_ext + 1 || psz_uri[i_uri - i_ext - 1] != '.' )
        return false;
    for( size_t i = 0; i < i_ext; ++i )
    {
        char a = psz_uri[i_uri - i_ext + i];
        char b = psz_ext[i];
        if( a >= 'A' && a <= 'Z' ) a += 'a' - 'A';
        if( b >= 'A' && b <= 'Z' ) b += 'a' - 'A';
        if( a != b )
            return false;
    }
    return true;
}

bool chromecast_ParseVttTimestamp( const std::string &s, int64_t *out_ms )
{
    unsigned h, m, sec, ms;
    if( sscanf( s.c_str(), "%u:%u:%u%*[,.]%u", &h, &m, &sec, &ms ) == 4 )
    {
        *out_ms = ( (int64_t)h * 3600 + m * 60 + sec ) * 1000 + ms;
        return true;
    }
    if( sscanf( s.c_str(), "%u:%u%*[,.]%u", &m, &sec, &ms ) == 3 )
    {
        *out_ms = ( (int64_t)m * 60 + sec ) * 1000 + ms;
        return true;
    }
    return false;
}

std::string chromecast_FormatVttTimestamp( int64_t ms )
{
    if( ms < 0 )
        ms = 0;
    unsigned msec = (unsigned)( ms % 1000 ); ms /= 1000;
    unsigned sec  = (unsigned)( ms % 60 );   ms /= 60;
    unsigned min  = (unsigned)( ms % 60 );   ms /= 60;
    unsigned hour = (unsigned)ms;
    char buf[32];
    snprintf( buf, sizeof(buf), "%02u:%02u:%02u.%03u", hour, min, sec, msec );
    return std::string( buf );
}

std::string chromecast_ConvertSubtitleToWebVTT( const std::string &content, bool is_srt,
                                                 int64_t i_offset_ms )
{
    std::string out( "WEBVTT\n\n" );

    size_t start = 0;
    /* Strip a leading UTF-8 BOM if present */
    if( content.compare( 0, 3, "\xEF\xBB\xBF" ) == 0 )
        start = 3;

    std::vector<std::string> lines;
    {
        size_t pos = start;
        while( pos <= content.size() )
        {
            size_t eol = content.find( '\n', pos );
            std::string line = content.substr( pos, eol == std::string::npos ? std::string::npos : eol - pos );
            if( !line.empty() && line.back() == '\r' )
                line.pop_back();
            lines.push_back( line );
            if( eol == std::string::npos )
                break;
            pos = eol + 1;
        }
    }

    size_t i = 0;
    /* Skip a leading WEBVTT signature line, and any header metadata up to
     * the first blank line, if present. */
    if( !is_srt && i < lines.size() && lines[i].compare( 0, 6, "WEBVTT" ) == 0 )
    {
        while( i < lines.size() && !lines[i].empty() )
            ++i;
    }

    while( i < lines.size() )
    {
        while( i < lines.size() && lines[i].empty() )
            ++i;
        if( i >= lines.size() )
            break;

        size_t block_start = i;
        /* An optional numeric/identifier line precedes the timing line */
        size_t timing_line = i;
        if( lines[timing_line].find( "-->" ) == std::string::npos
         && timing_line + 1 < lines.size() )
            timing_line = i + 1;

        bool have_timing = false;
        int64_t cue_start_ms = 0, cue_end_ms = 0;
        size_t arrow = std::string::npos;
        if( timing_line < lines.size() )
        {
            arrow = lines[timing_line].find( "-->" );
            if( arrow != std::string::npos )
            {
                std::string ts_start = lines[timing_line].substr( 0, arrow );
                std::string ts_end_raw = lines[timing_line].substr( arrow + 3 );
                /* Only the first token after "-->" is the timestamp; any
                 * trailing cue settings (line:, position:, ...) are not
                 * preserved. */
                size_t e0 = ts_end_raw.find_first_not_of( " \t" );
                std::string ts_end = e0 == std::string::npos ? std::string()
                                    : ts_end_raw.substr( e0 );
                size_t e1 = ts_end.find_first_of( " \t" );
                if( e1 != std::string::npos )
                    ts_end = ts_end.substr( 0, e1 );

                have_timing = chromecast_ParseVttTimestamp( ts_start, &cue_start_ms )
                           && chromecast_ParseVttTimestamp( ts_end, &cue_end_ms );
            }
        }

        size_t block_end = ( arrow != std::string::npos ) ? timing_line + 1 : block_start;
        while( block_end < lines.size() && !lines[block_end].empty() )
            ++block_end;

        if( have_timing )
        {
            cue_start_ms -= i_offset_ms;
            cue_end_ms -= i_offset_ms;

            /* Drop cues that belong entirely to a part of the file that
             * isn't part of the current segment. */
            if( cue_end_ms > 0 )
            {
                for( size_t j = block_start; j < timing_line; ++j )
                {
                    out += lines[j];
                    out += '\n';
                }
                out += chromecast_FormatVttTimestamp( cue_start_ms );
                out += " --> ";
                out += chromecast_FormatVttTimestamp( cue_end_ms );
                out += '\n';
                for( size_t j = timing_line + 1; j < block_end; ++j )
                {
                    out += lines[j];
                    out += '\n';
                }
                out += '\n';
            }
        }
        else
        {
            /* Not a cue we understand (e.g. a WebVTT NOTE block): pass it
             * through unshifted rather than losing it silently. */
            for( size_t j = block_start; j < block_end; ++j )
            {
                out += lines[j];
                out += '\n';
            }
            out += '\n';
        }

        i = block_end;
    }

    return out;
}

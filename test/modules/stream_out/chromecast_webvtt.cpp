/*****************************************************************************
 * chromecast_webvtt.cpp: Chromecast sidecar WebVTT helpers unit testing
 *****************************************************************************
 * Copyright © 2026 VideoLAN
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

#undef NDEBUG

#include <cassert>

#include "../../../modules/stream_out/chromecast/chromecast_webvtt.h"

static void test_HasExtension()
{
    assert( chromecast_HasExtension( "movie.srt", "srt" ) );
    assert( chromecast_HasExtension( "movie.SRT", "srt" ) );
    assert( chromecast_HasExtension( "movie.Srt", "srt" ) );
    assert( chromecast_HasExtension( "/path/to/movie.webvtt", "webvtt" ) );

    assert( !chromecast_HasExtension( "movie.ass", "srt" ) );
    assert( !chromecast_HasExtension( "movie.srtx", "srt" ) );
    assert( !chromecast_HasExtension( "srt", "srt" ) ); // no '.' before ext
    assert( !chromecast_HasExtension( "", "srt" ) );
}

static void test_ParseVttTimestamp()
{
    int64_t ms;

    assert( chromecast_ParseVttTimestamp( "00:00:00,000", &ms ) && ms == 0 );
    assert( chromecast_ParseVttTimestamp( "00:00:00.000", &ms ) && ms == 0 );
    assert( chromecast_ParseVttTimestamp( "01:02:03,456", &ms )
         && ms == ( (int64_t)1 * 3600 + 2 * 60 + 3 ) * 1000 + 456 );
    assert( chromecast_ParseVttTimestamp( "01:02:03.456", &ms )
         && ms == ( (int64_t)1 * 3600 + 2 * 60 + 3 ) * 1000 + 456 );
    /* WebVTT allows omitting the hours component */
    assert( chromecast_ParseVttTimestamp( "02:03.456", &ms )
         && ms == ( (int64_t)2 * 60 + 3 ) * 1000 + 456 );

    assert( !chromecast_ParseVttTimestamp( "not a timestamp", &ms ) );
    assert( !chromecast_ParseVttTimestamp( "", &ms ) );
    assert( !chromecast_ParseVttTimestamp( "00:00", &ms ) );
}

static void test_FormatVttTimestamp()
{
    assert( chromecast_FormatVttTimestamp( 0 ) == "00:00:00.000" );
    assert( chromecast_FormatVttTimestamp( 456 ) == "00:00:00.456" );
    assert( chromecast_FormatVttTimestamp( ( (int64_t)1 * 3600 + 2 * 60 + 3 ) * 1000 + 456 )
            == "01:02:03.456" );
    /* Negative values (a cue starting before the current segment) clamp to 0 */
    assert( chromecast_FormatVttTimestamp( -1000 ) == "00:00:00.000" );

    /* Round-trips through ParseVttTimestamp */
    int64_t ms;
    assert( chromecast_ParseVttTimestamp( chromecast_FormatVttTimestamp( 3723456 ), &ms ) );
    assert( ms == 3723456 );
}

static void test_ConvertSubtitleToWebVTT_BasicSrt()
{
    const std::string srt =
        "1\n"
        "00:00:01,000 --> 00:00:04,000\n"
        "Hello world\n"
        "\n"
        "2\n"
        "00:00:05,500 --> 00:00:07,000\n"
        "Second line\n";

    const std::string expected =
        "WEBVTT\n\n"
        "1\n"
        "00:00:01.000 --> 00:00:04.000\n"
        "Hello world\n\n"
        "2\n"
        "00:00:05.500 --> 00:00:07.000\n"
        "Second line\n\n";

    assert( chromecast_ConvertSubtitleToWebVTT( srt, true, 0 ) == expected );
}

static void test_ConvertSubtitleToWebVTT_OffsetDropsPastCues()
{
    /* Casting starts (or a seek lands) at 00:00:05.000 into the file: cues
     * that end before that point belong to a part of the file that isn't
     * part of the current segment and must be dropped, not just shifted
     * into negative timestamps. */
    const std::string srt =
        "1\n"
        "00:00:01,000 --> 00:00:04,000\n"
        "Dropped: entirely before the segment start\n"
        "\n"
        "2\n"
        "00:00:04,500 --> 00:00:06,000\n"
        "Clamped: starts before, ends after\n"
        "\n"
        "3\n"
        "00:00:10,000 --> 00:00:12,000\n"
        "Kept: entirely after\n";

    const std::string webvtt = chromecast_ConvertSubtitleToWebVTT( srt, true, 5000 );

    assert( webvtt.find( "Dropped" ) == std::string::npos );
    assert( webvtt.find( "00:00:00.000 --> 00:00:01.000\nClamped" ) != std::string::npos );
    assert( webvtt.find( "00:00:05.000 --> 00:00:07.000\nKept" ) != std::string::npos );
}

static void test_ConvertSubtitleToWebVTT_PassthroughVtt()
{
    const std::string vtt =
        "WEBVTT\n"
        "\n"
        "00:00:01.000 --> 00:00:02.000\n"
        "Already WebVTT\n";

    const std::string webvtt = chromecast_ConvertSubtitleToWebVTT( vtt, false, 0 );
    assert( webvtt.find( "WEBVTT\n\n" ) == 0 );
    assert( webvtt.find( "00:00:01.000 --> 00:00:02.000\nAlready WebVTT\n" )
            != std::string::npos );
}

static void test_ConvertSubtitleToWebVTT_StripsBOM()
{
    const std::string srt =
        "\xEF\xBB\xBF" "1\n00:00:01,000 --> 00:00:02,000\nHi\n";

    const std::string webvtt = chromecast_ConvertSubtitleToWebVTT( srt, true, 0 );
    /* The BOM must not leak into the output, in particular not right after
     * the mandatory "WEBVTT" signature. */
    assert( webvtt.compare( 0, 8, "WEBVTT\n\n" ) == 0 );
}

static void test_ConvertSubtitleToWebVTT_MalformedCuePassthrough()
{
    /* A block without a "-->" timing line (e.g. a WebVTT NOTE, or anything
     * this parser doesn't understand) must be preserved rather than
     * silently dropped. */
    const std::string vtt =
        "WEBVTT\n"
        "\n"
        "NOTE this is a comment\n"
        "\n"
        "00:00:01.000 --> 00:00:02.000\n"
        "Real cue\n";

    const std::string webvtt = chromecast_ConvertSubtitleToWebVTT( vtt, false, 0 );
    assert( webvtt.find( "NOTE this is a comment" ) != std::string::npos );
    assert( webvtt.find( "Real cue" ) != std::string::npos );
}

int main(void)
{
    test_HasExtension();
    test_ParseVttTimestamp();
    test_FormatVttTimestamp();
    test_ConvertSubtitleToWebVTT_BasicSrt();
    test_ConvertSubtitleToWebVTT_OffsetDropsPastCues();
    test_ConvertSubtitleToWebVTT_PassthroughVtt();
    test_ConvertSubtitleToWebVTT_StripsBOM();
    test_ConvertSubtitleToWebVTT_MalformedCuePassthrough();
    return 0;
}

/*****************************************************************************
 * chromecast_webvtt.cpp: Chromecast sidecar WebVTT conversion unit testing
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#undef NDEBUG

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <vlc_common.h>
#include <vlc_tick.h>

#include "../../../modules/stream_out/chromecast/chromecast_webvtt.h"
#include "../../libvlc/test.h"
#include "../lib/libvlc_internal.h"

/* chromecast_webvtt.cpp's msg_Dbg/msg_Warn calls need this symbol, normally
 * provided by vlc_module_begin() in the real plugin (chromecast_demux.cpp);
 * linked directly into this test binary instead of into that plugin, it
 * needs its own definition. */
extern "C" const char vlc_module_name[] = "test_modules_stream_out_chromecast_webvtt";

/* Writes content to a fresh temp file and returns a file:// URI to it
 * (heap-allocated, caller frees). chromecast_ConvertSubtitleFileToWebVTT
 * takes a URI, not a stream, since in production it always opens a real
 * external slave file - so the test goes through the same real file I/O
 * path rather than mocking it away. */
static char *WriteFixture( const char *content )
{
    char path[] = "/tmp/cc_webvtt_test_XXXXXX";
    int fd = mkstemp( path );
    assert( fd >= 0 );
    size_t len = strlen( content );
    ssize_t written = write( fd, content, len );
    assert( written == (ssize_t) len );
    close( fd );

    char *uri;
    int r = asprintf( &uri, "file://%s", path );
    assert( r >= 0 );
    return uri;
}

static void RemoveFixture( const char *uri )
{
    assert( strncmp( uri, "file://", 7 ) == 0 );
    unlink( uri + 7 );
}

#define SRT_FIXTURE \
    "1\n" \
    "00:00:01,000 --> 00:00:04,000\n" \
    "Hello world\n" \
    "\n" \
    "2\n" \
    "00:00:05,500 --> 00:00:07,000\n" \
    "Second line\n" \
    "with two rows\n"

#define MICRODVD_FIXTURE \
    "{25}{100}Frame-based cue one\n" \
    "{150}{200}Frame-based cue two\n"

/* SSA/ASS is only ever decoded through libass (a bitmap renderer), never a
 * text decoder, so it can never yield WebVTT cues through this pipeline -
 * this is a real, permanent limitation (see chromecast_webvtt.h), not
 * something a future fix removes, and is asserted here so a regression
 * that silently starts "succeeding" with garbage is caught too. */
#define ASS_FIXTURE \
    "[Script Info]\n" \
    "Title: Test\n" \
    "ScriptType: v4.00+\n" \
    "PlayResX: 1280\n" \
    "PlayResY: 720\n" \
    "\n" \
    "[V4+ Styles]\n" \
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n" \
    "Style: Default,Arial,40,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1\n" \
    "\n" \
    "[Events]\n" \
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n" \
    "Dialogue: 0,0:00:02.00,0:00:04.50,Default,,0,0,0,,Plain ASS cue\n"

static void test_BasicSrt( vlc_object_t *obj )
{
    char *uri = WriteFixture( SRT_FIXTURE );

    char *webvtt = chromecast_ConvertSubtitleFileToWebVTT( obj, uri, 0 );
    assert( webvtt != NULL );
    std::string out( webvtt );
    free( webvtt );

    assert( out.compare( 0, 8, "WEBVTT\n\n" ) == 0 );
    assert( out.find( "00:00:01.000 --> 00:00:04.000" ) != std::string::npos );
    assert( out.find( "Hello world" ) != std::string::npos );
    assert( out.find( "00:00:05.500 --> 00:00:07.000" ) != std::string::npos );
    assert( out.find( "Second line" ) != std::string::npos );
    /* multi-line cue text must survive */
    assert( out.find( "with two rows" ) != std::string::npos );

    RemoveFixture( uri );
    free( uri );
}

static void test_SegmentOffsetDropsAndShiftsCues( vlc_object_t *obj )
{
    char *uri = WriteFixture( SRT_FIXTURE );

    /* Casting starts (or a seek lands) 5s into the file: the first cue
     * (1s-4s) is entirely before that and must be dropped; the second
     * (5.5s-7s) must be shifted to start 0.5s into the new segment. */
    char *webvtt = chromecast_ConvertSubtitleFileToWebVTT( obj, uri, VLC_TICK_FROM_SEC( 5 ) );
    assert( webvtt != NULL );
    std::string out( webvtt );
    free( webvtt );

    assert( out.find( "Hello world" ) == std::string::npos );
    assert( out.find( "00:00:00.500 --> 00:00:02.000" ) != std::string::npos );
    assert( out.find( "Second line" ) != std::string::npos );

    RemoveFixture( uri );
    free( uri );
}

static void test_OffsetPastAllCuesYieldsNoTrack( vlc_object_t *obj )
{
    char *uri = WriteFixture( SRT_FIXTURE );

    /* Every cue ends before a segment starting at 1 minute in: nothing to
     * show for this segment at all, same as no subtitle slave. */
    char *webvtt = chromecast_ConvertSubtitleFileToWebVTT( obj, uri, VLC_TICK_FROM_SEC( 60 ) );
    assert( webvtt == NULL );

    RemoveFixture( uri );
    free( uri );
}

static void test_MicroDVDGenerality( vlc_object_t *obj )
{
    /* A format with nothing in common with SRT (frame-number timing, no
     * "-->" syntax at all): proves this isn't SRT-specific parsing. */
    char *uri = WriteFixture( MICRODVD_FIXTURE );

    char *webvtt = chromecast_ConvertSubtitleFileToWebVTT( obj, uri, 0 );
    assert( webvtt != NULL );
    std::string out( webvtt );
    free( webvtt );

    assert( out.find( "00:00:01.000 --> 00:00:04.000" ) != std::string::npos );
    assert( out.find( "Frame-based cue one" ) != std::string::npos );
    assert( out.find( "00:00:06.000 --> 00:00:08.000" ) != std::string::npos );
    assert( out.find( "Frame-based cue two" ) != std::string::npos );

    RemoveFixture( uri );
    free( uri );
}

static void test_StyledAssYieldsNoTrack( vlc_object_t *obj )
{
    char *uri = WriteFixture( ASS_FIXTURE );

    char *webvtt = chromecast_ConvertSubtitleFileToWebVTT( obj, uri, 0 );
    assert( webvtt == NULL );

    RemoveFixture( uri );
    free( uri );
}

static void test_MissingFileYieldsNoTrack( vlc_object_t *obj )
{
    char *webvtt = chromecast_ConvertSubtitleFileToWebVTT(
        obj, "file:///nonexistent/path/that/should/not/exist.srt", 0 );
    assert( webvtt == NULL );
}

int main(void)
{
    test_init();

    const char *const args[] = { "-vvv" };
    libvlc_instance_t *vlc = libvlc_new( ARRAY_SIZE( args ), args );
    assert( vlc != NULL );
    vlc_object_t *obj = VLC_OBJECT( vlc->p_libvlc_int );

    test_BasicSrt( obj );
    test_SegmentOffsetDropsAndShiftsCues( obj );
    test_OffsetPastAllCuesYieldsNoTrack( obj );
    test_MicroDVDGenerality( obj );
    test_StyledAssYieldsNoTrack( obj );
    test_MissingFileYieldsNoTrack( obj );

    libvlc_release( vlc );
    return 0;
}

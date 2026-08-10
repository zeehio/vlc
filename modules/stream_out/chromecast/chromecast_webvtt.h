/*****************************************************************************
 * chromecast_webvtt.h: convert a subtitle file to WebVTT for the Chromecast
 * sidecar track, reusing VLC's own subtitle demux/decode/encode/mux chain
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

#ifndef VLC_CHROMECAST_WEBVTT_H
#define VLC_CHROMECAST_WEBVTT_H

#include <vlc_common.h>
#include <vlc_tick.h>

/**
 * Converts an external subtitle file to a WebVTT document, for use as a
 * Chromecast sidecar text track.
 *
 * This reuses VLC's own subtitle pipeline end to end - the generic
 * "subtitle" demuxer (SRT, MicroDVD, SAMI, SubViewer, and more), the
 * matching spu decoder, and the "webvtt" spu encoder/muxer - rather than
 * a format-specific parser, so any text-based subtitle format VLC already
 * understands is supported here for free. Formats that only decode to a
 * bitmap/rendered representation (e.g. styled ASS/SSA via libass, or PGS,
 * DVB, VobSub) cannot become WebVTT cues and correctly yield no output,
 * same as if the slave had never been offered.
 *
 * \param p_parent object used as the demux/decoder/encoder's parent and
 *                 for logging.
 * \param psz_uri  URI of the external subtitle file.
 * \param i_offset the source-file position (as returned by DEMUX_GET_TIME
 *                 on the main content's demux) that the current Cast
 *                 segment starts at: cues are shifted by this amount so
 *                 they line up with the receiver's own clock (which
 *                 restarts near 0 for every segment - see
 *                 chromecast_demux.cpp for why), and any cue that ends up
 *                 entirely before the segment start is dropped. Pass 0 to
 *                 leave cues as they are in the source file.
 * \return a heap-allocated, NUL-terminated WebVTT document (ownership
 *         transferred to the caller, which must free() it), or NULL if the
 *         file couldn't be parsed as a subtitle, or none of its cues could
 *         be turned into WebVTT text.
 */
char *chromecast_ConvertSubtitleFileToWebVTT( vlc_object_t *p_parent,
                                              const char *psz_uri,
                                              vlc_tick_t i_offset );

#endif // VLC_CHROMECAST_WEBVTT_H

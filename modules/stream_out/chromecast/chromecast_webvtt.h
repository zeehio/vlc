/*****************************************************************************
 * chromecast_webvtt.h: SRT/WebVTT helpers for the Chromecast sidecar track
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

#include <cstdint>
#include <string>

/**
 * \return true if psz_uri ends with '.' + psz_ext (case-insensitive)
 */
bool chromecast_HasExtension( const char *psz_uri, const char *psz_ext );

/**
 * Parses a "[HH:]MM:SS[.,]mmm" timestamp (SRT uses ',' as the decimal
 * separator, WebVTT uses '.'; both are accepted). Returns false if the
 * string doesn't match either form.
 */
bool chromecast_ParseVttTimestamp( const std::string &s, int64_t *out_ms );

/**
 * Formats a millisecond count as a WebVTT "HH:MM:SS.mmm" timestamp.
 * Negative values are clamped to 0.
 */
std::string chromecast_FormatVttTimestamp( int64_t ms );

/**
 * SRT and WebVTT share the same cue structure (an optional numeric
 * identifier line, a "start --> end" timing line, then one or more text
 * lines, cues separated by a blank line).
 *
 * Cue timestamps in the source file are always relative to the file's own
 * 0:00. The Chromecast receiver's own playback clock however restarts near
 * 0 for every "segment" it is fed: at the initial LOAD, and again after
 * every seek (the Cast protocol has no concept of seeking within VLC's
 * live-restream, ES_OUT_RESET_PCR just tells the encoder to start a new
 * timestamp epoch and the receiver's clock follows that). i_offset_ms is
 * the source-file position (in ms) the current segment starts at: it is
 * subtracted from every cue so cues line up with the receiver's clock, and
 * any cue that ends before that point (i.e. it belongs to a part of the
 * file no longer being sent) is dropped.
 */
std::string chromecast_ConvertSubtitleToWebVTT( const std::string &content, bool is_srt,
                                                 int64_t i_offset_ms );

#endif // VLC_CHROMECAST_WEBVTT_H

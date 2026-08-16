/*****************************************************************************
 * chromecast_common.h: Chromecast common code between modules for vlc
 *****************************************************************************
 * Copyright © 2015-2016 VideoLAN
 *
 * Authors: Adrien Maglo <magsoft@videolan.org>
 *          Jean-Baptiste Kempf <jb@videolan.org>
 *          Steve Lhomme <robux4@videolabs.io>
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

#ifndef VLC_CHROMECAST_COMMON_H
#define VLC_CHROMECAST_COMMON_H

#include <vlc_input.h>

# ifdef __cplusplus
extern "C" {
# endif

#define CC_SHARED_VAR_NAME "cc_sout"

#define CC_PACE_ERR        (-2)
#define CC_PACE_ERR_RETRY  (-1)
#define CC_PACE_OK          (0)
#define CC_PACE_OK_WAIT     (1)
#define CC_PACE_OK_ENDED    (2)

enum cc_input_event
{
    CC_INPUT_EVENT_EOF,
    CC_INPUT_EVENT_RETRY,
};

union cc_input_arg
{
    bool eof;
};

typedef void (*on_input_event_itf)( void *data, enum cc_input_event, union cc_input_arg );

typedef void (*on_paused_changed_itf)( void *data, bool );

typedef struct
{
    void *p_opaque;

    void (*pf_set_demux_enabled)(void *, bool enabled, on_paused_changed_itf, void *);

    vlc_tick_t (*pf_get_time)(void*);

    int (*pf_pace)(void*);

    void (*pf_send_input_event)(void*, enum cc_input_event, union cc_input_arg);

    void (*pf_set_pause_state)(void*, bool paused);

    void (*pf_set_meta)(void*, vlc_meta_t *p_meta);

    void (*pf_set_input_length)(void*, vlc_tick_t length);

    /**
     * Report what the demux filter knows about the original source: its
     * URL, the name of the demuxer that was actually selected for it (not
     * the caller's requested hint), and whether it is seekable. Used to
     * decide whether the source can be handed to the receiver as-is
     * instead of going through the transcode/live-restream chain.
     *
     * \param psz_url NULL or empty clears the previously reported source.
     * \param psz_demux NULL if unknown.
     */
    void (*pf_set_source_info)(void *, const char *psz_url, const char *psz_demux,
                               bool b_can_seek);

    /**
     * Whether this session serves the original source bytes directly
     * (Range-capable, over the URL pf_set_source_info reported) instead of
     * the transcode/live-restream chain, because the source is already a
     * Chromecast-compatible, finite, seekable file. Set once the sout has
     * seen the track set and decided; read by the demux filter to decide
     * whether local playback position/seeking should defer to the
     * receiver instead of the local demux.
     */
    bool (*pf_is_source_direct)(void *);

    /**
     * Direct-serve only: ask the receiver to seek to the given absolute position
     * in the source it is reading directly, instead of seeking (and
     * re-casting from) the local demux. Returns false if the request could
     * not be sent (e.g. no media session yet).
     */
    bool (*pf_seek)(void *, vlc_tick_t time);

    /**
     * Whether cast.cpp has settled the direct-serve eligibility decision for the
     * currently-known track set (whichever way it went). The subtitle scan
     * waits for this before its first run, instead of generating a
     * possibly-wrong provisional sidecar (using the wrong offset
     * convention) and correcting it later, which risks the receiver
     * rendering cues from that first, wrong fetch with no guarantee a
     * reload actually clears them.
     */
    bool (*pf_is_eligibility_decided)(void *);

    /**
     * Direct-serve only: report the position local playback was already at when
     * this cast session started (e.g. resuming a video, or attaching a
     * renderer to a playback already in progress), so the initial LOAD can
     * ask the receiver to start there directly instead of at the
     * beginning of the source.
     */
    void (*pf_set_start_time)(void *, vlc_tick_t time);

    /**
     * Report how many audio tracks the master source has (embedded
     * ones - read from the input item's es_vec, not slaves). Direct-serve
     * hands the receiver the raw container bytes as-is with no way to
     * tell it which embedded audio track to honour: the Default/Styled
     * Media Receiver Cast apps only support selecting text tracks via
     * the LOAD request's activeTrackIds, not audio or video (see
     * https://developers.google.com/cast/docs/web_sender/advanced), so
     * with more than one audio track the receiver would demux the file
     * itself and pick whichever one it considers the default, which may
     * not match what is actually selected locally. Used to fall back to
     * the transcode/live-restream chain instead, which only ever
     * encodes the one track actually selected.
     */
    void (*pf_set_audio_track_count)(void *, unsigned count);

    /**
     * Hand over a WebVTT subtitle track to be served as a sidecar HTTP
     * resource and rendered by the Chromecast receiver itself, instead of
     * being burned into the video.
     *
     * \param psz_webvtt heap-allocated, NUL-terminated WebVTT document.
     *                   Ownership is transferred to the callee, which must
     *                   free() it. NULL clears any previously set subtitle.
     */
    void (*pf_set_subtitle)(void *, char *psz_webvtt);

    /**
     * Ask for the current media to be reloaded on the receiver (a fresh
     * LOAD message reusing the same content). The Cast protocol only
     * fetches sidecar text tracks once, at LOAD time, and the media here
     * is declared as a LIVE stream (no built-in seeking), so this is the
     * only way to make the receiver pick up a track that was just updated
     * via pf_set_subtitle after the session already started.
     */
    void (*pf_reload)(void *);

    /**
     * Report that an external subtitle slave's own text track was just
     * explicitly selected. The input core routes DEMUX_SET_ES /
     * DEMUX_SET_ES_LIST only to the specific source that owns the
     * newly-selected track (see ControlSetEsList/INPUT_CONTROL_SET_ES in
     * src/input/input.c), so a slave's own demux filter instance seeing
     * one of these calls is a precise "my track was just selected"
     * signal. Reported here so the master's subtitle sidecar scan (the
     * only instance with the local playback position the live-restream
     * offset needs) can honour it instead of always defaulting to the
     * first external slave found.
     *
     * A non-empty URI also supersedes any embedded track previously
     * reported via pf_set_selected_subtitle_track below - only one
     * subtitle can really be showing at a time.
     *
     * \param psz_uri the slave's own URI ("" clears the override,
     *                falling back to the first external slave found).
     */
    void (*pf_set_selected_subtitle_uri)(void *, const char *psz_uri);

    /**
     * Test-and-clear: true (once) exactly when pf_set_selected_subtitle_uri
     * or pf_set_selected_subtitle_track was called since the last call to
     * this function, meaning the sidecar scan should be re-run to honour
     * the new selection.
     */
    bool (*pf_consume_subtitle_selection_change)(void *);

    /**
     * Return the URI last reported via pf_set_selected_subtitle_uri
     * (heap-allocated, caller must free()); NULL if none has been
     * reported yet.
     */
    char *(*pf_get_selected_subtitle_uri)(void *);

    /**
     * Report that an embedded (in-container) subtitle track on the
     * master source was just explicitly selected, so the sidecar scan
     * can hand its content over as a WebVTT sidecar the same way it does
     * for an external slave (see chromecast_webvtt.h's
     * chromecast_ConvertEmbeddedTrackToWebVTT). Unlike an external
     * slave, an embedded track has no URI of its own: it is identified
     * by the id its own demuxer gave it (es_format_t::i_id), which a
     * second, independent pass over the same source matches against to
     * single it out. Reuses pf_consume_subtitle_selection_change - from
     * the sidecar scan's point of view, "an external slave got selected"
     * and "an embedded track got selected" both just mean "reconsider
     * what to show".
     *
     * A track id >= 0 also supersedes any external slave URI previously
     * reported via pf_set_selected_subtitle_uri above - only one
     * subtitle can really be showing at a time.
     *
     * \param i_track_id the container-assigned track id, or -1 to clear
     *                    the override (falls back to whatever else
     *                    applies, e.g. an external slave).
     */
    void (*pf_set_selected_subtitle_track)(void *, int i_track_id);

    /**
     * Return the track id last reported via
     * pf_set_selected_subtitle_track, or -1 if none has been reported
     * yet (or it was cleared).
     */
    int (*pf_get_selected_subtitle_track)(void *);

} chromecast_common;

# ifdef __cplusplus
}
# endif

#endif // VLC_CHROMECAST_COMMON_H


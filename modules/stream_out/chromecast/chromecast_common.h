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
     * Report the total duration of the media being cast, as known by the
     * local demux (VLC_TICK_INVALID/<= 0 if unknown, e.g. a live capture
     * source such as a webcam). A known duration lets the LOAD message
     * declare the media as seekable (streamType "BUFFERED" with a real
     * duration) instead of "LIVE".
     */
    void (*pf_set_input_length)(void *, vlc_tick_t length);

    /**
     * Report the original source of the media being cast, as known by the
     * local demux: its URL (so a direct-serve HTTP endpoint can open the
     * same resource) and the name of the demux module that recognized it
     * (e.g. "mp4", "webm" - so we can tell whether the source container is
     * already Chromecast-compatible). Called once, before playback starts.
     */
    void (*pf_set_source_info)(void *, const char *psz_url, const char *psz_demux,
                               bool b_can_seek);

    /**
     * True while the receiver is playing the source directly (see
     * pf_set_source_info / the Tier-1 direct-serve path): the demux filter
     * uses this to source local playback position from the receiver's own
     * (polled) absolute position instead of the segment-relative timing
     * used by the live-restream path, and to redirect local seeks to
     * pf_seek instead of seeking the local demux.
     */
    bool (*pf_is_source_direct)(void *);

    /**
     * Ask the receiver to seek to an absolute position in the media
     * (Tier-1 only: the receiver has a real, complete index to seek
     * against). Returns false if no request could be sent (e.g. no active
     * media session yet).
     */
    bool (*pf_seek)(void *, vlc_tick_t time);

    /**
     * True once the source has been checked at least once for Tier-1
     * eligibility (pf_is_source_direct() reflects a real answer, not just
     * its not-yet-decided default). Tier-1 eligibility depends on every
     * known track's codec, which is only settled once data has flowed
     * through the sout chain at least once - before that,
     * pf_is_source_direct() is always false, whether or not the session
     * will end up being Tier-1. Code that needs to know the *real* answer
     * (not a possibly-still-pending one) must wait for this first.
     */
    bool (*pf_is_eligibility_decided)(void *);

} chromecast_common;

# ifdef __cplusplus
}
# endif

#endif // VLC_CHROMECAST_COMMON_H


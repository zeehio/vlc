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
     * currently-known track set (whichever way it went). Code that needs
     * the *real* answer, not a possibly-still-default one from before the
     * first eligibility check ran, can wait on this instead of guessing.
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

} chromecast_common;

# ifdef __cplusplus
}
# endif

#endif // VLC_CHROMECAST_COMMON_H


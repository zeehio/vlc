/*****************************************************************************
 * chromecast_demux.cpp: Chromecast demux filter module for vlc
 *****************************************************************************
 * Copyright © 2015-2016 VideoLAN
 *
 * Authors: Steve Lhomme <robux4@videolabs.io>
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

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_demux.h>
#include <vlc_input_item.h>
#include <vlc_stream.h>
#include <vlc_strings.h>

#include "chromecast_common.h"

#include <cassert>
#include <cinttypes>
#include <new>
#include <string>
#include <vector>

static void on_paused_changed_cb(void *data, bool paused);

/**
 * \return true if psz_uri ends with '.' + psz_ext (case-insensitive)
 */
static bool HasExtension( const char *psz_uri, const char *psz_ext )
{
    size_t i_uri = strlen( psz_uri );
    size_t i_ext = strlen( psz_ext );
    if( i_uri < i_ext + 1 || psz_uri[i_uri - i_ext - 1] != '.' )
        return false;
    return vlc_ascii_strcasecmp( psz_uri + i_uri - i_ext, psz_ext ) == 0;
}

/**
 * Parses a "[HH:]MM:SS[.,]mmm" timestamp (SRT uses ',' as the decimal
 * separator, WebVTT uses '.'; both are accepted). Returns false if the
 * string doesn't match either form.
 */
static bool ParseVttTimestamp( const std::string &s, int64_t *out_ms )
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

static std::string FormatVttTimestamp( int64_t ms )
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
static std::string ConvertSubtitleToWebVTT( const std::string &content, bool is_srt,
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

                have_timing = ParseVttTimestamp( ts_start, &cue_start_ms )
                           && ParseVttTimestamp( ts_end, &cue_end_ms );
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
                out += FormatVttTimestamp( cue_start_ms );
                out += " --> ";
                out += FormatVttTimestamp( cue_end_ms );
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

/**
 * Reads an external subtitle slave and, if it is a plain SRT or WebVTT
 * file, returns its content as a heap-allocated WebVTT document (to be
 * handed over to pf_set_subtitle, which takes ownership). Returns NULL for
 * anything else (embedded tracks, styled formats such as ASS/SSA, unreadable
 * or oversized files): those are simply not offered as a Cast sidecar track.
 */
static char *LoadSubtitleAsWebVTT( vlc_object_t *p_obj, const char *psz_uri,
                                   vlc_tick_t i_offset )
{
    bool is_srt = HasExtension( psz_uri, "srt" );
    bool is_vtt = !is_srt && ( HasExtension( psz_uri, "vtt" ) || HasExtension( psz_uri, "webvtt" ) );
    if( !is_srt && !is_vtt )
    {
        msg_Dbg( p_obj, "cc subtitle load: %s has no srt/vtt/webvtt extension", psz_uri );
        return NULL;
    }

    stream_t *s = vlc_stream_NewURL( p_obj, psz_uri );
    if( s == NULL )
    {
        msg_Warn( p_obj, "cc subtitle load: vlc_stream_NewURL failed for %s", psz_uri );
        return NULL;
    }

    uint64_t size;
    /* Subtitle files are tiny; refuse anything unreasonable rather than
     * load a mismatched/huge resource into memory. */
    if( vlc_stream_GetSize( s, &size ) != VLC_SUCCESS || size == 0
     || size > INT64_C(4000000) )
    {
        msg_Warn( p_obj, "cc subtitle load: bad size for %s (size=%" PRIu64 ")", psz_uri, size );
        vlc_stream_Delete( s );
        return NULL;
    }

    char *psz_data = (char *)malloc( size + 1 );
    if( psz_data == NULL )
    {
        vlc_stream_Delete( s );
        return NULL;
    }

    ssize_t read = vlc_stream_Read( s, psz_data, size );
    vlc_stream_Delete( s );
    if( read <= 0 )
    {
        msg_Warn( p_obj, "cc subtitle load: read failed for %s (read=%zd)", psz_uri, read );
        free( psz_data );
        return NULL;
    }
    msg_Dbg( p_obj, "cc subtitle load: read %zd bytes from %s", read, psz_uri );
    psz_data[read] = '\0';

    std::string converted = ConvertSubtitleToWebVTT( std::string( psz_data, read ),
                                                     is_srt, MS_FROM_VLC_TICK( i_offset ) );
    char *psz_webvtt = strdup( converted.c_str() );

    free( psz_data );
    return psz_webvtt;
}

struct demux_cc
{
    demux_cc(demux_t * const demux, chromecast_common * const renderer)
        :p_demux(demux)
        ,p_renderer(renderer)
        ,m_enabled( true )
        ,m_subtitle_scanned( false )
        ,m_subtitle_loaded_once( false )
    {
        init();
    }

    void init()
    {
        resetDemuxEof();

        vlc_meta_t *p_meta = vlc_meta_New();
        if( likely(p_meta != NULL) )
        {
            input_item_t *p_item = p_demux->p_next->p_input ?
                                   input_GetItem( p_demux->p_next->p_input ) : NULL;
            if( p_item )
            {
                /* Favor Meta from the input item of the input_thread since
                 * it's pre-processed by the meta fetcher */
                for( int i = 0; i < VLC_META_TYPE_COUNT; ++i )
                {
                    char *psz_meta = input_item_GetMeta( p_item, (vlc_meta_type_t)i );
                    if( psz_meta )
                    {
                        vlc_meta_Set( p_meta, (vlc_meta_type_t)i, psz_meta );
                        free( psz_meta );
                    }
                }
                if( vlc_meta_Get( p_meta, vlc_meta_Title ) == NULL )
                {
                    char *psz_meta = input_item_GetName( p_item );
                    if( psz_meta )
                    {
                        vlc_meta_Set( p_meta, vlc_meta_Title, psz_meta );
                        free( psz_meta );
                    }
                }
                p_renderer->pf_set_meta( p_renderer->p_opaque, p_meta );
            }
            else if (demux_Control( p_demux->p_next, DEMUX_GET_META, p_meta) == VLC_SUCCESS)
                p_renderer->pf_set_meta( p_renderer->p_opaque, p_meta );
            else
                vlc_meta_Delete( p_meta );
        }

        if (demux_Control( p_demux->p_next, DEMUX_CAN_SEEK, &m_can_seek ) != VLC_SUCCESS)
            m_can_seek = false;
        if (demux_Control( p_demux->p_next, DEMUX_GET_LENGTH, &m_length ) != VLC_SUCCESS)
            m_length = -1;

        /* input_item_t's slave list is not populated yet at this point:
         * Init() creates the master demux (and this filter along with it)
         * before it calls LoadSlaves(), which is what actually fills
         * pp_slaves (it clears it and rebuilds it, including auto-detected
         * same-named subtitle files). Scanning here would always see an
         * empty list, so it is deferred to the first Demux() call instead,
         * which only runs once Init() (and therefore LoadSlaves()) has
         * fully completed. */
        m_subtitle_scanned = false;
        /* This is a fresh casting session (or the demux filter was just
         * re-enabled): its own initial LOAD, sent by the caller, will pick
         * up whatever the upcoming scan sets, so no explicit reload should
         * be requested for it. */
        m_subtitle_loaded_once = false;

        int i_current_title;
        if( demux_Control( p_demux->p_next, DEMUX_GET_TITLE,
                           &i_current_title ) == VLC_SUCCESS )
        {
            input_title_t** pp_titles;
            int i_nb_titles, i_title_offset, i_chapter_offset;
            if( demux_Control( p_demux->p_next, DEMUX_GET_TITLE_INFO, &pp_titles,
                              &i_nb_titles, &i_title_offset,
                              &i_chapter_offset ) == VLC_SUCCESS )
            {
                int64_t i_longest_duration = 0;
                int i_longest_title = 0;
                bool b_is_interactive = false;
                for( int i = 0 ; i < i_nb_titles; ++i )
                {
                    if( pp_titles[i]->i_length > i_longest_duration )
                    {
                        i_longest_duration = pp_titles[i]->i_length;
                        i_longest_title = i;
                    }
                    if( i_current_title == i &&
                            pp_titles[i]->i_flags & INPUT_TITLE_INTERACTIVE )
                    {
                        b_is_interactive = true;
                    }
                    vlc_input_title_Delete( pp_titles[i] );
                }
                free( pp_titles );

                if( b_is_interactive == true )
                {
                    demux_Control( p_demux->p_next, DEMUX_SET_TITLE,
                                   i_longest_title );
                    p_demux->info.i_update = p_demux->p_next->info.i_update;
                }
            }
        }

        es_out_Control( p_demux->p_next->out, ES_OUT_RESET_PCR );

        p_renderer->pf_set_demux_enabled(p_renderer->p_opaque, true,
                                         on_paused_changed_cb, p_demux);

        resetTimes();
    }

    void deinit()
    {
        assert(p_renderer);
        p_renderer->pf_set_meta( p_renderer->p_opaque, NULL );
        p_renderer->pf_set_subtitle( p_renderer->p_opaque, NULL );
        p_renderer->pf_set_demux_enabled(p_renderer->p_opaque, false, NULL, NULL);
    }

    /**
     * Look for an external SRT/WebVTT subtitle slave on the input item and,
     * if found, hand it over as a Cast sidecar text track. Only external
     * slaves are considered (not embedded tracks): this is the only case
     * where the whole subtitle content is available upfront, which a sidecar
     * track needs since it is fetched once by the Cast receiver rather than
     * streamed incrementally. Re-evaluated each time this demux filter is
     * (re)enabled, so subtitles added before a cast starts are picked up;
     * slaves added while already casting need a session restart to appear.
     */
    void setSubtitleFromSlaves()
    {
        std::string uri;
        input_thread_t *p_input = p_demux->p_next->p_input;
        input_item_t *p_item = p_input ? input_GetItem( p_input ) : NULL;
        msg_Dbg( p_demux, "cc subtitle scan: p_input=%p p_item=%p", (void*)p_input, (void*)p_item );
        if( p_item )
        {
            vlc_mutex_lock( &p_item->lock );
            msg_Dbg( p_demux, "cc subtitle scan: i_slaves=%d", p_item->i_slaves );
            for( int i = 0; i < p_item->i_slaves; ++i )
            {
                const input_item_slave_t *p_slave = p_item->pp_slaves[i];
                msg_Dbg( p_demux, "cc subtitle scan: slave[%d] type=%d uri=%s",
                        i, (int)p_slave->i_type, p_slave->psz_uri );
                if( p_slave->i_type != SLAVE_TYPE_SPU )
                    continue;
                /* Only plain SRT/WebVTT slaves can become a sidecar track;
                 * keep looking if this one is a styled/unsupported format. */
                if( HasExtension( p_slave->psz_uri, "srt" )
                 || HasExtension( p_slave->psz_uri, "vtt" )
                 || HasExtension( p_slave->psz_uri, "webvtt" ) )
                {
                    uri = p_slave->psz_uri;
                    break;
                }
                else
                    msg_Dbg( p_demux, "cc subtitle scan: slave[%d] extension not srt/vtt/webvtt, skipping", i );
            }
            vlc_mutex_unlock( &p_item->lock );
        }

        if( uri.empty() )
            msg_Dbg( p_demux, "cc subtitle scan: no matching external SRT/WebVTT slave found" );
        else
            msg_Dbg( p_demux, "cc subtitle scan: using slave uri=%s", uri.c_str() );

        vlc_tick_t offset = 0;
        if( !uri.empty()
         && demux_Control( p_demux->p_next, DEMUX_GET_TIME, &offset ) != VLC_SUCCESS )
            offset = 0;

        char *psz_webvtt = uri.empty() ? NULL
                          : LoadSubtitleAsWebVTT( VLC_OBJECT(p_demux), uri.c_str(), offset );
        msg_Dbg( p_demux, "cc subtitle scan: webvtt %s (segment offset %" PRId64 "ms)",
                psz_webvtt ? "generated" : "NULL (not set)", MS_FROM_VLC_TICK( offset ) );
        p_renderer->pf_set_subtitle( p_renderer->p_opaque, psz_webvtt );

        if( psz_webvtt != NULL )
        {
            /* The Cast protocol only fetches a sidecar text track once, at
             * LOAD time, and never refreshes it on its own (the media is
             * declared as a LIVE stream, which has no notion of seeking:
             * VLC implements seeking locally and just keeps feeding the
             * same connection from a new position). The very first time
             * this runs, the initial LOAD triggered by the caller already
             * picks up what was just set above; on every subsequent call
             * (a seek re-triggered the scan) the receiver is still showing
             * cues generated for the previous segment, so force a fresh
             * LOAD to make it re-fetch what was just regenerated. */
            if( m_subtitle_loaded_once )
            {
                msg_Dbg( p_demux, "cc subtitle scan: requesting a reload so the "
                        "receiver re-fetches the regenerated sidecar track" );
                p_renderer->pf_reload( p_renderer->p_opaque );
            }
            else
                m_subtitle_loaded_once = true;
        }
    }

    void resetTimes()
    {
        m_start_time = m_last_time = -1;
        m_start_pos = m_last_pos = -1.0f;
    }

    void initTimes()
    {
        if( demux_Control( p_demux->p_next, DEMUX_GET_TIME, &m_start_time ) != VLC_SUCCESS )
            m_start_time = -1;

        if( demux_Control( p_demux->p_next, DEMUX_GET_POSITION, &m_start_pos ) != VLC_SUCCESS )
            m_start_pos = -1.0f;

        m_last_time = m_start_time;
        m_last_pos = m_start_pos;
    }

    ~demux_cc()
    {
        if( p_renderer )
            deinit();
    }

    void resetDemuxEof()
    {
        m_demux_eof = false;
        p_renderer->pf_send_input_event( p_renderer->p_opaque, CC_INPUT_EVENT_EOF,
                                         cc_input_arg { false } );
    }

    void setPauseState(bool paused)
    {
        p_renderer->pf_set_pause_state( p_renderer->p_opaque, paused );
    }

    vlc_tick_t getCCTime()
    {
        return p_renderer->pf_get_time( p_renderer->p_opaque );
    }

    vlc_tick_t getTime()
    {
        if( m_start_time < 0 )
            return -1;

        int64_t time = m_start_time;
        vlc_tick_t cc_time = getCCTime();

        if( cc_time != VLC_TICK_INVALID )
            time += cc_time;
        m_last_time = time;
        return time;
    }

    double getPosition()
    {
        if( m_length >= 0 && m_start_pos >= 0 )
        {
            m_last_pos = ( getCCTime() / double( m_length ) ) + m_start_pos;
            return m_last_pos;
        }
        else
            return -1;
    }

    void seekBack( vlc_tick_t time, double pos )
    {
        es_out_Control( p_demux->p_next->out, ES_OUT_RESET_PCR );

        if( m_can_seek )
        {
            int ret = VLC_EGENERIC;
            if( time >= 0 )
                ret = demux_Control( p_demux->p_next, DEMUX_SET_TIME, time, false );

            if( ret != VLC_SUCCESS && pos >= 0 )
                demux_Control( p_demux->p_next, DEMUX_SET_POSITION, pos, false );
        }
    }

    int Demux()
    {
        if ( !m_enabled )
            return demux_Demux( p_demux->p_next );

        if( !m_subtitle_scanned )
        {
            m_subtitle_scanned = true;
            setSubtitleFromSlaves();
        }

        /* The CC sout is not pacing, so we pace here */
        int pace = p_renderer->pf_pace( p_renderer->p_opaque );
        switch (pace)
        {
            case CC_PACE_ERR:
                return VLC_DEMUXER_EGENERIC;
            case CC_PACE_ERR_RETRY:
            {
                /* Seek back to started position */
                seekBack(m_start_time, m_start_pos);

                resetDemuxEof();
                p_renderer->pf_send_input_event( p_renderer->p_opaque,
                                                 CC_INPUT_EVENT_RETRY,
                                                 cc_input_arg{false} );
                break;
            }
            case CC_PACE_OK_WAIT:
                /* Yield: return to let the input thread doing controls  */
                return VLC_DEMUXER_SUCCESS;
            case CC_PACE_OK:
            case CC_PACE_OK_ENDED:
                break;
            default:
                vlc_assert_unreachable();
        }

        int ret = VLC_DEMUXER_SUCCESS;
        if( !m_demux_eof )
        {
            ret = demux_Demux( p_demux->p_next );
            if( ret != VLC_DEMUXER_EGENERIC
             && ( m_start_time < 0 || m_start_pos < 0.0f ) )
                initTimes();
            if( ret == VLC_DEMUXER_EOF )
                m_demux_eof = true;
        }

        if( m_demux_eof )
        {
            /* Signal EOF to the sout when the es_out is empty (so when the
             * DecoderThread fifo are empty) */
            bool b_empty;
            es_out_Control( p_demux->p_next->out, ES_OUT_GET_EMPTY, &b_empty );
            if( b_empty )
                p_renderer->pf_send_input_event( p_renderer->p_opaque,
                                                 CC_INPUT_EVENT_EOF,
                                                 cc_input_arg{ true } );

            /* Don't return EOF until the chromecast is not EOF. This allows
             * this demux filter to have more controls over the sout. Indeed,
             * we still can seek or change tracks when the input is EOF and we
             * should continue to handle CC errors. */
            ret = pace == CC_PACE_OK ? VLC_DEMUXER_SUCCESS : VLC_DEMUXER_EOF;
        }

        return ret;
    }

    int Control( demux_t *p_demux_filter, int i_query, va_list args )
    {
        if( !m_enabled && i_query != DEMUX_FILTER_ENABLE )
            return demux_vaControl( p_demux_filter->p_next, i_query, args );

        switch (i_query)
        {
        case DEMUX_GET_POSITION:
        {
            double pos = getPosition();
            if( pos >= 0 )
            {
                *va_arg( args, double * ) = pos;
                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;
        }
        case DEMUX_GET_TIME:
        {
            vlc_tick_t time = getTime();
            if( time >= 0 )
            {
                *va_arg(args, int64_t *) = time;
                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;
        }
        case DEMUX_GET_LENGTH:
        {
            int ret;
            va_list ap;

            va_copy( ap, args );
            ret = demux_vaControl( p_demux_filter->p_next, i_query, args );
            if( ret == VLC_SUCCESS )
                m_length = *va_arg( ap, int64_t * );
            va_end( ap );
            return ret;
        }

        case DEMUX_CAN_SEEK:
        {
            int ret;
            va_list ap;

            va_copy( ap, args );
            ret = demux_vaControl( p_demux_filter->p_next, i_query, args );
            if( ret == VLC_SUCCESS )
                m_can_seek = *va_arg( ap, bool* );
            va_end( ap );
            return ret;
        }

        case DEMUX_SET_POSITION:
        {
            double pos = va_arg( args, double );
            /* Force imprecise seek */
            int ret = demux_Control( p_demux->p_next, DEMUX_SET_POSITION, pos, false );
            if( ret != VLC_SUCCESS )
                return ret;

            resetTimes();
            resetDemuxEof();
            /* The segment the receiver is about to be fed starts at a new
             * position: re-run the subtitle scan on the next Demux() call
             * so the sidecar track (if any) gets regenerated for it and
             * the receiver is told to reload. */
            m_subtitle_scanned = false;
            return VLC_SUCCESS;
        }
        case DEMUX_SET_TIME:
        {
            vlc_tick_t time = va_arg( args, int64_t );
            /* Force imprecise seek */
            int ret = demux_Control( p_demux->p_next, DEMUX_SET_TIME, time, false );
            if( ret != VLC_SUCCESS )
                return ret;

            resetTimes();
            resetDemuxEof();
            m_subtitle_scanned = false;
            return VLC_SUCCESS;
        }
        case DEMUX_SET_PAUSE_STATE:
        {
            va_list ap;

            va_copy( ap, args );
            int paused = va_arg( ap, int );
            va_end( ap );

            setPauseState( paused != 0 );
            break;
        }
        case DEMUX_SET_ES:
            /* Seek back to the last known pos when changing tracks. This will
             * flush sout streams, make sout del/add called right away and
             * clear CC buffers. */
            seekBack(m_last_time, m_last_pos);
            resetTimes();
            resetDemuxEof();
            m_subtitle_scanned = false;
            break;
        case DEMUX_FILTER_ENABLE:
            p_renderer = static_cast<chromecast_common *>(
                        var_InheritAddress( p_demux, CC_SHARED_VAR_NAME ) );
            assert(p_renderer != NULL);
            m_enabled = true;
            init();
            return VLC_SUCCESS;

        case DEMUX_FILTER_DISABLE:

            deinit();

            /* Seek back to last known position. Indeed we don't want to resume
             * from the input position that can be more than 1 minutes forward
             * (depending on the CC buffering policy). */
            seekBack(m_last_time, m_last_pos);

            m_enabled = false;
            p_renderer = NULL;

            return VLC_SUCCESS;
        case DEMUX_CAN_PAUSE:
        case DEMUX_CAN_CONTROL_PACE:
        {
            int ret;
            va_list ap;

            va_copy( ap, args );
            ret = demux_vaControl( p_demux_filter->p_next, i_query, args );
            if( ret != VLC_SUCCESS )
                *va_arg( ap, bool* ) = false;
            va_end( ap );
            return VLC_SUCCESS;
        }
        case DEMUX_GET_PTS_DELAY:
        {
            int ret;
            va_list ap;

            va_copy( ap, args );
            ret = demux_vaControl( p_demux_filter->p_next, i_query, args );
            if( ret != VLC_SUCCESS )
                *va_arg( ap, int64_t* ) = 0;
            va_end( ap );
            return VLC_SUCCESS;
        }
        }

        return demux_vaControl( p_demux_filter->p_next, i_query, args );
    }

protected:
    demux_t     * const p_demux;
    chromecast_common  * p_renderer;
    vlc_tick_t    m_length;
    bool          m_can_seek;
    bool          m_enabled;
    bool          m_subtitle_scanned;
    bool          m_subtitle_loaded_once;
    bool          m_demux_eof;
    double        m_start_pos;
    double        m_last_pos;
    vlc_tick_t    m_start_time;
    vlc_tick_t    m_last_time;
};

static void on_paused_changed_cb( void *data, bool paused )
{
    demux_t *p_demux = reinterpret_cast<demux_t*>(data);

    input_thread_t *p_input = p_demux->p_next->p_input;
    if( p_input )
        input_Control( p_input, INPUT_SET_STATE, paused ? PAUSE_S : PLAYING_S );
}

static int Demux( demux_t *p_demux_filter )
{
    demux_cc *p_sys = reinterpret_cast<demux_cc*>(p_demux_filter->p_sys);

    return p_sys->Demux();
}

static int Control( demux_t *p_demux_filter, int i_query, va_list args)
{
    demux_cc *p_sys = reinterpret_cast<demux_cc*>(p_demux_filter->p_sys);

    return p_sys->Control( p_demux_filter, i_query, args );
}

int Open(vlc_object_t *p_this)
{
    demux_t *p_demux = reinterpret_cast<demux_t*>(p_this);
    chromecast_common *p_renderer = static_cast<chromecast_common *>(
                var_InheritAddress( p_demux, CC_SHARED_VAR_NAME ) );
    if ( p_renderer == NULL )
    {
        msg_Warn( p_this, "using Chromecast demuxer with no sout" );
        return VLC_ENOOBJ;
    }

    demux_cc *p_sys = new(std::nothrow) demux_cc( p_demux, p_renderer );
    if (unlikely(p_sys == NULL))
        return VLC_ENOMEM;

    p_demux->p_sys = reinterpret_cast<demux_sys_t*>(p_sys);
    p_demux->pf_demux = Demux;
    p_demux->pf_control = Control;

    return VLC_SUCCESS;
}

void Close(vlc_object_t *p_this)
{
    demux_t *p_demux = reinterpret_cast<demux_t*>(p_this);
    demux_cc *p_sys = reinterpret_cast<demux_cc*>(p_demux->p_sys);

    delete p_sys;
}

vlc_module_begin ()
    set_shortname( "cc_demux" )
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_DEMUX )
    set_description( N_( "Chromecast demux wrapper" ) )
    set_capability( "demux_filter", 0 )
    add_shortcut( "cc_demux" )
    set_callbacks( Open, Close )
vlc_module_end ()

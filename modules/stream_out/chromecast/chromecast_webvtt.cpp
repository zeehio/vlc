/*****************************************************************************
 * chromecast_webvtt.cpp: convert a subtitle file to WebVTT for the
 * Chromecast sidecar track, reusing VLC's own subtitle demux/decode/
 * encode/mux chain
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

#include <vlc_stream.h>
#include <vlc_demux.h>
#include <vlc_codec.h>
#include <vlc_es_out.h>
#include <vlc_sout.h>
#include <vlc_modules.h>
#include <vlc_block.h>

#include <vector>

namespace {

/* ---- decoder: owns a decoder_t and collects every subpicture it queues,
 * decoding synchronously (this is a one-shot batch conversion of a whole
 * file, not a live pipeline: no threads, no pacing). Mirrors the pattern
 * modules/stream_out/transcode/spu.c and src/misc/image.c's CreateDecoder
 * use for the same headless "load a decoder module directly" need: the
 * decoder_t must be the first member of a vlc_object_create()'d block, C++
 * construction does not apply to it. ---- */
struct SubtitleDecoder
{
    decoder_t     dec;
    es_format_t   fmt_in;
    subpicture_t *spu_first;
    subpicture_t **spu_last;

    static subpicture_t *BufferNew( decoder_t *, const subpicture_updater_t *upd )
    {
        return subpicture_New( upd );
    }

    static void QueueSub( decoder_t *dec, subpicture_t *spu )
    {
        SubtitleDecoder *sys = container_of( dec, SubtitleDecoder, dec );
        *sys->spu_last = spu;
        sys->spu_last = &spu->p_next;
        spu->p_next = nullptr;
    }
};

void DeleteSubtitleDecoder( SubtitleDecoder *sys )
{
    if( sys == nullptr )
        return;
    for( subpicture_t *sp = sys->spu_first; sp != nullptr; )
    {
        subpicture_t *next = sp->p_next;
        subpicture_Delete( sp );
        sp = next;
    }
    es_format_Clean( &sys->fmt_in );
    decoder_Destroy( &sys->dec ); /* frees the SubtitleDecoder block itself */
}

/* ---- minimal es_out_t: hands every SPU_ES block from the target track to
 * the decoder above as soon as the demuxer sends it (no fifo/threading
 * needed: pf_decode is a synchronous call for spu decoders).
 *
 * Two modes, selected by i_track_id:
 *  - external-file mode (i_track_id < 0): the demuxer behind this es_out
 *    is always the generic "subtitle" module, which only ever has one ES,
 *    so the first (and only) SPU track found is it. Every other track
 *    (there is none in practice) is rejected outright.
 *  - embedded-track mode (i_track_id >= 0): the demuxer behind this
 *    es_out is a real container demuxer with several tracks of every
 *    kind. Only the SPU track whose container-assigned id matches is
 *    decoded, but every track still needs Add() to return a valid,
 *    distinct id: container demuxers track each one's selection state by
 *    id (see e.g. MP4_TrackSelect()/DemuxTrack() in
 *    modules/demux/mp4/mp4.c) and poll it back via ES_OUT_GET_ES_STATE
 *    before reading its samples, skipping (seeking over, not reading)
 *    those reported unselected - which is exactly how real playback
 *    already avoids decoding tracks nobody asked for. Answering that
 *    query honestly for every id here is what makes this pass over the
 *    file cheap, and it is a plain property of the es_out_t contract any
 *    conformant demuxer already relies on, not something specific to any
 *    one container. ---- */
struct SubtitleEsOut
{
    es_out_t out;
    vlc_object_t *p_parent;
    SubtitleDecoder *decoder;
    es_out_id_t *target_id;
    int i_track_id;
    std::vector<void *> placeholders;

    static es_out_id_t *Add( es_out_t *out, input_source_t *, const es_format_t *fmt )
    {
        SubtitleEsOut *sys = container_of( out, SubtitleEsOut, out );

        bool b_want = sys->decoder == nullptr && fmt->i_cat == SPU_ES
                    && ( sys->i_track_id < 0 || fmt->i_id == sys->i_track_id );
        if( !b_want )
        {
            if( sys->i_track_id < 0 )
                return nullptr;

            void *placeholder = malloc( 1 );
            if( unlikely( placeholder == nullptr ) )
                return nullptr;
            sys->placeholders.push_back( placeholder );
            return static_cast<es_out_id_t *>( placeholder );
        }

        SubtitleDecoder *dec = vlc_object_create<SubtitleDecoder>( sys->p_parent );
        if( unlikely( dec == nullptr ) )
            return nullptr;
        dec->spu_first = nullptr;
        dec->spu_last = &dec->spu_first;

        decoder_Init( &dec->dec, &dec->fmt_in, fmt );

        static const struct decoder_owner_callbacks cbs = {
            .spu = { .buffer_new = SubtitleDecoder::BufferNew,
                     .queue = SubtitleDecoder::QueueSub },
        };
        dec->dec.cbs = &cbs;
        dec->dec.pf_decode = nullptr;
        decoder_LoadModule( &dec->dec, false, true );
        if( dec->dec.p_module == nullptr )
        {
            msg_Warn( sys->p_parent, "cc subtitle convert: no spu decoder found" );
            es_format_Clean( &dec->fmt_in );
            decoder_Destroy( &dec->dec );
            return nullptr;
        }

        sys->decoder = dec;
        sys->target_id = reinterpret_cast<es_out_id_t *>( dec );
        return sys->target_id;
    }

    static int Send( es_out_t *out, es_out_id_t *id, block_t *block )
    {
        SubtitleEsOut *sys = container_of( out, SubtitleEsOut, out );
        if( sys->decoder == nullptr || id != sys->target_id
         || sys->decoder->dec.pf_decode == nullptr )
        {
            block_Release( block );
            return VLC_SUCCESS;
        }
        sys->decoder->dec.pf_decode( &sys->decoder->dec, block );
        return VLC_SUCCESS;
    }

    static void Del( es_out_t *, es_out_id_t * ) {}

    static int Control( es_out_t *out, input_source_t *, int query, va_list args )
    {
        if( query == ES_OUT_GET_ES_STATE )
        {
            SubtitleEsOut *sys = container_of( out, SubtitleEsOut, out );
            es_out_id_t *id = va_arg( args, es_out_id_t * );
            bool *pb_selected = va_arg( args, bool * );
            *pb_selected = ( id == sys->target_id );
            return VLC_SUCCESS;
        }
        return VLC_EGENERIC;
    }

    static void Destroy( es_out_t * ) {}
};

const struct es_out_callbacks es_out_cbs = {
    .add = SubtitleEsOut::Add, .send = SubtitleEsOut::Send, .del = SubtitleEsOut::Del,
    .control = SubtitleEsOut::Control, .destroy = SubtitleEsOut::Destroy,
};

/* ---- in-memory sout_access_out_t: the "webvtt" muxer writes its output
 * through this instead of a real socket/file. ---- */
struct MemorySink
{
    char *buf = nullptr;
    size_t len = 0, cap = 0;

    bool Append( const uint8_t *data, size_t n )
    {
        if( len + n > cap )
        {
            size_t new_cap = ( len + n ) * 2 + 64;
            char *new_buf = static_cast<char *>( realloc( buf, new_cap ) );
            if( unlikely( new_buf == nullptr ) )
                return false;
            buf = new_buf;
            cap = new_cap;
        }
        memcpy( buf + len, data, n );
        len += n;
        return true;
    }

    static ssize_t Write( sout_access_out_t *access, block_t *block )
    {
        MemorySink *sys = static_cast<MemorySink *>( access->p_sys );
        ssize_t total = 0;
        bool ok = true;
        while( block )
        {
            if( ok )
                ok = sys->Append( block->p_buffer, block->i_buffer );
            total += block->i_buffer;
            block_t *next = block->p_next;
            block_Release( block );
            block = next;
        }
        return ok ? total : -1;
    }
};

/**
 * Shared tail for both public entry points below: once a demux pass is
 * done, an external subtitle file and an embedded track look identical -
 * a decoder that collected zero or more subpictures, plus a segment
 * offset. Always takes ownership of *p_decoder (destroys it, even on
 * failure); p_decoder may be NULL (no matching track was found).
 */
char *EncodeToWebVTT( vlc_object_t *p_parent, SubtitleDecoder *p_decoder,
                      vlc_tick_t i_offset, const char *psz_log_id )
{
    if( p_decoder == nullptr )
    {
        msg_Dbg( p_parent, "cc subtitle convert: no subtitle track found in %s", psz_log_id );
        return nullptr;
    }

    /* spu decoders (subsdec, libass, ...) don't lay out regions at decode
     * time: that's deferred to a subpicture_updater_t, normally invoked by
     * the video compositor against the real canvas size. We have none, so
     * use a placeholder: WebVTT cue positioning is percentage/line-based
     * (see encvtt.c's use of i_original_picture_height), not pixel-exact. */
    video_format_t canvas;
    video_format_Init( &canvas, 0 );
    canvas.i_width = canvas.i_visible_width = 1280;
    canvas.i_height = canvas.i_visible_height = 720;
    canvas.i_sar_num = canvas.i_sar_den = 1;

    encoder_t *enc = sout_EncoderCreate( p_parent, sizeof( *enc ) );
    if( unlikely( enc == nullptr ) )
    {
        DeleteSubtitleDecoder( p_decoder );
        return nullptr;
    }
    es_format_Init( &enc->fmt_in, SPU_ES, p_decoder->fmt_in.i_codec );
    es_format_Init( &enc->fmt_out, SPU_ES, VLC_CODEC_WEBVTT );
    enc->p_cfg = nullptr;
    /* No shortcut name: exactly one "spu encoder" module claims
     * VLC_CODEC_WEBVTT (encvtt.c), matched purely by fmt_out.i_codec. */
    enc->p_module = module_need( enc, "spu encoder", nullptr, false );
    if( enc->p_module == nullptr )
    {
        msg_Warn( p_parent, "cc subtitle convert: no WebVTT spu encoder available" );
        es_format_Clean( &enc->fmt_in );
        es_format_Clean( &enc->fmt_out );
        vlc_object_delete( enc );
        DeleteSubtitleDecoder( p_decoder );
        return nullptr;
    }

    sout_access_out_t *access = vlc_object_create<sout_access_out_t>( p_parent );
    sout_mux_t *mux = nullptr;
    sout_input_t *mux_input = nullptr;
    MemorySink sink;
    if( access != nullptr )
    {
        access->psz_access = strdup( "mock" );
        access->p_cfg = nullptr;
        access->p_module = nullptr;
        access->p_sys = &sink;
        access->psz_path = nullptr;
        access->pf_control = nullptr;
        access->pf_read = nullptr;
        access->pf_seek = nullptr;
        access->pf_write = MemorySink::Write;
        if( access->psz_access != nullptr )
            mux = sout_MuxNew( access, "webvtt" );
    }
    if( mux != nullptr )
    {
        es_format_t mux_fmt;
        es_format_Init( &mux_fmt, SPU_ES, VLC_CODEC_WEBVTT );
        mux_input = sout_MuxAddStream( mux, &mux_fmt );
        mux->b_waiting_stream = false;
    }

    unsigned n_cues = 0;
    if( mux_input != nullptr )
    {
        for( subpicture_t *sp = p_decoder->spu_first; sp != nullptr; sp = sp->p_next )
        {
            subpicture_Update( sp, &canvas, &canvas, canvas.i_width, canvas.i_height,
                               sp->i_start );

            /* Shift into the current Cast segment's own timeline (see the
             * header doc), dropping cues that end up entirely before it. */
            vlc_tick_t start = sp->i_start - i_offset + VLC_TICK_0;
            vlc_tick_t stop = sp->i_stop != VLC_TICK_INVALID
                             ? sp->i_stop - i_offset + VLC_TICK_0 : VLC_TICK_INVALID;
            if( stop != VLC_TICK_INVALID && stop <= VLC_TICK_0 )
                continue;

            subpicture_t saved = *sp;
            sp->i_start = start;
            sp->i_stop = stop;
            block_t *enc_blocks = enc->ops->encode_sub( enc, sp );
            *sp = saved;

            while( enc_blocks != nullptr )
            {
                block_t *next = enc_blocks->p_next;
                enc_blocks->p_next = nullptr;
                if( sout_MuxSendBuffer( mux, mux_input, enc_blocks ) == VLC_SUCCESS )
                    n_cues++;
                enc_blocks = next;
            }
        }
    }

    msg_Dbg( p_parent, "cc subtitle convert: %s -> %u WebVTT cue(s) for this segment",
            psz_log_id, n_cues );

    if( mux_input != nullptr )
        sout_MuxDeleteStream( mux, mux_input );
    if( mux != nullptr )
        sout_MuxDelete( mux );
    if( access != nullptr )
    {
        free( access->psz_access );
        vlc_object_delete( access );
    }
    vlc_encoder_Destroy( enc );
    DeleteSubtitleDecoder( p_decoder );

    if( n_cues == 0 )
    {
        free( sink.buf );
        return nullptr;
    }

    /* MemorySink's buffer is plain malloc()'d, NUL-terminate and hand
     * ownership to the caller as-is. */
    char *out = static_cast<char *>( realloc( sink.buf, sink.len + 1 ) );
    if( unlikely( out == nullptr ) )
    {
        free( sink.buf );
        return nullptr;
    }
    out[sink.len] = '\0';
    return out;
}

} // namespace

char *chromecast_ConvertSubtitleFileToWebVTT( vlc_object_t *p_parent,
                                              const char *psz_uri,
                                              vlc_tick_t i_offset )
{
    stream_t *s = vlc_stream_NewURL( p_parent, psz_uri );
    if( s == nullptr )
    {
        msg_Warn( p_parent, "cc subtitle convert: vlc_stream_NewURL failed for %s", psz_uri );
        return nullptr;
    }

    /* demux/subtitle.c inherits this var from input_thread_t normally (see
     * src/input/var.c: "Inherited by demux/subtitle.c"); we're not running
     * under one, so create it ourselves - otherwise it asserts. */
    var_Create( p_parent, "sub-original-fps", VLC_VAR_FLOAT );

    SubtitleEsOut es_out = { { &es_out_cbs }, p_parent, nullptr, nullptr, -1, {} };
    demux_t *demux = demux_New( p_parent, "subtitle", psz_uri, s, &es_out.out );
    if( demux == nullptr )
    {
        msg_Dbg( p_parent, "cc subtitle convert: %s is not a subtitle file VLC recognizes",
                psz_uri );
        vlc_stream_Delete( s );
        return nullptr;
    }

    int ret;
    do
        ret = demux_Demux( demux );
    while( ret == VLC_DEMUXER_SUCCESS );

    demux_Delete( demux ); /* also deletes s */

    return EncodeToWebVTT( p_parent, es_out.decoder, i_offset, psz_uri );
}

char *chromecast_ConvertEmbeddedTrackToWebVTT( vlc_object_t *p_parent,
                                               const char *psz_uri,
                                               const char *psz_demux,
                                               int i_track_id,
                                               vlc_tick_t i_offset )
{
    stream_t *s = vlc_stream_NewURL( p_parent, psz_uri );
    if( s == nullptr )
    {
        msg_Warn( p_parent, "cc embedded subtitle convert: vlc_stream_NewURL failed for %s",
                 psz_uri );
        return nullptr;
    }

    SubtitleEsOut es_out = { { &es_out_cbs }, p_parent, nullptr, nullptr, i_track_id, {} };
    /* Ask for the exact demux module the master source is already using,
     * not "any"/auto-detection: this is a second, independent pass over
     * the same file, and it has to see the same track layout - and
     * therefore assign the same track ids - as the live session that
     * i_track_id was taken from. */
    demux_t *demux = demux_New( p_parent, psz_demux, psz_uri, s, &es_out.out );
    if( demux == nullptr )
    {
        msg_Dbg( p_parent, "cc embedded subtitle convert: %s could not be reopened with "
                "the \"%s\" demuxer", psz_uri, psz_demux );
        vlc_stream_Delete( s );
        return nullptr;
    }

    int ret;
    do
        ret = demux_Demux( demux );
    while( ret == VLC_DEMUXER_SUCCESS );

    demux_Delete( demux ); /* also deletes s */

    for( void *p : es_out.placeholders )
        free( p );

    return EncodeToWebVTT( p_parent, es_out.decoder, i_offset, psz_uri );
}

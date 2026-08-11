/*****************************************************************************
 * chromecast_webvtt.cpp: convert a subtitle file to WebVTT for the
 * Chromecast sidecar track, reusing VLC's own subtitle demux/decode chain
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
#include <vlc_modules.h>
#include <vlc_text_style.h>

#include <cstdio>
#include <string>

namespace {

/* ---- decoder: owns a decoder_t and collects every subpicture it queues,
 * decoding synchronously (this is a one-shot batch conversion of a whole
 * file, not a live pipeline: no threads, no pacing). Mirrors the pattern
 * modules/stream_out/transcode/spu.c uses for the same "spu decoder"
 * capability, minus everything specific to a live sout_stream. Unlike the
 * 4.0 branch this is ported from, decoder_t here is a plain vlc_object_t
 * (VLC_COMMON_MEMBERS) with fmt_in/callbacks as direct members - no
 * separate "owner" wrapper struct is needed. ---- */
struct SubtitleDecoder
{
    decoder_t     dec;
    subpicture_t *spu_first;
    subpicture_t **spu_last;

    static subpicture_t *BufferNew( decoder_t *, const subpicture_updater_t *upd )
    {
        return subpicture_New( upd );
    }

    static int QueueSub( decoder_t *dec, subpicture_t *spu )
    {
        SubtitleDecoder *sys = static_cast<SubtitleDecoder *>( dec->p_queue_ctx );
        *sys->spu_last = spu;
        sys->spu_last = &spu->p_next;
        spu->p_next = nullptr;
        return VLC_SUCCESS;
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
    if( sys->dec.p_module != nullptr )
        module_unneed( &sys->dec, sys->dec.p_module );
    es_format_Clean( &sys->dec.fmt_in );
    es_format_Clean( &sys->dec.fmt_out );
    vlc_object_release( &sys->dec );
}

/* ---- minimal es_out_t: hands every SPU_ES block to the decoder above as
 * soon as the demuxer sends it (no fifo/threading needed: pf_decode is a
 * synchronous call for spu decoders). Only the first SPU track found is
 * used - a sidecar track needs one text stream, and subtitle files don't
 * carry more than one anyway. ---- */
struct SubtitleEsOut
{
    es_out_t out;
    vlc_object_t *p_parent;
    SubtitleDecoder *decoder;

    static es_out_id_t *Add( es_out_t *out, const es_format_t *fmt )
    {
        SubtitleEsOut *sys = container_of( out, SubtitleEsOut, out );
        if( fmt->i_cat != SPU_ES || sys->decoder != nullptr )
            return nullptr;

        SubtitleDecoder *dec = static_cast<SubtitleDecoder *>(
            vlc_object_create( sys->p_parent, sizeof( SubtitleDecoder ) ) );
        if( unlikely( dec == nullptr ) )
            return nullptr;
        dec->spu_first = nullptr;
        dec->spu_last = &dec->spu_first;

        if( es_format_Copy( &dec->dec.fmt_in, fmt ) != VLC_SUCCESS )
        {
            vlc_object_release( &dec->dec );
            return nullptr;
        }
        es_format_Init( &dec->dec.fmt_out, SPU_ES, 0 );
        dec->dec.p_module = nullptr;
        dec->dec.p_sys = nullptr;
        dec->dec.b_frame_drop_allowed = false;
        dec->dec.pf_decode = nullptr;
        dec->dec.pf_spu_buffer_new = SubtitleDecoder::BufferNew;
        dec->dec.pf_queue_sub = SubtitleDecoder::QueueSub;
        dec->dec.p_queue_ctx = dec;

        dec->dec.p_module = module_need( &dec->dec, "spu decoder", nullptr, false );
        if( dec->dec.p_module == nullptr )
        {
            msg_Warn( sys->p_parent, "cc subtitle convert: no spu decoder found" );
            es_format_Clean( &dec->dec.fmt_in );
            es_format_Clean( &dec->dec.fmt_out );
            vlc_object_release( &dec->dec );
            return nullptr;
        }

        sys->decoder = dec;
        return reinterpret_cast<es_out_id_t *>( dec );
    }

    static int Send( es_out_t *out, es_out_id_t *id, block_t *block )
    {
        SubtitleEsOut *sys = container_of( out, SubtitleEsOut, out );
        if( reinterpret_cast<SubtitleDecoder *>( id ) != sys->decoder
         || sys->decoder->dec.pf_decode == nullptr )
        {
            block_Release( block );
            return VLC_SUCCESS;
        }
        sys->decoder->dec.pf_decode( &sys->decoder->dec, block );
        return VLC_SUCCESS;
    }

    static void Del( es_out_t *, es_out_id_t * ) {}
    static int Control( es_out_t *, int, va_list ) { return VLC_EGENERIC; }
};

/* ---- WebVTT text serialization: this VLC checkout has no "spu encoder"
 * or "sout mux" module for WebVTT (those are 4.0-only), so cues are
 * formatted directly from the decoded subpicture text regions instead of
 * going through that plugin machinery - there is no ISOBMFF box framing
 * to worry about either way, this only ever needs to produce a plain
 * text/vtt document. Escaping mirrors modules/codec/webvtt/encvtt.c on
 * the 4.0 branch this is ported from. ---- */
void AppendEscaped( std::string &out, const char *psz )
{
    for( ; *psz; ++psz )
    {
        switch( *psz )
        {
            case '&': out += "&#x26;"; break;
            case '<': out += "&#x3c;"; break;
            case '>': out += "&#x3e;"; break; /* escapes forbidden --> sequence */
            default:  out += *psz;
        }
    }
}

void AppendCueText( std::string &out, const subpicture_region_t *region )
{
    for( const text_segment_t *seg = region->p_text; seg != nullptr; seg = seg->p_next )
    {
        if( seg->psz_text == nullptr )
            continue;

        const text_style_t *style = seg->style;
        bool bold = false, italic = false, underline = false;
        if( style != nullptr && ( style->i_features & STYLE_HAS_FLAGS ) )
        {
            bold = style->i_style_flags & STYLE_BOLD;
            italic = style->i_style_flags & STYLE_ITALIC;
            underline = style->i_style_flags & STYLE_UNDERLINE;
        }

        if( bold ) out += "<b>";
        if( underline ) out += "<u>";
        if( italic ) out += "<i>";
        AppendEscaped( out, seg->psz_text );
        if( italic ) out += "</i>";
        if( underline ) out += "</u>";
        if( bold ) out += "</b>";
    }
}

std::string FormatVttTimestamp( vlc_tick_t t )
{
    if( t < 0 )
        t = 0;
    int64_t ms = MS_FROM_VLC_TICK( t );
    unsigned msec = (unsigned)( ms % 1000 ); ms /= 1000;
    unsigned sec  = (unsigned)( ms % 60 );   ms /= 60;
    unsigned min  = (unsigned)( ms % 60 );   ms /= 60;
    unsigned hour = (unsigned)ms;
    char buf[32];
    snprintf( buf, sizeof(buf), "%02u:%02u:%02u.%03u", hour, min, sec, msec );
    return std::string( buf );
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

    /* demux/subtitle.c inherits this var from input_thread_t normally; we
     * are not running under one, so create it ourselves defensively. */
    var_Create( p_parent, "sub-original-fps", VLC_VAR_FLOAT );

    SubtitleEsOut es_out;
    es_out.out.pf_add = SubtitleEsOut::Add;
    es_out.out.pf_send = SubtitleEsOut::Send;
    es_out.out.pf_del = SubtitleEsOut::Del;
    es_out.out.pf_control = SubtitleEsOut::Control;
    es_out.p_parent = p_parent;
    es_out.decoder = nullptr;

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

    if( es_out.decoder == nullptr )
    {
        msg_Dbg( p_parent, "cc subtitle convert: no subtitle track found in %s", psz_uri );
        return nullptr;
    }

    /* spu decoders (subsdec, libass, ...) don't lay out regions at decode
     * time: that's deferred to a subpicture_updater_t, normally invoked by
     * the video compositor against the real canvas size. We have none, so
     * use a placeholder: WebVTT cue positioning here is percentage/line
     * based, not pixel-exact. */
    video_format_t canvas;
    video_format_Init( &canvas, 0 );
    canvas.i_width = canvas.i_visible_width = 1280;
    canvas.i_height = canvas.i_visible_height = 720;
    canvas.i_sar_num = canvas.i_sar_den = 1;

    std::string out( "WEBVTT\n\n" );
    unsigned n_cues = 0;

    for( subpicture_t *sp = es_out.decoder->spu_first; sp != nullptr; sp = sp->p_next )
    {
        subpicture_Update( sp, &canvas, &canvas, sp->i_start );

        /* Shift into the current Cast segment's own timeline (see the
         * header doc), dropping cues that end up entirely before it. */
        vlc_tick_t start = sp->i_start - i_offset + VLC_TICK_0;
        vlc_tick_t stop = sp->i_stop != VLC_TICK_INVALID
                         ? sp->i_stop - i_offset + VLC_TICK_0 : VLC_TICK_INVALID;
        if( stop != VLC_TICK_INVALID && stop <= VLC_TICK_0 )
            continue;

        for( const subpicture_region_t *r = sp->p_region; r != nullptr; r = r->p_next )
        {
            if( r->p_text == nullptr )
                continue;

            std::string cue_text;
            AppendCueText( cue_text, r );
            if( cue_text.empty() )
                continue;

            out += FormatVttTimestamp( start );
            out += " --> ";
            out += FormatVttTimestamp( stop != VLC_TICK_INVALID ? stop : start + VLC_TICK_FROM_SEC(4) );
            out += '\n';
            out += cue_text;
            out += "\n\n";
            n_cues++;
        }
    }

    msg_Dbg( p_parent, "cc subtitle convert: %s -> %u WebVTT cue(s) for this segment",
            psz_uri, n_cues );

    DeleteSubtitleDecoder( es_out.decoder );

    if( n_cues == 0 )
        return nullptr;

    return strdup( out.c_str() );
}

/*****************************************************************************
 * chromecast_http_range.cpp: HTTP Range header parsing unit testing
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

#include <vlc_common.h>
#include <vlc_httpd.h>

#include "../../../modules/stream_out/chromecast/chromecast_http_range.h"

/* httpd_MsgAdd()/httpd_MsgGet() only touch the message's own header list -
 * no vlc_object_t, no running instance needed - so a stack-allocated,
 * zero-initialized httpd_message_t is exactly what a real query looks like
 * to chromecast_ParseByteRange(). */
static httpd_message_t MakeQuery( const char *psz_range )
{
    httpd_message_t query = {};
    if( psz_range != NULL )
        httpd_MsgAdd( &query, "Range", "%s", psz_range );
    return query;
}

static void test_NoRangeHeaderServesWhole()
{
    httpd_message_t query = MakeQuery( NULL );
    uint64_t start, end;
    bool partial = chromecast_ParseByteRange( &query, 1000, &start, &end );

    assert( !partial );
    assert( start == 0 );
    assert( end == 999 );
}

static void test_SimpleRange()
{
    httpd_message_t query = MakeQuery( "bytes=100-199" );
    uint64_t start, end;
    bool partial = chromecast_ParseByteRange( &query, 1000, &start, &end );

    assert( partial );
    assert( start == 100 );
    assert( end == 199 );
}

static void test_OpenEndedRangeClampsToResourceEnd()
{
    httpd_message_t query = MakeQuery( "bytes=900-" );
    uint64_t start, end;
    bool partial = chromecast_ParseByteRange( &query, 1000, &start, &end );

    assert( partial );
    assert( start == 900 );
    assert( end == 999 );
}

static void test_RangeEndPastResourceSizeIsClamped()
{
    httpd_message_t query = MakeQuery( "bytes=500-999999" );
    uint64_t start, end;
    bool partial = chromecast_ParseByteRange( &query, 1000, &start, &end );

    assert( partial );
    assert( start == 500 );
    assert( end == 999 );
}

static void test_ZeroToLastByte()
{
    httpd_message_t query = MakeQuery( "bytes=0-999" );
    uint64_t start, end;
    bool partial = chromecast_ParseByteRange( &query, 1000, &start, &end );

    assert( partial );
    assert( start == 0 );
    assert( end == 999 );
}

static void test_StartAtOrPastResourceSizeServesWhole()
{
    httpd_message_t query = MakeQuery( "bytes=1000-1005" );
    uint64_t start, end;
    bool partial = chromecast_ParseByteRange( &query, 1000, &start, &end );

    assert( !partial );
    assert( start == 0 );
    assert( end == 999 );
}

static void test_StartAfterEndServesWhole()
{
    httpd_message_t query = MakeQuery( "bytes=500-100" );
    uint64_t start, end;
    bool partial = chromecast_ParseByteRange( &query, 1000, &start, &end );

    assert( !partial );
    assert( start == 0 );
    assert( end == 999 );
}

static void test_MissingUnitPrefixServesWhole()
{
    httpd_message_t query = MakeQuery( "100-199" );
    uint64_t start, end;
    bool partial = chromecast_ParseByteRange( &query, 1000, &start, &end );

    assert( !partial );
    assert( start == 0 );
    assert( end == 999 );
}

static void test_NoDashServesWhole()
{
    httpd_message_t query = MakeQuery( "bytes=100" );
    uint64_t start, end;
    bool partial = chromecast_ParseByteRange( &query, 1000, &start, &end );

    assert( !partial );
    assert( start == 0 );
    assert( end == 999 );
}

int main(void)
{
    test_NoRangeHeaderServesWhole();
    test_SimpleRange();
    test_OpenEndedRangeClampsToResourceEnd();
    test_RangeEndPastResourceSizeIsClamped();
    test_ZeroToLastByte();
    test_StartAtOrPastResourceSizeServesWhole();
    test_StartAfterEndServesWhole();
    test_MissingUnitPrefixServesWhole();
    test_NoDashServesWhole();

    return 0;
}

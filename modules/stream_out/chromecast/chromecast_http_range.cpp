/*****************************************************************************
 * chromecast_http_range.cpp: parse an HTTP Range request header
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
# include "config.h"
#endif

#include "chromecast_http_range.h"

#include <cstdlib>
#include <cstring>

bool chromecast_ParseByteRange( const httpd_message_t *query, uint64_t i_size,
                                uint64_t *pi_start, uint64_t *pi_end )
{
    *pi_start = 0;
    *pi_end = i_size - 1;

    const char *psz_range = httpd_MsgGet( query, "Range" );
    if( psz_range == NULL || strncmp( psz_range, "bytes=", 6 ) )
        return false;

    const char *psz_dash = strchr( psz_range + 6, '-' );
    if( psz_dash == NULL )
        return false;

    uint64_t i_range_start = strtoull( psz_range + 6, NULL, 10 );
    uint64_t i_range_end = ( psz_dash[1] != '\0' )
        ? strtoull( psz_dash + 1, NULL, 10 ) : *pi_end;
    if( i_range_start >= i_size || i_range_start > i_range_end )
        return false;

    *pi_start = i_range_start;
    if( i_range_end < *pi_end )
        *pi_end = i_range_end;
    return true;
}

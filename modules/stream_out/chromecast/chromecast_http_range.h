/*****************************************************************************
 * chromecast_http_range.h: parse an HTTP Range request header
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

#ifndef VLC_CHROMECAST_HTTP_RANGE_H
#define VLC_CHROMECAST_HTTP_RANGE_H

#include <vlc_common.h>
#include <vlc_httpd.h>

/**
 * Parse the value of an HTTP "Range: bytes=start-end" request header
 * against a resource of the given size. Only a single byte range is
 * accepted (the common case, and the only one Chromecast receivers send);
 * anything else - no header, a malformed one, or one outside the resource
 * - is not an error, it just means the whole resource should be served,
 * exactly as HTTP requires when a range cannot be honored.
 *
 * \return true if the request asked for (and got) a sub-range of the
 *         resource (pi_start and pi_end hold it); false if the whole
 *         resource should be served instead (pi_start is set to 0,
 *         pi_end to i_size - 1).
 */
bool chromecast_ParseByteRange( const httpd_message_t *query, uint64_t i_size,
                                uint64_t *pi_start, uint64_t *pi_end );

#endif // VLC_CHROMECAST_HTTP_RANGE_H

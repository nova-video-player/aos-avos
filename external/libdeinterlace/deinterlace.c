/*****************************************************************************
 * "X" algorithm for vlc deinterlacer
 *****************************************************************************
 * Copyright (C) 2000-2011 VLC authors and VideoLAN
 * $Id: a42861043c2ceb2cd70ed0276fbdafb016c9b9f9 $
 *
 * Author: Laurent Aimar <fenrir@videolan.org>
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
 * Internal functions
 *****************************************************************************/

/* XDeint8x8Detect: detect if a 8x8 block is interlaced.
 * XXX: It need to access to 8x10
 * We use more than 8 lines to help with scrolling (text)
 * (and because XDeint8x8Frame use line 9)
 * XXX: smooth/uniform area with noise detection doesn't works well
 * but it's not really a problem because they don't have much details anyway
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef ARM_HAS_NEON
#include "neon_deinterlace.h"
static int use_neon_deinterlacing = 1;
#endif

static inline int ssd( int a ) { return a*a; }
static inline int XDeint8x8DetectC( uint8_t *src, int i_src )
{
    int y, x;
    int ff, fr;
    int fc;

    /* Detect interlacing */
    fc = 0;
    for( y = 0; y < 7; y += 2 )
    {
        ff = fr = 0;
#ifdef ARM_HAS_NEON
        if(use_neon_deinterlacing) {
            neon_XDeint8x8DetectC(src, i_src, &fr, &ff);
        } else
#endif
        {
            for( x = 0; x < 8; x++ )
            {
                fr += ssd(src[      x] - src[1*i_src+x]) +
                      ssd(src[i_src+x] - src[2*i_src+x]);
                ff += ssd(src[      x] - src[2*i_src+x]) +
                      ssd(src[i_src+x] - src[3*i_src+x]);
            }
        }
        if( ff < 6*fr/8 && fr > 32 )
            fc++;

        src += 2*i_src;
    }

    return fc < 1 ? 0 : 1;
}

static inline void XDeint8x8MergeC( uint8_t *dst,  int i_dst,
                                    uint8_t *src1, int i_src1,
                                    uint8_t *src2, int i_src2 )
{
    int y, x;

    /* Progressive */
    for( y = 0; y < 8; y += 2 )
    {
        memcpy( dst, src1, 8 );
        dst  += i_dst;

#ifdef ARM_HAS_NEON
        if(use_neon_deinterlacing) {
            neon_XDeint8x8MergeC(dst, src1, src2, i_src1);
        } else
#endif
        {
            for( x = 0; x < 8; x++ ) {
                dst[x] = (src1[x] + 6*src2[x] + src1[i_src1+x] + 4 ) >> 3;
	    }
	}
        dst += i_dst;

        src1 += i_src1;
        src2 += i_src2;
    }
}

/* XDeint8x8FieldE: Stupid deinterlacing (1,0,1) for block that miss a
 * neighbour
 * (Use 8x9 pixels)
 * TODO: a better one for the inner part.
 */
static inline void XDeint8x8FieldEC( uint8_t *dst, int i_dst,
                                     uint8_t *src, int i_src )
{
    int y, x;

    /* Interlaced */
    for( y = 0; y < 8; y += 2 )
    {
        memcpy( dst, src, 8 );
        dst += i_dst;
#ifdef ARM_HAS_NEON
        if(use_neon_deinterlacing) {
            neon_XDeint8x8FieldEC(dst, src, i_src);
        } else
#endif
        {
            for( x = 0; x < 8; x++ )
                dst[x] = (src[x] + src[2*i_src+x] ) >> 1;
	}
        dst += 1*i_dst;
        src += 2*i_src;
    }
}

/* XDeint8x8Field: Edge oriented interpolation
 */
static inline void XDeint8x8FieldC( uint8_t *dst, int i_dst,
                                    uint8_t *src, int i_src )
{
    int y, x;

    /* Interlaced */
    for( y = 0; y < 8; y += 2 )
    {
        memcpy( dst, src, 8 );
        dst += i_dst;

#ifdef ARM_HAS_NEON
        if(use_neon_deinterlacing) {
            uint8_t *src1 = &src[-4];
            uint8_t *src2 = &src[2*i_src - 4];
            neon_XDeint8x8FieldC(dst, src1, src2);
        } else
#endif
        {
            for( x = 0; x < 8; x++ )
            {
                uint8_t *src1 = &src[x-4];
                uint8_t *src2 = &src[x+2*i_src - 4];
                int c0 = 0;
                int c1 = 0;
                int c2 = 0;
#ifdef ARM_HAS_NEON
                if(use_neon_deinterlacing) {
                    c0 = neon_XDeint8x8FieldC_pix(dst, src1, &src2[2]);
                } else
#endif
                c0 = abs(src1[0]-src2[2])
                   + abs(src1[1]-src2[3])
                   + abs(src1[2]-src2[4])
                   + abs(src1[3]-src2[5]);
                   + abs(src1[4]-src2[6])
                   + abs(src1[5]-src2[7]);

#ifdef ARM_HAS_NEON
                if(use_neon_deinterlacing) {
                    c1 = neon_XDeint8x8FieldC_pix(dst, &src1[1], &src2[1]);
                } else
#endif
                c1 = abs(src1[1]-src2[1])
                   + abs(src1[2]-src2[2])
                   + abs(src1[3]-src2[3])
                   + abs(src1[4]-src2[4])
                   + abs(src1[5]-src2[5])
                   + abs(src1[6]-src2[6]);

#ifdef ARM_HAS_NEON
                if(use_neon_deinterlacing) {
                    c2 = neon_XDeint8x8FieldC_pix(dst, &src1[2], &src2[0]);
                } else
#endif
                c2 = abs(src1[2]-src2[0])
                   + abs(src1[3]-src2[1])
                   + abs(src1[4]-src2[2])
                   + abs(src1[5]-src2[3])
                   + abs(src1[6]-src2[4])
                   + abs(src1[7]-src2[5]);

                if( c0 < c1 && c1 <= c2 )
                    dst[x] = (src1[3] + src2[5]) >> 1;
                else if( c2 < c1 && c1 <= c0 )
                    dst[x] = (src1[5] + src2[3]) >> 1;
                else
                    dst[x] = (src1[4] + src2[4]) >> 1;
            }
	}
        dst += 1*i_dst;
        src += 2*i_src;
    }
}

/* NxN arbitray size (and then only use pixel in the NxN block)
 */
static inline int XDeintNxNDetect( uint8_t *src, int i_src,
                                   int i_height, int i_width )
{
    int y, x;
    int ff, fr;
    int fc;


    /* Detect interlacing */
    /* FIXME way too simple, need to be more like XDeint8x8Detect */
    ff = fr = 0;
    fc = 0;
    for( y = 0; y < i_height - 2; y += 2 )
    {
        const uint8_t *s = &src[y*i_src];
        for( x = 0; x < i_width; x++ )
        {
            fr += ssd(s[      x] - s[1*i_src+x]);
            ff += ssd(s[      x] - s[2*i_src+x]);
        }
        if( ff < fr && fr > i_width / 2 )
            fc++;
    }

    return fc < 2 ? 0 : 1;
}

static inline void XDeintNxNFrame( uint8_t *dst, int i_dst,
                                   uint8_t *src, int i_src,
                                   int i_width, int i_height )
{
    int y, x;

    /* Progressive */
    for( y = 0; y < i_height; y += 2 )
    {
        memcpy( dst, src, i_width );
        dst += i_dst;

        if( y < i_height - 2 )
        {
            for( x = 0; x < i_width; x++ )
                dst[x] = (src[x] + 2*src[1*i_src+x] + src[2*i_src+x] + 2 ) >> 2;
        }
        else
        {
            /* Blend last line */
            for( x = 0; x < i_width; x++ )
                dst[x] = (src[x] + src[1*i_src+x] ) >> 1;
        }
        dst += 1*i_dst;
        src += 2*i_src;
    }
}

static inline void XDeintNxNField( uint8_t *dst, int i_dst,
                                   uint8_t *src, int i_src,
                                   int i_width, int i_height )
{
    int y, x;

    /* Interlaced */
    for( y = 0; y < i_height; y += 2 )
    {
        memcpy( dst, src, i_width );
        dst += i_dst;

        if( y < i_height - 2 )
        {
            for( x = 0; x < i_width; x++ )
                dst[x] = (src[x] + src[2*i_src+x] ) >> 1;
        }
        else
        {
            /* Blend last line */
            for( x = 0; x < i_width; x++ )
                dst[x] = (src[x] + src[i_src+x]) >> 1;
        }
        dst += 1*i_dst;
        src += 2*i_src;
    }
}

static inline void XDeintNxN( uint8_t *dst, int i_dst, uint8_t *src, int i_src,
                              int i_width, int i_height )
{
    if( XDeintNxNDetect( src, i_src, i_width, i_height ) )
        XDeintNxNField( dst, i_dst, src, i_src, i_width, i_height );
    else
        XDeintNxNFrame( dst, i_dst, src, i_src, i_width, i_height );
}

static inline int median( int a, int b, int c )
{
    int min = a, max =a;
    if( b < min )
        min = b;
    else
        max = b;

    if( c < min )
        min = c;
    else if( c > max )
        max = c;

    return a + b + c - min - max;
}


/* XDeintBand8x8:
 */
static inline void XDeintBand8x8C( uint8_t *dst, int i_dst,
                                   uint8_t *src, int i_src,
                                   const int i_mbx, int i_modx )
{
    int x;

    for( x = 0; x < i_mbx; x++ )
    {
        int s;
        if( ( s = XDeint8x8DetectC( src, i_src ) ) )
        {
            if( x == 0 || x == i_mbx - 1 )
                XDeint8x8FieldEC( dst, i_dst, src, i_src );
            else
                XDeint8x8FieldC( dst, i_dst, src, i_src );
        }
        else
        {
            XDeint8x8MergeC( dst, i_dst,
                             &src[0*i_src], 2*i_src,
                             &src[1*i_src], 2*i_src );
        }

        dst += 8;
        src += 8;
    }

    if( i_modx )
        XDeintNxN( dst, i_dst, src, i_src, i_modx, 8 );
}

/*****************************************************************************
 * Public functions
 *****************************************************************************/

void RenderX( unsigned char *p_outpic, unsigned char *p_pic, int width, int height, int dst_linesize, int src_linesize )
{
        const int i_mby = ( height + 7 )/8 - 1;
        const int i_mbx = width/8;

        const int i_mody = height - 8*i_mby;
        const int i_modx = width - 8*i_mbx;

        const int i_dst = dst_linesize;
        const int i_src = src_linesize;

        int y, x;

        for( y = 0; y < i_mby; y++ )
        {
            uint8_t *dst = &p_outpic[8*y*i_dst];
            uint8_t *src = &p_pic[8*y*i_src];

                XDeintBand8x8C( dst, i_dst, src, i_src, i_mbx, i_modx );
        }

        /* Last line (C only)*/
        if( i_mody )
        {
            uint8_t *dst = &p_outpic[8*y*i_dst];
            uint8_t *src = &p_pic[8*y*i_src];

            for( x = 0; x < i_mbx; x++ )
            {
                XDeintNxN( dst, i_dst, src, i_src, 8, i_mody );

                dst += 8;
                src += 8;
            }

            if( i_modx )
                XDeintNxN( dst, i_dst, src, i_src, i_modx, i_mody );
        }

}

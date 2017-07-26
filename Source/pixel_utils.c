/*
 * Copyright 2017 Archos SA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "global.h"
#include "composer.h"
#include "color.h"
#include "osd.h"
#include "fb.h"
#include "atime.h"

#include <string.h>

#ifndef CONFIG_NEON
// ************************************
//
//	_arm_copy_aligned_rect_rgba32_rotated
//
// ************************************
static void _arm_copy_aligned_rect_rgba32_rotated( PaintSurface *tgt, PaintSurface *src, rect *area, int alpha )
{
	if( alpha == ALPHA_OPAQUE ) {
		//
		// fast part, do 16 horizontal lines at a time
		//
		int y;
		for ( y = area->y; y < area->y + (area->height & ~0x0F); y += 16 ) {
			rgba *pline_src0 = ((rgba*) src->color_buf) + y * src->color_ppr + area->x;
			rgba *pline_src1 = pline_src0 + src->color_ppr;
			rgba *pline_src2 = pline_src1 + src->color_ppr;
			rgba *pline_src3 = pline_src2 + src->color_ppr;
			rgba *pline_src4 = pline_src3 + src->color_ppr;
			rgba *pline_src5 = pline_src4 + src->color_ppr;
			rgba *pline_src6 = pline_src5 + src->color_ppr;
			rgba *pline_src7 = pline_src6 + src->color_ppr;
			rgba *pline_src8 = pline_src7 + src->color_ppr;
			rgba *pline_src9 = pline_src8 + src->color_ppr;
			rgba *pline_srca = pline_src9 + src->color_ppr;
			rgba *pline_srcb = pline_srca + src->color_ppr;
			rgba *pline_srcc = pline_srcb + src->color_ppr;
			rgba *pline_srcd = pline_srcc + src->color_ppr;
			rgba *pline_srce = pline_srcd + src->color_ppr;
			rgba *pline_srcf = pline_srce + src->color_ppr;

			rgba *pline_tgt  = ((rgba*) tgt->color_buf) + (tgt->bounds.width - area->x - 1) * tgt->color_ppr + y;
			int i;
			for( i = 0 ; i < area->width ; i++ ){
				*pline_tgt++ = *pline_src0++;
				*pline_tgt++ = *pline_src1++;
				*pline_tgt++ = *pline_src2++;
				*pline_tgt++ = *pline_src3++;
				*pline_tgt++ = *pline_src4++;
				*pline_tgt++ = *pline_src5++;
				*pline_tgt++ = *pline_src6++;
				*pline_tgt++ = *pline_src7++;

				*pline_tgt++ = *pline_src8++;
				*pline_tgt++ = *pline_src9++;
				*pline_tgt++ = *pline_srca++;
				*pline_tgt++ = *pline_srcb++;
				*pline_tgt++ = *pline_srcc++;
				*pline_tgt++ = *pline_srcd++;
				*pline_tgt++ = *pline_srce++;
				*pline_tgt++ = *pline_srcf++;

				pline_tgt -= 16;
				pline_tgt -= tgt->color_ppr;
			}
		}
		// do the remaining lines
		for ( ; y < area->y + area->height; y ++ ) {
			rgba *pline_src1 = ((rgba*) src->color_buf) + y * src->color_ppr + area->x;
			rgba *pline_tgt  = ((rgba*) tgt->color_buf) + (tgt->bounds.width - area->x - 1) * tgt->color_ppr + y;
			int i;
			for( i = 0 ; i < area->width ; i++ ){
				*pline_tgt = *pline_src1++;
				pline_tgt -= tgt->color_ppr;
			}
		}		
	} else {
		const int d = 255 - alpha;
		int y;
		for ( y = area->y; y < area->y + area->height; y ++ ) {
			rgba *pline_src = ((rgba*) src->color_buf) + y * src->color_ppr + area->x;
			rgba *pline_tgt = ((rgba*) tgt->color_buf) + (tgt->bounds.width - area->x - 1) * tgt->color_ppr + y;
			int i;
			for( i = 0 ; i < area->width ; i++ ){
				rgba color = *pline_src++;
				color.a    = max( color.a - d, 0 );
				*pline_tgt = color;
				pline_tgt -= tgt->color_ppr;
			}
		}
	}
}

// ************************************
//
//	_arm_copy_aligned_rect_rgba32
//	Blits an rectangular region from PaintSurface src to PaintSurface tgt.
//	(Only used internally by Composer_blit() .)
//	the alpha values will be reduced by 255 - (alpha << 5) so that we can fade the osd this way
//
// ************************************
static void _arm_copy_aligned_rect_rgba32( PaintSurface *tgt, PaintSurface *src, rect *area, int alpha )
{
	rgba *pline_src = ((rgba*) src->color_buf) + area->y * src->color_ppr + area->x;
	rgba *pline_tgt = ((rgba*) tgt->color_buf) + area->y * tgt->color_ppr + area->x;

	const int d = 255 - alpha;
	int y_end = area->y + area->height;

	int y;
	for ( y = area->y; y < y_end; y++ ) {	
		if( alpha == ALPHA_OPAQUE ) {
			memcpy( pline_tgt, pline_src, area->width * sizeof( rgba ) );
		} else {
			int i;
			for( i = 0 ; i < area->width ; i++ ){
				rgba color = pline_src[i];
				color.a = max( color.a - d, 0 );
				pline_tgt[i] = color;
			}
		}
		pline_tgt += tgt->color_ppr;
		pline_src += src->color_ppr;
	}
}

// ******************************************************************
//
//	_arm_copy_yuv_image_rotated
//
// ******************************************************************
static void _arm_copy_yuv_image_rotated( int xpos, int ypos, const IMAGE *src, const IMAGE *dst, int fx_mask )
{
	int darker = fx_mask & 0x1;
	
	rect bounds = {    0,    0, dst->width, dst->height };
	rect area   = { xpos, ypos, src->width, src->height };

	area = Rect_Intersection( &area, &bounds );

	int y;
	for( y = 0; y < area.height ; y += 4 ){
		UINT *src0 = (UINT*)PIXELPTR( src, area.x - xpos, y + 0 + area.y - ypos );
		UINT *src1 = (UINT*)PIXELPTR( src, area.x - xpos, y + 1 + area.y - ypos );
		
		UINT *src2 = (UINT*)PIXELPTR( src, area.x - xpos, y + 2 + area.y - ypos );
		UINT *src3 = (UINT*)PIXELPTR( src, area.x - xpos, y + 3 + area.y - ypos );

		int x;
		for( x = 0 ; x < area.width ; x += 2 ){

			UINT U;
			UINT V;
			
			INT *dst0 = (UINT*)PIXELPTR( dst, area.y + y, area.width - (area.x + x + 0) - 1 );
			INT *dst1 = (UINT*)PIXELPTR( dst, area.y + y, area.width - (area.x + x + 1) - 1 );

			UINT U0  = *src0++;
			int  Y01 = (U0 >>  8) & 0xFF;
			UINT V0  = (U0 >> 16) & 0xFF;
			int  Y02 = (U0 >> 24) & 0xFF;
			     U0 &= 0xFF;
			
			UINT U1  = *src1++;
			int  Y11 = (U1 >>  8) & 0xFF;
			UINT V1  = (U1 >> 16) & 0xFF;
			int  Y12 = (U1 >> 24) & 0xFF;
			     U1 &= 0xFF;
			
			UINT U2  = *src2++;
			int  Y21 = (U2 >>  8) & 0xFF;
			UINT V2  = (U2 >> 16) & 0xFF;
			int  Y22 = (U2 >> 24) & 0xFF;
			     U2 &= 0xFF;
			
			UINT U3  = *src3++;
			int  Y31 = (U3 >>  8) & 0xFF;
			UINT V3  = (U3 >> 16) & 0xFF;
			int  Y32 = (U3 >> 24) & 0xFF;
			     U3 &= 0xFF;

			U = (U0 + U1) >> 1;
			V = (V0 + V1) >> 1;

			if( darker ) {
				Y01 -= 80; if( Y01 < 0 ) { Y01 = 0; }
				Y02 -= 80; if( Y02 < 0 ) { Y02 = 0; }
				Y11 -= 80; if( Y11 < 0 ) { Y11 = 0; }
				Y12 -= 80; if( Y12 < 0 ) { Y12 = 0; }
			}
			
			*dst0++ = U | (Y01 << 8) | (V << 16) | (Y11 << 24);
			*dst1++ = U | (Y02 << 8) | (V << 16) | (Y12 << 24);
			
			U = (U2 + U3) >> 1;
			V = (V2 + V3) >> 1;

			if( darker ) {
				Y21 -= 80; if( Y21 < 0 ) { Y21 = 0; }
				Y22 -= 80; if( Y22 < 0 ) { Y22 = 0; }
				Y31 -= 80; if( Y31 < 0 ) { Y31 = 0; }
				Y32 -= 80; if( Y32 < 0 ) { Y32 = 0; }
			}
			
			*dst0++ = U | (Y21 << 8) | (V << 16) | (Y31 << 24);
			*dst1++ = U | (Y22 << 8) | (V << 16) | (Y32 << 24);
		}
#ifdef CONFIG_FB_QVFB
// we draw the rotated wallpaper directly in the video buffer, so make that visible
// on the SIM by updating the QVFB while we go along.
if( !(y % 0x10) ) {
	msec_sleep( 1 );
	FB_update( FB_UPDATE_VID );
}
#endif
	}
}

void copy_yuv_image_rotated( int xpos, int ypos, const IMAGE *src, const IMAGE *dst, int fx_mask ) 
{
	return _arm_copy_yuv_image_rotated( xpos, ypos, src, dst, fx_mask );
}

SurfaceTool surface_tool = {
	.copy_aligned_rect_rgba32_rotated = _arm_copy_aligned_rect_rgba32_rotated,
	.copy_aligned_rect_rgba32         = _arm_copy_aligned_rect_rgba32,
	.copy_yuv_image_rotated           = _arm_copy_yuv_image_rotated,
};
#endif

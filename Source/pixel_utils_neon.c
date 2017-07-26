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
#include <string.h>

#include "neon.h"

#ifdef CONFIG_NEON
// ************************************
//
//	_neon_copy_aligned_rect_rgba32_rotated
//
// ************************************
static void _neon_copy_aligned_rect_rgba32_rotated( PaintSurface *tgt, PaintSurface *src, rect *area, int alpha )
{
	int y = 0;
	const int d = 255 - alpha;
	rgba *pline_src0 = ((rgba*) src->color_buf) + area->y * src->color_ppr + area->x;
	rgba *pline_tgt  = ((rgba*) tgt->color_buf) + (tgt->bounds.width - area->x - 1) * tgt->color_ppr + area->y;
	neon_copy_aligned32_rotated_CW270(pline_src0, pline_tgt,area->height, area->width, src->color_ppr*4, tgt->color_ppr*4, alpha);
	/* Process remaining line*/
	int newY = area->y + (area->height - area->height%2);
	for ( y = newY; y < area->y + area->height; y ++ ) {
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
	/* Process remaining column*/
	int newX = area->x + (area->width - area->width%2);
	for ( y = area->y; y < area->y + area->height; y ++ ) {
			rgba *pline_src = ((rgba*) src->color_buf) + y * src->color_ppr + newX;
			rgba *pline_tgt = ((rgba*) tgt->color_buf) + (tgt->bounds.width - newX - 1) * tgt->color_ppr + y;
			int i;
			for( i = 0 ; i < area->width%2 ; i++ ){
				rgba color = *pline_src++;
				color.a    = max( color.a - d, 0 );
				*pline_tgt = color;
				pline_tgt -= tgt->color_ppr;
			}
		}
}

// ************************************
//
//	_neon_copy_aligned_rect_rgba32
//	Blits an rectangular region from PaintSurface src to PaintSurface tgt.
//	(Only used internally by Composer_blit() .)
//	the alpha values will be reduced by 255 - (alpha << 5) so that we can fade the osd this way
//
// ************************************
static void _neon_copy_aligned_rect_rgba32( PaintSurface *tgt, PaintSurface *src, rect *area, int alpha )
{
	rgba *pline_src = ((rgba*) src->color_buf) + area->y * src->color_ppr + area->x;
	rgba *pline_tgt = ((rgba*) tgt->color_buf) + area->y * tgt->color_ppr + area->x;

	int y_end = area->y + area->height;

	int y;
	for ( y = area->y; y < y_end; y++ ) {	
		if( alpha == ALPHA_OPAQUE ) {
			memcpy( pline_tgt, pline_src, area->width * sizeof( rgba ) );
		} else {
			neon_copy_aligned32( pline_tgt, pline_src, area->width * sizeof( rgba ), alpha );
		}
		pline_tgt += tgt->color_ppr;
		pline_src += src->color_ppr;
	}
}

// ******************************************************************
//
//	_neon_copy_yuv_image_rotated
//
// ******************************************************************
static void _neon_copy_yuv_image_rotated( int xpos, int ypos, const IMAGE *src, const IMAGE *dst, int fx_mask )
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
			INT *dst0 = (UINT*)PIXELPTR( dst, area.y + y, area.width - (area.x + x + 0) - 1 );
			INT *dst1 = (UINT*)PIXELPTR( dst, area.y + y, area.width - (area.x + x + 1) - 1 );
			neon_copy_yuv_rotated_4(src0, src1, src2, src3, dst0, dst1, darker);
			src0++;
			src1++;
			src2++;
			src3++;
			dst0+=2;
			dst1+=2;
		}
	}
}

void copy_yuv_image_rotated( int xpos, int ypos, const IMAGE *src, const IMAGE *dst, int fx_mask ) 
{
	return _neon_copy_yuv_image_rotated( xpos, ypos, src, dst, fx_mask );
}

SurfaceTool surface_tool = {
	.copy_aligned_rect_rgba32_rotated = _neon_copy_aligned_rect_rgba32_rotated,
	.copy_aligned_rect_rgba32         = _neon_copy_aligned_rect_rgba32,
	.copy_yuv_image_rotated           = _neon_copy_yuv_image_rotated,
};
#endif

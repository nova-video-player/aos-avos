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

// ****************************************************
//
// 	YUV 422 image manipulation functions
//
// ****************************************************

#include "global.h"
#include "types.h"
#include "debug.h"
#include "file.h"
#include "astdlib.h"
#include "image.h"
#include "av.h"
#include "browse.h"
#include "atime.h"
#include "util.h"
#include "file_type.h"
#include "stream_config.h"

#include <string.h>
#include <stdio.h>

#define DBG  if(Debug[DBG_IMG])
#define DBG2 if(Debug[DBG_IMG] > 1)
#define ERR  if(1)

// ****************************************************
//
//	_to_yuv
//
// ****************************************************
static inline UINT16 _to_yuv( UINT16 *in_ptr, int in_x, int out_x )
{
	if( out_x % 2 ){
		if( in_x % 2 )
			return *in_ptr;
		else
			return (( *in_ptr & 0xff00 ) | (*(in_ptr + 1 ) & 0xff ));
	} else {
		if( in_x % 2 )
			return (( *in_ptr & 0xff00 ) | (*(in_ptr + 1 ) & 0xff ));
		else
			return *in_ptr;
	}
}

// ****************************************************
//
//    yuv_to_rgb16
//
// ****************************************************
UINT16 yuv_to_rgb16( UCHAR y, UCHAR u, UCHAR v )
{
	int ub = (int)u - 128;
	int vb = (int)v - 128;

	unsigned int r = MAX(MIN(( 256 * (int)y + 359 * (int)vb ) >> 8, 255 ), 0);
	unsigned int g = MAX(MIN(( 256 * (int)y -  88 * (int)ub - 182 * (int)vb ) >> 8, 255 ), 0);
	unsigned int b = MAX(MIN(( 256 * (int)y + 453 * (int)ub ) >> 8, 255 ), 0);

	return( (r >> 3) << 11
 		|(g >> 2) << 5
		|(b >> 3));
}

// ****************************************************
//
//	yuv_to_rgba32
//
// ****************************************************
UINT32 yuv_to_rgba32( UCHAR y, UCHAR u, UCHAR v )
{
	int ub = (int)u - 128;
	int vb = (int)v - 128;

	unsigned int r = MAX(MIN(( 256 * (int)y + 359 * (int)vb ) >> 8, 255 ), 0);
	unsigned int g = MAX(MIN(( 256 * (int)y -  88 * (int)ub - 182 * (int)vb ) >> 8, 255 ), 0);
	unsigned int b = MAX(MIN(( 256 * (int)y + 453 * (int)ub ) >> 8, 255 ), 0);

	return (255 << 24) | (r << 16) | (g << 8 ) | (b << 0);
}

// ****************************************************
//
//	_to_rgb16
//
// ****************************************************
static inline UINT16 _to_rgb16( UINT16 *in_ptr, int in_x, int out_x )
{
	UCHAR y, u, v;

	y = (( *in_ptr & 0xff00 ) >> 8 );

	if( in_x % 2 ){
		v = ( *in_ptr & 0x00ff );
		u = (*(in_ptr + 1 ) & 0xff );
	} else {
		u = ( *in_ptr & 0x00ff );
		v = (*(in_ptr + 1 ) & 0xff );
	}

	return yuv_to_rgb16( y, u, v );
}

// ****************************************************
//
//	_to_rgb24
//
// ****************************************************
static inline void _to_rgb24( USHORT *in_ptr, UCHAR *out_ptr, int in_x, int out_x )
{
	UCHAR y, u, v;

	y = (( *in_ptr & 0xff00 ) >> 8 );

	if( in_x % 2 ){
		v = ( *in_ptr & 0x00ff );
		u = (*(in_ptr + 1 ) & 0xff );
	} else {
		u = ( *in_ptr & 0x00ff );
		v = (*(in_ptr + 1 ) & 0xff );
	}

	int ub = (int)u - 128;
	int vb = (int)v - 128;

	*out_ptr = MAX(MIN(( 256 * (int)y + 359 * (int)vb ) >> 8, 255 ), 0);
	*(out_ptr + 1 ) = MAX(MIN(( 256 * (int)y -  88 * (int)ub - 182 * (int)vb ) >> 8, 255 ), 0);
	*(out_ptr + 2 ) = MAX(MIN(( 256 * (int)y + 453 * (int)ub ) >> 8, 255 ), 0);
}

// ****************************************************
//
//	_bytes_per_pixel
//
// ****************************************************
static int _bytes_per_pixel( int colorspace )
{
	switch( colorspace ) {
	case AV_IMAGE_GRAYSCALE:
	case AV_IMAGE_8BIT_INDEXED:
		return 1;
	case AV_IMAGE_RGB_24:
		return 3;
	case AV_IMAGE_RGBA_32:
	case AV_IMAGE_BGRA_32:
	case AV_IMAGE_ARGB_32:
		return 4;
	default:
		return 2;
	}
}

// ****************************************************
//
//	_alloc
//
//	Allocates an image of size width * height
//
// ****************************************************
static IMAGE *_alloc( int width, int height, int type, int colorspace )
{
	IMAGE *img;

	if( !(img = acalloc( 1, sizeof( IMAGE ) )) )
		return NULL;

	img->colorspace = colorspace;

	if( colorspace == AV_IMAGE_8BIT_INDEXED ) {
		if( !(img->palette = amalloc( 256 * sizeof( USHORT ) ) ) ) {
			afree( img );
			return NULL;
		}
	}
		
	img->bpp[0] = _bytes_per_pixel( colorspace );

	int linestep = image_linestep_from_width( width, img->bpp[0] );

	// Geometry
	img->width    = width;
	img->height   = height;
	img->linestep[0] = linestep;
	img->size     = linestep * height + linestep;

	img->mem_type   = type;
	if( img->mem_type == IMAGE_DMA )
		img->buffer = amalloc_dma( img->size );
	else if(  img->mem_type == IMAGE_DMA_CACHED )
		img->buffer = amalloc_dma_cached( img->size );
	else
		img->buffer = amalloc( img->size );

	img->data[0] = img->buffer;

	if ( !img->buffer ) {
		afree(img);
		return NULL;
	}

	return img;
}

// ****************************************************
//
//	image_alloc
//
// ****************************************************
IMAGE *image_alloc( int width, int height, int colorspace )
{
	return _alloc( width, height, IMAGE_DMA, colorspace );
}

// ****************************************************
//
//	image_alloc_normal
//
// ****************************************************
IMAGE *image_alloc_normal( int width, int height, int colorspace )
{
	return _alloc( width, height, IMAGE_NRM, colorspace );
}

// ****************************************************
//
//	image_alloc_cached
//
// ****************************************************
IMAGE *image_alloc_cached( int width, int height, int colorspace )
{
	return _alloc( width, height, IMAGE_DMA_CACHED, colorspace );
}

// ****************************************************
//
//	image_alloc_duplicate
//
// ****************************************************
IMAGE *image_alloc_duplicate(IMAGE *src) 
{
	if (!src)
		return NULL;

	IMAGE *img;

	if( !(img = acalloc( 1, sizeof( IMAGE ) )) )
		return NULL;

	img->mem_type   = src->mem_type;
	img->size	= src->size;

	if( src->colorspace == AV_IMAGE_8BIT_INDEXED ) {
		if( !(img->palette = amalloc( 256 * sizeof( USHORT ) ) ) ) {
			afree( img );
			return NULL;
		}
		memcpy(img->palette,src->palette, 256 * sizeof( USHORT ));
	}

	if( img->mem_type == IMAGE_DMA )
		img->buffer = amalloc_dma( img->size );
	else if(  img->mem_type == IMAGE_DMA_CACHED )
		img->buffer = amalloc_dma_cached( img->size );
	else
		img->buffer = amalloc( img->size );

	img->data[0] = img->buffer;

	if ( !img->buffer ) {
		afree(img);
		return NULL;
	}
	
	memcpy(img->data[0], src->data[0], img->size);

	img->colorspace = src->colorspace;	
	img->bpp[0]     = src->bpp[0];

	img->linestep[0] = src->linestep[0];

	// Geometry
	img->width    = src->width;
	img->height   = src->height;

	return img;	
}

// ****************************************************
//
//	image_free
//
// ****************************************************
void image_free( IMAGE *img )
{
	if ( !img )
		return;

	if( img->palette ) {
		afree( img->palette );
	} 
	if( img->buffer ) {
		if( img->mem_type == IMAGE_DMA ) {
			afree_dma( img->buffer, img->size );
		} else if( img->mem_type == IMAGE_NRM ){
			afree( img->buffer );
		} else if( img->mem_type == IMAGE_DMA_CACHED ){
			afree_dma_cached( img->buffer, img->size );
		} else {
serprintf("image_free: unknow type\n");
		}
	}

	afree( img );
}

// *****************************************************************************
//
//	image_crop
//
// *****************************************************************************
IMAGE image_crop( const IMAGE *src, rect *area )
{
	IMAGE image = *src;

	rect bounds = { 0, 0, src->width, src->height };

	*area = Rect_Intersection( area, &bounds );

	image.data[0] = PIXELPTR( src, area->x, area->y );
	image.width   = area->width;
	image.height  = area->height;
	image.size   -= area->y * src->linestep[0] + area->x * src->bpp[0];

	return image;
}

// *****************************************************************************
//
//	rgb24_to_yu
//
// *****************************************************************************
USHORT rgb24_to_yu( UCHAR r, UCHAR g, UCHAR b )
{
	USHORT YU;

	YU = (   	77 * r + 150 * g + 29 * b ) & 0xFF00;	// Y
	YU |= ( 32768 - 43 * r - 85 * g + 128 * b ) >> 8;	// U

	return YU;
}

// *****************************************************************************
//
//	rgb24_to_yv
//
// *****************************************************************************
USHORT rgb24_to_yv( UCHAR r, UCHAR g, UCHAR b )
{
	USHORT YV;

	YV = (   	 77 * r + 150 * g +  29 * b ) & 0xFF00;	// Y
	YV |= ( 32768 + 128 * r - 107 * g -  21 * b ) >> 8;	// V

	return YV;
}

// *****************************************************************************
//
//	rgb24_to_yuv422
//
// *****************************************************************************
int rgb24_to_yuv422( UCHAR r, UCHAR g, UCHAR b )
{
	int  y =          77 * r + 150 * g +  29 * b;
	int  u = 32768 -  43 * r -  85 * g + 128 * b;
	int  v = 32768 + 128 * r - 107 * g -  21 * b;

	return(
		((y & 0xFF00) << 16) |
		((v & 0xFF00) <<  8) |
		((y & 0xFF00)      ) |
		((u & 0xFF00) >>  8)
	);
}

// *****************************************************************************
//
//	rgb16_to_yu
//
// *****************************************************************************
USHORT rgb16_to_yu( USHORT rgb16 )
{
	UCHAR r, g, b;
	r = (UCHAR)(( rgb16 >> 11 ) << 3);
	g = (UCHAR)((( rgb16 >> 5 ) & 0x3F ) << 2 );
	b = (UCHAR)(( rgb16 & 0x1F ) << 3 );

	return rgb24_to_yu( r, g, b );
}

// *****************************************************************************
//
//	rgb16_to_yv
//
// *****************************************************************************
USHORT rgb16_to_yv( USHORT rgb16 )
{
	UCHAR r, g, b;
	r = (UCHAR)(( rgb16 >> 11 ) << 3);
	g = (UCHAR)((( rgb16 >> 5 ) & 0x3F ) << 2 );
	b = (UCHAR)(( rgb16 & 0x1F ) << 3 );

	return rgb24_to_yv( r, g, b );

}

// *****************************************************************************
//
//	image_fill_window
//
// *****************************************************************************
void image_fill_window( IMAGE *img, int yuv_color )
{

	if( img->window.x & 0x1 ){
ERR serprintf("IMG: image_fill_window, please use even coordinates\r\n");
		return;
	}

	if( img->window.width & 0x1 ){
ERR serprintf("IMG: image_fill_window, warning, last pixel ignored\r\n");
	}

	int j;
	for( j = 0 ; j < img->window.height ; j++ ){
		INT32 *ptr = (UINT32*)(PIXELPTR(img, img->window.x, img->window.y + j));
		memset32( ptr, yuv_color, img->window.width / 2);
	}
}

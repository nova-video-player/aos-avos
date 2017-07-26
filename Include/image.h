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

#ifndef _IMAGE_H
#define _IMAGE_H

#include "av.h"

#define image_linestep_from_width( width, bpp )	( 32 * ( ( width * bpp  + 31 ) / 32 ) )

#define PIXELPTR( img, x, y ) \
	((( img )->data[0]  ) + (y) * (img)->linestep[0] + ((img)->bpp[0]) * (x) )

#define PIXELPTR_N( img, n, x, y ) \
	((( img )->data[n]  ) + (y) * (img)->linestep[n] + ((img)->bpp[n]) * (x) )

#define FB_VIDEO_IMAGE { \
		.colorspace  = AV_IMAGE_YUV_422, \
		.bpp[0]      = 2, \
		.data[0]     = FB_get_video_data(), \
		.linestep[0] = FB_get_video_linestep(), \
		.width       = FB_get_video_width(), \
		.height      = FB_get_video_height(), \
		.size        = 2 * FB_get_video_width() * FB_get_video_height(), \
		} 

#define	YUV_BLACK	0x00800080
#define	YUV_GRAY	0x80808080
#define	YUV_LIGHT_GRAY	0xA880A880
#define YUV_WHITE	0xff80ff80
#define YUV_BLUE	0x606060b0
#define YUV_RED		0x60ff6085
#define YUV_GREEN	0x60216043

#define SCALE_BASE	512 * 1024

IMAGE *image_alloc          ( int width, int height, int colorspace );
IMAGE *image_alloc_normal   ( int width, int height, int colorspace );
IMAGE *image_alloc_cached(    int width, int height, int colorspace );

IMAGE *image_alloc_duplicate( IMAGE *src );

IMAGE  image_crop( const IMAGE *src, rect *area );

void image_free( IMAGE *img );

enum {
	RSZ_ASYNC = 0,
	RSZ_SYNC
};

enum {
	RSZ_ASAP = 0,
	RSZ_VBL
};

enum {
	RSZ_NORMAL = 0,
	RSZ_BLUR,
	RSZ_NEAREST_NEIGHBOUR,
	RSZ_HORIZONTAL_BLUR
};

typedef enum {
	IMAGE_NRM = 0,
	IMAGE_DMA,
	IMAGE_DMA_CACHED
} ImageMemoryType;

// standard resize: uses a blackman window and a sinus cardinal filter
void image_resize( const IMAGE *src_img, IMAGE *dst_img );

// SIM resize: without hardware acceleration, also without alignement limitations
void image_software_resize( const IMAGE *src_img, IMAGE *dst_img );

USHORT rgb24_to_yu( UCHAR r, UCHAR g, UCHAR b );
USHORT rgb24_to_yv( UCHAR r, UCHAR g, UCHAR b );
int    rgb24_to_yuv422( UCHAR r, UCHAR g, UCHAR b );

USHORT rgb16_to_yu( USHORT rgb16 );
USHORT rgb16_to_yv( USHORT rgb16 );

UINT16 yuv_to_rgb16( UCHAR y, UCHAR u, UCHAR v );
UINT32 yuv_to_rgba32( UCHAR y, UCHAR u, UCHAR v );

void image_fill_window( IMAGE *img, int yuv_color );

static inline void image_full_window( IMAGE * img ){
	img->window.x = 0;
	img->window.y = 0;
	img->window.width  = img->width;
	img->window.height = img->height;
}

#endif // _IMAGE_H

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
#include "types.h"
#include "debug.h"
#include "util.h"
#include "stream.h"
#include "codec_utils.h"
#include "av.h"

#ifdef CONFIG_LIBYUV
#include "libyuv.h"
#endif

#define DBG if(0)

extern void RenderX( unsigned char *p_outpic, unsigned char *p_pic, int width, int height, int dst_linesize, int src_linesize );

#ifdef CONFIG_NEON
#include "neon.h"
static int use_neon = 1;
DECLARE_DEBUG_TOGGLE("neon", use_neon );
static int neon_swap = 0;
DECLARE_DEBUG_TOGGLE("neons", neon_swap );
#endif

#ifdef DEBUG_MSG
static int deinterlace_perf = 0;
static int total_time = 0;
static int total_byte = 0;

static int get_time()
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void toggle_deinterlace_perf_report(void)
{
	if (!deinterlace_perf) {
		total_time = total_byte = 0;
		deinterlace_perf = 1;
	}
	else deinterlace_perf = 0;
}
DECLARE_DEBUG_COMMAND_VOID("deintp", toggle_deinterlace_perf_report);
#endif

/**
 * Converts YUV420 NV21 to RGB8888
 * 
 * @param data byte array on YUV420 NV21 format.
 * @param width pixels width
 * @param height pixels height
 * @return a RGB8888 pixels int array. Where each int is a pixels ARGB. 
 */

static int convertYUVtoBGRA32( int y, int u, int v )
{
	int r, g, b;

	r = y + ( int )   1.13983 *v;
	g = y - ( int ) ( 0.39465 * u + 0.58060 * v );
	b = y + ( int )   2.03211 *u;
	r = r > 255 ? 255 : r < 0 ? 0 : r;
	g = g > 255 ? 255 : g < 0 ? 0 : g;
	b = b > 255 ? 255 : b < 0 ? 0 : b;
	return ( 0xff << 24 ) | ( r << 16 ) | ( g << 8 ) | b;
}

static int convertYUVtoRGBX32( int y, int u, int v )
{
	int r, g, b;

	r = y + ( int )   1.13983 *v;
	g = y - ( int ) ( 0.39465 * u + 0.58060 * v );
	b = y + ( int )   2.03211 *u;
	r = r > 255 ? 255 : r < 0 ? 0 : r;
	g = g > 255 ? 255 : g < 0 ? 0 : g;
	b = b > 255 ? 255 : b < 0 ? 0 : b;
	return ( b << 16 ) | ( g << 8 ) | r;
}

static int convertYUV10btoBGRA32( int y, int u, int v )
{
	int r, g, b;

	r = (y + ( int )   1.13983 *v) / 4;
	g = (y - ( int ) ( 0.39465 * u + 0.58060 * v )) / 4;
	b = (y + ( int )   2.03211 *u) / 4;
	r = r > 255 ? 255 : r < 0 ? 0 : r;
	g = g > 255 ? 255 : g < 0 ? 0 : g;
	b = b > 255 ? 255 : b < 0 ? 0 : b;
	return ( 0xff << 24 ) | ( r << 16 ) | ( g << 8 ) | b;
}

static int convertYUV10btoRGBX32( int y, int u, int v )
{
	int r, g, b;

	r = (y + ( int )   1.13983 *v) / 4;
	g = (y - ( int ) ( 0.39465 * u + 0.58060 * v )) / 4;
	b = (y + ( int )   2.03211 *u) / 4;
	r = r > 255 ? 255 : r < 0 ? 0 : r;
	g = g > 255 ? 255 : g < 0 ? 0 : g;
	b = b > 255 ? 255 : b < 0 ? 0 : b;
	return ( b << 16 ) | ( g << 8 ) | r;
}

__attribute__((unused))
static void convert_420P_to_RGB( int colorspace, unsigned char *src_data[], int src_linesize[], int width, int height, int start, unsigned char *data, int linesize, int deinterlace )
{
	if( !src_data[0] || !src_data[1] || !src_data[2] || (colorspace != AV_IMAGE_BGRA_32 && colorspace != AV_IMAGE_RGBX_32) )
		return;	  

	int perf_time = 0;

	unsigned char* inBufferY = src_data[0];
	int inStrideY = src_linesize[0];

	unsigned char* inBufferU = src_data[1];
	int inStrideU = src_linesize[1];

	unsigned char* inBufferV = src_data[2];
	int inStrideV = src_linesize[2];

	if (deinterlace) {
		inBufferY = (unsigned char *)malloc(width*height);
		inBufferU = (unsigned char *)malloc(width*height/4);
		inBufferV = (unsigned char *)malloc(width*height/4);
		inStrideY = width;
		inStrideU = width/2;
		inStrideV = width/2;
#ifdef DEBUG_MSG
		if (deinterlace_perf) {
			total_byte += (width * height) * 3 / 2;
			perf_time = get_time();
		}
#endif
		RenderX(inBufferY, src_data[0], width, height, width, src_linesize[0]);
		RenderX(inBufferU, src_data[1], width/2, height/2, width/2, src_linesize[1]);
		RenderX(inBufferV, src_data[2], width/2, height/2, width/2, src_linesize[2]);
#ifdef DEBUG_MSG
		if (deinterlace_perf) {
			total_time += get_time()-perf_time;
			if (total_byte >= (40 << 20)) {
				serprintf("Deint %f MB/s\n", ((float)total_byte*1000.0)/(1024.0*1024.0*(float)total_time));
				total_time = total_byte = 0;
			}
		}
#endif
	}
#ifdef CONFIG_NEON
	if( use_neon ) {
		void (*convert)(uint32_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, int, int) = colorspace == AV_IMAGE_BGRA_32 ? neon_yuv420_to_BGRA32 : neon_yuv420_to_RGBX32;
		int y;
		for (y = start; y < start + height; y += 2) {
			int x;
			for( x = 0; x < width; x += 16 ) {
				unsigned char *Y1 = inBufferY + y      * inStrideY + x;
				unsigned char *Y2 = inBufferY +(y + 1) * inStrideY + x;
				unsigned char *U  = inBufferU + y / 2  * inStrideU + x / 2;
				unsigned char *V  = inBufferV + y / 2  * inStrideV + x / 2;
				UINT32 *dst = (UINT32*)data + y  * linesize + x;
				convert(&dst[0], Y1, Y2, U, V, width - x, linesize);
			}
		}
	} else
#endif
	{
		int (*convert)(int, int, int) = colorspace == AV_IMAGE_BGRA_32 ? convertYUVtoBGRA32 : convertYUVtoRGBX32;
		int y;
		for( y = start ; y < start + height; y += 2 ) {
			int x;

			for( x = 0; x < width; x++ ) {
				unsigned char *Y1 = inBufferY + y      * inStrideY + x;
				unsigned char *Y2 = inBufferY +(y + 1) * inStrideY + x;
				unsigned char *U  = inBufferU + y / 2  * inStrideU + x / 2;
				unsigned char *V  = inBufferV + y / 2  * inStrideV + x / 2;
				UINT32 *dst = (UINT32*)data + y  * linesize + x;
				dst[0]        = convert(*Y1 - 16, *U - 128, *V - 128);
				dst[linesize] = convert(*Y2 - 16, *U - 128, *V - 128);
			}
		}
	}
	if (deinterlace) {
		free(inBufferY);
		free(inBufferU);
		free(inBufferV);
	}
}

static void convert_420P10b_to_RGB( int colorspace, unsigned char *src_data[], int src_linesize[], int width, int height, int start, unsigned char *data, int linesize )
{
	if( !src_data[0] || !src_data[1] || !src_data[2] || (colorspace != AV_IMAGE_BGRA_32 && colorspace != AV_IMAGE_RGBX_32) )
		return;

	int y = 0;
#ifdef CONFIG_NEON
	if( use_neon ) {
		void (*convert)(uint32_t *, uint16_t *, uint16_t *, uint16_t *, uint16_t *, int, int) = colorspace == AV_IMAGE_BGRA_32 ? neon_yuv42010b_to_BGRA32 : neon_yuv42010b_to_RGBX32;
		for (y = start; y < start+height; y += 2) {
			int x;
			for( x = 0; x < width; x += 16 ) {
				UINT16 *Y1 = (UINT16*)src_data[0] + y      * src_linesize[0]/2 + x;
				UINT16 *Y2 = (UINT16*)src_data[0] +(y + 1) * src_linesize[0]/2 + x;
				UINT16 *U  = (UINT16*)src_data[1] + y / 2  * src_linesize[1]/2 + x / 2;
				UINT16 *V  = (UINT16*)src_data[2] + y / 2  * src_linesize[2]/2 + x / 2;
				UINT32 *dst = (UINT32*)data + y  * linesize + x;

				convert(&dst[0], Y1, Y2, U, V, width - x, linesize);
			}
		}
	} else
#endif
	{
		int (*convert)(int, int, int) = colorspace == AV_IMAGE_BGRA_32 ? convertYUV10btoBGRA32 : convertYUV10btoRGBX32;

		for( y=start ; y < start+height; y += 2 ) {
			int x;

			for( x = 0; x < width; x++ ) {
				UINT16 *Y1 = (UINT16*)src_data[0] + y      * src_linesize[0]/2 + x;
				UINT16 *Y2 = (UINT16*)src_data[0] +(y + 1) * src_linesize[0]/2 + x;
				UINT16 *U  = (UINT16*)src_data[1] + y / 2  * src_linesize[1]/2 + x / 2;
				UINT16 *V  = (UINT16*)src_data[2] + y / 2  * src_linesize[2]/2 + x / 2;
				UINT32 *dst = (UINT32*)data + y  * linesize + x;

				dst[0]        = convert(*Y1 - 64, *U - 512, *V - 512);
				dst[linesize] = convert(*Y2 - 64, *U - 512, *V - 512);
			}
		}
	}
}

__attribute__((unused))
static void convert_422P_to_RGB( int colorspace, unsigned char *src_data[], int src_linesize[], int width, int height, int start, unsigned char *data, int linesize )
{
	if( !src_data[0] || !src_data[1] || !src_data[2] || (colorspace != AV_IMAGE_BGRA_32 && colorspace != AV_IMAGE_RGBX_32) )
		return;

#ifdef CONFIG_NEON
	if( use_neon ) {
		void (*convert)(uint32_t *, uint8_t *, uint8_t *, uint8_t *, int) = colorspace == AV_IMAGE_BGRA_32 ? neon_yuv422_to_BGRA32 : neon_yuv422_to_RGBX32;
		int y;
		for (y = start; y < start + height; y ++) {
			int x;
			for( x = 0; x < width; x += 16 ) {
				unsigned char  *Y = src_data[0] + y * src_linesize[0] + x;
				unsigned char  *U = src_data[1] + y * src_linesize[1] + x;
				unsigned char  *V = src_data[2] + y * src_linesize[2] + x;
				UINT32     *dst = (UINT32*)data + y * linesize + x;

				convert(&dst[0], Y, U, V, width - x);
			}
		}
	} else
#endif
	{
		int (*convert)(int, int, int) = colorspace == AV_IMAGE_BGRA_32 ? convertYUVtoBGRA32 : convertYUVtoRGBX32;
		int y;
		for( y = start; y < start + height; y ++ ) {
			int x;

			for( x = 0; x < width; x++ ) {
				unsigned char  *Y = src_data[0] + y * src_linesize[0] + x;
				unsigned char  *U = src_data[1] + y * src_linesize[1] + x;
				unsigned char  *V = src_data[2] + y * src_linesize[2] + x;
				UINT32 *dst = (UINT32*)data + y * linesize + x;

				dst[0] = convert(*Y - 16, *U - 128, *V - 128);
			}
		}
	}
}

static void convert_444P_to_RGB( int colorspace, unsigned char *src_data[], int src_linesize[], int width, int height, int start, unsigned char *data, int linesize )
{
	if( !src_data[0] || !src_data[1] || !src_data[2] || (colorspace != AV_IMAGE_BGRA_32 && colorspace != AV_IMAGE_RGBX_32) )
		return;

#ifdef CONFIG_NEON
/*
	if( use_neon ) {
		// TBD
	} else
*/
#endif
	{
		int (*convert)(int, int, int) = colorspace == AV_IMAGE_BGRA_32 ? convertYUVtoBGRA32 : convertYUVtoRGBX32;
		int y;
		for( y = start; y < start + height; y ++ ) {
			int x;

			for( x = 0; x < width; x++ ) {
				unsigned char  *Y = src_data[0] + y * src_linesize[0] + x;
				unsigned char  *U = src_data[1] + y * src_linesize[1] + x;
				unsigned char  *V = src_data[2] + y * src_linesize[2] + x;
				UINT32 *dst = (UINT32*)data + y * linesize + x;

				dst[0] = convert(*Y - 16, *U - 128, *V - 128);
			}
		}
	}
}

static void convert_NV12_to_RGB( int colorspace, unsigned char *src_data[], int src_linesize[], int width, int height, int start, unsigned char *data, int linesize )
{
	if( !src_data[0] || !src_data[1] || (colorspace != AV_IMAGE_BGRA_32 && colorspace != AV_IMAGE_RGBX_32) ) {
		return;
	}

#ifdef CONFIG_NEON
	if( use_neon ) {
		void (*convert)(uint32_t *, uint8_t *, uint8_t *, uint8_t *, int, int) = colorspace == AV_IMAGE_BGRA_32 ? neon_nv12_to_BGRA32 : neon_nv12_to_RGBX32;
		int y;
		for (y = start; y < start + height; y += 2) {
			int x;
			for( x = 0; x < width; x += 16 ) {
				unsigned char *Y1  = src_data[0] + y      * src_linesize[0] + x;
				unsigned char *Y2  = src_data[0] +(y + 1) * src_linesize[0] + x;
				unsigned char *UV  = src_data[1] + y / 2  * src_linesize[1] + x;
				UINT32 *dst = (UINT32*)data + y * linesize + x;

				convert(&dst[0], Y1, Y2, UV, width - x, linesize);
			}
		}
	} else
#endif
	{
		int (*convert)(int, int, int) = colorspace == AV_IMAGE_BGRA_32 ? convertYUVtoBGRA32 : convertYUVtoRGBX32;
		int y;
		for( y = start; y < start + height; y += 2 ) {
			int x;

			unsigned char *Y1 = src_data[0] + y      * src_linesize[0];
			unsigned char *Y2 = src_data[0] +(y + 1) * src_linesize[0];
			unsigned char *UV = src_data[1] + y / 2  * src_linesize[1];
			UINT32 *dst = (UINT32*)data + y * linesize;

			for( x = 0; x < width; x+= 2 ) {
				unsigned char U = *UV++;
				unsigned char V = *UV++;

				dst[0] =        convert(*Y1 - 16, U - 128, V - 128);
				dst[linesize] = convert(*Y2 - 16, U - 128, V - 128);

				Y1++;
				Y2++;
				dst++;

				dst[0] =        convert(*Y1 - 16, U - 128, V - 128);
				dst[linesize] = convert(*Y2 - 16, U - 128, V - 128);
			
				Y1++;
				Y2++;
				dst++;
			}
		}
	}
}

static void convert_420P_to_UYVY( unsigned char *src_data[], int src_linesize[], int width, int height, int start, unsigned char *data, int linesize )
{
	if( !src_data[0] || !src_data[1] || !src_data[2] )
		return;

	int y = start;
#ifdef CONFIG_NEON
	if( use_neon ) {
		unsigned char  *Y = src_data[0] + y     * src_linesize[0];
		unsigned char  *U = src_data[1] + y / 2 * src_linesize[1];
		unsigned char  *V = src_data[2] + y / 2 * src_linesize[2];

		int w = (width + 0x0f) & ~0x0f;
		int h = height & ~0x0f;
		if( neon_swap ) {
			neon_yuv420_to_YUYV((USHORT*)data, Y, U, V, w, h, src_linesize[0], src_linesize[1], linesize * 2);
		} else {
			neon_yuv420_to_UYVY((USHORT*)data, Y, U, V, w, h, src_linesize[0], src_linesize[1], linesize * 2);
		}
		y += h;
		if( y == start + height ) {
			return;
		}
	}
#endif
	for( ; y < start + height; y += 2 ) {
		unsigned char  *Y1 = src_data[0] + y       * src_linesize[0];
		unsigned char  *Y2 = src_data[0] + (y + 1) * src_linesize[0];
		
		unsigned char  *U  = src_data[1] + y / 2   * src_linesize[1];
		unsigned char  *V  = src_data[2] + y / 2   * src_linesize[2];
		
		int dst_y1 = y;
		int dst_y2 = y + 1;
		
		unsigned short *dst1 	= (USHORT*)data  + dst_y1  * linesize;
		unsigned short *dst2 	= (USHORT*)data  + dst_y2  * linesize;
		
		int x;
		for( x = 0; x < width; x += 2 ) {
			unsigned short YU1 = (*Y1++ << 8) | *U;
			unsigned short YV1 = (*Y1++ << 8) | *V;
			
			unsigned short YU2 = (*Y2++ << 8) | *U++;
			unsigned short YV2 = (*Y2++ << 8) | *V++;
			
			*dst1++ = YU1;
			*dst1++ = YV1;
			
			*dst2++ = YU2;
			*dst2++ = YV2;
		}	
	}
}

#define SCALE(A) (((uint32_t)(A)*220+16384)>>10)

static void convert_420P10b_to_UYVY( unsigned char *src_data[], int src_linesize[], int width, int height, int start, unsigned char *data, int linesize )
{
	if( !src_data[0] || !src_data[1] || !src_data[2] )
		return;

	int y = 0;
	for( ; y < start + height; y += 2 ) {
		uint16_t *Y1     = (uint16_t*)src_data[0] + y       * src_linesize[0]/2;
		uint16_t *Y2     = (uint16_t*)src_data[0] + (y + 1) * src_linesize[0]/2;

		uint16_t *U      = (uint16_t*)src_data[1] + y / 2   * src_linesize[1]/2;
		uint16_t *V      = (uint16_t*)src_data[2] + y / 2   * src_linesize[2]/2;

		int dst_y1 = y;
		int dst_y2 = y + 1;

		unsigned short *dst1   = (USHORT*)data  + dst_y1  * linesize;
		unsigned short *dst2   = (USHORT*)data  + dst_y2  * linesize;

		int x;
		for( x = 0; x < width; x += 2 ) {
			unsigned short YU1 = (SCALE(*Y1++) << 8) | ((*U>>2));
			unsigned short YV1 = (SCALE(*Y1++) << 8) | ((*V>>2));

			unsigned short YU2 = (SCALE(*Y2++) << 8) | ((*U++)>>2);
			unsigned short YV2 = (SCALE(*Y2++) << 8) | ((*V++)>>2);

			*dst1++ = YU1;
			*dst1++ = YV1;

			*dst2++ = YU2;
			*dst2++ = YV2;
		}
	}
}

static void convert_422P_to_UYVY( unsigned char *src_data[], int src_linesize[], int width, int height, int start, unsigned char *data, int linesize )
{
	if( !src_data[0] || !src_data[1] || !src_data[2] )
		return;

	int y = start;
#ifdef CONFIG_NEON
	if( use_neon ) {
		unsigned char  *Y = src_data[0] + y * src_linesize[0];
		unsigned char  *U = src_data[1] + y * src_linesize[1];
		unsigned char  *V = src_data[2] + y * src_linesize[2];
		
		int w = (width + 0x0f) & ~0x0f;
		int h = height & ~0x0f;
		if( neon_swap ) {
		//	neon_yuv422_to_YUYV((USHORT*)data, Y, U, V, w, h, src_linesize[0], src_linesize[1], linesize * 2);
		} else {
			neon_yuv422_to_UYVY((USHORT*)data, Y, U, V, w, h, src_linesize[0], src_linesize[1], linesize * 2);
		}
		y += h;
		if( y == start + height ) {
			return;
		}
	}
#endif
	for( ; y < start + height; y ++ ) {
		unsigned char  *Y 	= src_data[0] + y * src_linesize[0];
		
		unsigned char  *U  	= src_data[1] + y * src_linesize[1];
		unsigned char  *V  	= src_data[2] + y * src_linesize[2];
		
		int dst_y = y;
		
		unsigned short *dst 	= (USHORT*)data  + dst_y  * linesize;
		
		int x;
		for( x = 0; x < width; x += 2 ) {
			unsigned short YU = (*Y++ << 8) | *U++;
			unsigned short YV = (*Y++ << 8) | *V++;
			
			*dst++ = YU;
			*dst++ = YV;
		}	
	}
}

static void convert_444P_to_UYVY( unsigned char *src_data[], int src_linesize[], int width, int height, int start, unsigned char *data, int linesize )
{
	if( !src_data[0] || !src_data[1] || !src_data[2] )
		return;

	int y = start;
#ifdef CONFIG_NEON
	if( use_neon ) {
		// TBD
	}
#endif
	for( ; y < start + height; y ++ ) {
		unsigned char  *Y 	= src_data[0] + y * src_linesize[0];
		
		unsigned char  *U  	= src_data[1] + y * src_linesize[1];
		unsigned char  *V  	= src_data[2] + y * src_linesize[2];
		
		int dst_y = y;
		
		unsigned short *dst 	= (USHORT*)data  + dst_y  * linesize;
		
		int x;
		for( x = 0; x < width; x += 2 ) {
			unsigned short YU = (*Y++ << 8) | *U++;
			unsigned short YV = (*Y++ << 8) | *V++;
			
			U++;
			V++;
			
			*dst++ = YU;
			*dst++ = YV;
		}	
	}
}

static void convert_420P10b_to_NV12( unsigned char *src_data[], int src_linesize[], int width, int height, int start, unsigned char *dataY, int linesizeY, unsigned char *dataUV, int linesizeUV )
{
	if( !src_data[0] || !src_data[1] || !src_data[2] )
		return;

	const uint16_t *src_y = (uint16_t*)src_data[0] + start * src_linesize[0]/2;
	const uint16_t *src_u = (uint16_t*)src_data[1] + (start + 1)/2 * src_linesize[1]/2;
	const uint16_t *src_v = (uint16_t*)src_data[2] + (start + 1)/2 * src_linesize[2]/2;
	uint8_t *dst_y = dataY + start * linesizeY;
	uint16_t *dst_uv = (uint16_t*)dataUV + (start + 1)/2 * linesizeUV/2;
	int y, x;

	for (y = 0; y < height; ++y) {
#ifdef CONFIG_NEON
		if( use_neon ) {
			neon_scale_420P10b_YUV(dst_y, src_y, width);
		} else
#endif
		{
			for (x = 0; x < width; x++)
				  dst_y[x] = SCALE(src_y[x]);
		}
		src_y += src_linesize[0]/2;
		dst_y += linesizeY;
	}
	for (y = 0; y < (height + 1) / 2; ++y) {
#ifdef CONFIG_NEON
		if( use_neon ) {
			neon_copy_pack_shift(dst_uv, src_v, src_u, width);
		} else
#endif
		{
			for (x = 0; x < (width + 1) / 2; x++) {
				dst_uv[x] = ((src_v[x]>>2) << 8) | src_u[x]>>2; 
			}
		}
		src_u += src_linesize[1]/2;
		src_v += src_linesize[2]/2;

		dst_uv += linesizeUV/2;
	}
}

static void convert_420P_to_NV12( unsigned char *src_data[], int src_linesize[], int width, int height, int start, unsigned char *dataY, int linesizeY, unsigned char *dataUV, int linesizeUV )
{
	if( !src_data[0] || !src_data[1] || !src_data[2] )
		return;

	int y;
	for( y = start; y < start + height; y += 2 ) {
		unsigned char  *Y1 	= src_data[0] + y       * src_linesize[0];
		unsigned char  *Y2 	= src_data[0] + (y + 1) * src_linesize[0];
		
		unsigned char  *U  	= src_data[1] + y / 2   * src_linesize[1];
		unsigned char  *V  	= src_data[2] + y / 2   * src_linesize[2];
		
		int dst_y1 = y;
		int dst_y2 = y + 1;
		
		unsigned short *dstY1 	= (USHORT*)dataY  + dst_y1 * linesizeY / 2;
		unsigned short *dstY2 	= (USHORT*)dataY  + dst_y2 * linesizeY / 2;
		unsigned short *dstUV 	= (USHORT*)dataUV + (y / 2) * linesizeUV / 2;
		
		memcpy( dstY1, Y1, linesizeY );
		memcpy( dstY2, Y2, linesizeY );

		int x;
		for( x = 0; x < width; x += 2 ) {
			*dstUV++ = (*V++ << 8) | *U++;
		}
	}
}

static void convert_422P_to_NV12( unsigned char *src_data[], int src_linesize[], int width, int height, int start, unsigned char *dataY, int linesizeY, unsigned char *dataUV, int linesizeUV )
{
	if( !src_data[0] || !src_data[1] || !src_data[2] )
		return;

	int y;
	for( y = start; y < start + height; y += 2 ) {
		unsigned char  *Y1 	= src_data[0] + y       * src_linesize[0];
		unsigned char  *Y2 	= src_data[0] + (y + 1) * src_linesize[0];
		
//  U+V tbd
//		unsigned char  *U  	= src_data[1] + y / 2   * src_linesize[1];
//		unsigned char  *V  	= src_data[2] + y / 2   * src_linesize[2];
		
		int dst_y1 = y;
		int dst_y2 = y + 1;
		
		unsigned short *dstY1 	= (USHORT*)dataY  + dst_y1 * linesizeY / 2;
		unsigned short *dstY2 	= (USHORT*)dataY  + dst_y2 * linesizeY / 2;
		unsigned short *dstUV 	= (USHORT*)dataUV + (y / 2) * linesizeUV / 2;
		
		memcpy( dstY1, Y1, linesizeY );
		memcpy( dstY2, Y2, linesizeY );
		memset( dstUV, 0x80, linesizeUV );
	}
}

__attribute__((unused))
static void convert_444P_to_NV12( unsigned char *src_data[], int src_linesize[], int width, int height, int start, unsigned char *dataY, int linesizeY, unsigned char *dataUV, int linesizeUV )
{
	if( !src_data[0] || !src_data[1] || !src_data[2] )
		return;

	int y;
	for( y = start; y < start + height; y += 2 ) {
		unsigned char  *Y1 	= src_data[0] + y       * src_linesize[0];
		unsigned char  *Y2 	= src_data[0] + (y + 1) * src_linesize[0];
		
//  U+V tbd
//		unsigned char  *U  	= src_data[1] + y / 2   * src_linesize[1];
//		unsigned char  *V  	= src_data[2] + y / 2   * src_linesize[2];
		
		int dst_y1 = y;
		int dst_y2 = y + 1;
		
		unsigned short *dstY1 	= (USHORT*)dataY  + dst_y1 * linesizeY / 2;
		unsigned short *dstY2 	= (USHORT*)dataY  + dst_y2 * linesizeY / 2;
		unsigned short *dstUV 	= (USHORT*)dataUV + (y / 2) * linesizeUV / 2;
		
		memcpy( dstY1, Y1, linesizeY );
		memcpy( dstY2, Y2, linesizeY );
		memset( dstUV, 0x80, linesizeUV );
	}
}

static void convert_420P10b_to_YV12( unsigned char *src_data[], int src_linesize[], int width, int height, int start, unsigned char *dst_data[], int dst_linesize[] )
{
	const uint16_t *src_y = (uint16_t*)src_data[0] + start * src_linesize[0]/2;
	const uint16_t *src_u = (uint16_t*)src_data[1] + (start + 1)/2 * src_linesize[1]/2;;
	const uint16_t *src_v = (uint16_t*)src_data[2] + (start + 1)/2 * src_linesize[2]/2;;
	uint8_t *dst_y = dst_data[0] + start * dst_linesize[0];
	uint8_t *dst_v = dst_data[1] + (start + 1)/2 * dst_linesize[1];
	uint8_t *dst_u = dst_data[2] + (start + 1)/2 * dst_linesize[2];
	int y, x;

	for (y = 0; y < height; ++y) {
#ifdef CONFIG_NEON
		if( use_neon ) {
			neon_scale_420P10b_YUV(dst_y, src_y, width);
		} else
#endif
		{
			for (x = 0; x < width; x++)
				  dst_y[x] = SCALE(src_y[x]);
		}
		src_y += src_linesize[0]/2;
		dst_y += dst_linesize[0];
	}
	for (y = 0; y < (height + 1) / 2; ++y) {
#ifdef CONFIG_NEON
		if( use_neon ) {
			neon_memcpy_shift(dst_u, src_u, (width + 1) / 2);
			neon_memcpy_shift(dst_v, src_v, (width + 1) / 2);
		} else
#endif
		{
			for (x = 0; x < (width + 1) / 2; x++) {
				dst_u[x] = src_u[x] >> 2;
				dst_v[x] = src_v[x] >> 2;
			}
		}
		src_u += src_linesize[1]/2;
		src_v += src_linesize[2]/2;

		dst_u += dst_linesize[1];
		dst_v += dst_linesize[2];
	}
}

static void convert_420P_to_YV12( unsigned char *src_data[], int src_linesize[], int width, int height, int start, unsigned char *dst_data[], int dst_linesize[], int deinterlace )
{
	uint8_t *src_y = src_data[0] + start * src_linesize[0];
	uint8_t *dst_y = dst_data[0] + start * dst_linesize[0];
		
	uint8_t *src_u = src_data[1] + (start + 1)/2 * src_linesize[1];
	uint8_t *dst_v = dst_data[1] + (start + 1)/2 * dst_linesize[1];
	
	uint8_t *src_v = src_data[2] + (start + 1)/2 * src_linesize[2];
	uint8_t *dst_u = dst_data[2] + (start + 1)/2 * dst_linesize[2];
	int y;
	int perf_time = 0;
	
	unsigned char* inBufferY = src_y;
	int inStrideY = src_linesize[0];

	unsigned char* inBufferU = src_u;
	int inStrideU = src_linesize[1];

	unsigned char* inBufferV = src_v;
	int inStrideV = src_linesize[2];

	if (deinterlace) {
		inBufferY = (unsigned char *)malloc(width*height);
		inBufferU = (unsigned char *)malloc((width + 1)/2*(height+1)/2);
		inBufferV = (unsigned char *)malloc((width + 1)/2*(height+1)/2);
		inStrideY = width;
		inStrideU = (width + 1)/2;
		inStrideV = (width + 1)/2;
#ifdef DEBUG_MSG
		if (deinterlace_perf) {
			total_byte += (width * height) * 3 / 2;
			perf_time = get_time();
		}
#endif
		RenderX(inBufferY, src_y, width, height, inStrideY, src_linesize[0]);
		RenderX(inBufferU, src_u, width/2, (height + 1)/2, inStrideU, src_linesize[1]);
		RenderX(inBufferV, src_v, width/2, (height + 1)/2, inStrideV, src_linesize[2]);
#ifdef DEBUG_MSG
		if (deinterlace_perf) {
			total_time += get_time()-perf_time;
			if (total_byte >= (40 << 20)) {
				serprintf("Deint %f MB/s\n", ((float)total_byte*1000.0)/(1024.0*1024.0*(float)total_time));
				total_time = total_byte = 0;
			}
		}
#endif
	}

	for (y = 0; y < height; y++) {
		memcpy(dst_y, inBufferY + y * inStrideY, width);
		dst_y += dst_linesize[0];
	}

	for (y = 0; y < (height + 1) / 2; y++) {
		memcpy(dst_u, inBufferU + y * inStrideU, (width + 1) / 2);
		memcpy(dst_v, inBufferV + y * inStrideV, (width + 1) / 2);

		dst_v += dst_linesize[1];
		dst_u += dst_linesize[2];
	}

	if (deinterlace) {
		free(inBufferY);
		free(inBufferU);
		free(inBufferV);
	}
}


/* get frame tile coordinate. XXX: nothing to be understood here, don't try. */
static size_t tile_pos(size_t x, size_t y, size_t w, size_t h)
{
	size_t flim = x + (y & ~1) * w;

	if (y & 1) {
		flim += (x & ~3) + 2;
	} else if ((h & 1) == 0 || y != (h - 1)) {
		flim += (x + 2) & ~3;
	}

	return flim;
}

//void qcom_convert(const uint8_t *src, picture_t *pic)
static void convert_QCOM_NV12_TILED_to_NV12( unsigned char *src_data[], int src_linesize[], int width, int height, int start, int total_height, unsigned char *dst_data[], int dst_linesize[] )
{
#define TILE_WIDTH 64
#define TILE_HEIGHT 32
#define TILE_SIZE (TILE_WIDTH * TILE_HEIGHT)
#define TILE_GROUP_SIZE (4 * TILE_SIZE)

	size_t pitch = dst_linesize[0];

	const size_t tile_w       = (width - 1) / TILE_WIDTH + 1;
	const size_t tile_w_align = (tile_w + 1) & ~1;

	const size_t tile_h_luma_start = (start) / TILE_HEIGHT;
	const size_t tile_h_luma_end   = (start + height - 1) / TILE_HEIGHT + 1;
	const size_t total_tile_h_chroma = (total_height / 2 - 1) / TILE_HEIGHT + 1;

	const size_t total_tile_h_luma = (total_height - 1) / TILE_HEIGHT + 1;

	size_t luma_size = tile_w_align * total_tile_h_luma * TILE_SIZE;

	size_t x, y;

	if((luma_size % TILE_GROUP_SIZE) != 0)
		luma_size = (((luma_size - 1) / TILE_GROUP_SIZE) + 1) * TILE_GROUP_SIZE;

	for(y = tile_h_luma_start; y < tile_h_luma_end; y++) {
		size_t row_width = width;
		for(x = 0; x < tile_w; x++) {
			/* luma source pointer for this tile */
			const uint8_t *src_luma  = src_data[0]
				+ tile_pos(x, y,tile_w_align, total_tile_h_luma) * TILE_SIZE;

			/* chroma source pointer for this tile */
			const uint8_t *src_chroma = src_data[0] + luma_size
				+ tile_pos(x, y/2, tile_w_align, total_tile_h_chroma) * TILE_SIZE;
			if (y & 1)
				src_chroma += TILE_SIZE/2;

			/* account for right columns */
			size_t tile_width = row_width;
			if (tile_width > TILE_WIDTH)
				tile_width = TILE_WIDTH;

			/* account for bottom rows */
			size_t tile_height = height;
			if (tile_height > TILE_HEIGHT)
				tile_height = TILE_HEIGHT;

			/* dest luma memory index for this tile */
			size_t luma_idx = y * TILE_HEIGHT * pitch + x * TILE_WIDTH;

			/* dest chroma memory index for this tile */
			/* XXX: remove divisions */
			size_t chroma_idx = (luma_idx / pitch) * pitch/2 + (luma_idx % pitch);

			tile_height /= 2; // we copy 2 luma lines at once
			while (tile_height--) {
				memcpy(&dst_data[0][luma_idx], src_luma, tile_width);
				src_luma += TILE_WIDTH;
				luma_idx += pitch;

				memcpy(&dst_data[0][luma_idx], src_luma, tile_width);
				src_luma += TILE_WIDTH;
				luma_idx += pitch;

				memcpy(&dst_data[1][chroma_idx], src_chroma, tile_width);
				src_chroma += TILE_WIDTH;
				chroma_idx += pitch;
			}
			row_width -= TILE_WIDTH;
		}
		height -= TILE_HEIGHT;
	}
#undef TILE_WIDTH
#undef TILE_HEIGHT
#undef TILE_SIZE
#undef TILE_GROUP_SIZE
}

static void convert_QCOM_NV12_TILED_to_RGB( int colorspace, unsigned char *src_data[], int src_linesize[], int width, int height, int start, int total_height, unsigned char *data, int linesize )
{
#define TILE_WIDTH 64
#define TILE_HEIGHT 32
#define TILE_SIZE (TILE_WIDTH * TILE_HEIGHT)
#define TILE_GROUP_SIZE (4 * TILE_SIZE)

	size_t pitch = linesize;

	const size_t tile_w = (width - 1) / TILE_WIDTH + 1;
	const size_t tile_w_align = (tile_w + 1) & ~1;
	
	const size_t tile_h_luma_start = (start) / TILE_HEIGHT;
	const size_t tile_h_luma_end   = (start + height - 1) / TILE_HEIGHT + 1;
	const size_t total_tile_h_chroma = (total_height / 2 - 1) / TILE_HEIGHT + 1;

	const size_t total_tile_h_luma = (total_height - 1) / TILE_HEIGHT + 1;

	size_t luma_size = tile_w_align * total_tile_h_luma * TILE_SIZE;

	size_t x, y;
	if((luma_size % TILE_GROUP_SIZE) != 0)
		luma_size = (((luma_size - 1) / TILE_GROUP_SIZE) + 1) * TILE_GROUP_SIZE;

#ifdef CONFIG_NEON
	if( use_neon ) {
		void (*convert)(uint32_t *, uint8_t *, uint8_t *, uint8_t *, int, int) = colorspace == AV_IMAGE_BGRA_32 ? neon_nv12_to_BGRA32 : neon_nv12_to_RGBX32;

		for(y = tile_h_luma_start; y < tile_h_luma_end; y++) {
			size_t row_width = width;
			for(x = 0; x < tile_w; x++) {
				int ty;
				/* luma source pointer for this tile */
				uint8_t *src_luma  = src_data[0]
					+ tile_pos(x, y,tile_w_align, total_tile_h_luma) * TILE_SIZE;

				/* chroma source pointer for this tile */
				uint8_t *src_chroma = src_data[0] + luma_size
					+ tile_pos(x, y/2, tile_w_align, total_tile_h_chroma) * TILE_SIZE;
				if (y & 1)
					src_chroma += TILE_SIZE/2;

				/* account for right columns */
				size_t tile_width = row_width;
				if (tile_width > TILE_WIDTH)
					tile_width = TILE_WIDTH;

				/* account for bottom rows */
				size_t tile_height = height;
				if (tile_height > TILE_HEIGHT)
					tile_height = TILE_HEIGHT;

				/* dest luma memory index for this tile */
				size_t luma_idx = y * TILE_HEIGHT * pitch + x * TILE_WIDTH;
				
				for( ty=0 ; ty < tile_height; ty += 2 ) {
					int tx;

					for( tx = 0; tx < width; tx+= 16 ) {
						unsigned char *Y1 = src_luma + ty      * TILE_WIDTH + tx;
						unsigned char *Y2 = src_luma +(ty + 1) * TILE_WIDTH + tx;
						unsigned char *UV  = src_chroma + ty / 2  * TILE_WIDTH + tx;
						UINT32 *dst = (UINT32*)data + luma_idx + tx;

						convert(&dst[0], Y1, Y2, UV, tile_width - tx, linesize);
					}
					luma_idx += 2*pitch;
				}
				row_width -= TILE_WIDTH;
			}
			height -= TILE_HEIGHT;
		}
	} else
#endif
	{
		int (*convert)(int, int, int) = colorspace == AV_IMAGE_BGRA_32 ? convertYUVtoBGRA32 : convertYUVtoRGBX32;
		for(y = tile_h_luma_start; y < tile_h_luma_end; y++) {
			size_t row_width = width;
			for(x = 0; x < tile_w; x++) {
				int ty;
				/* luma source pointer for this tile */
				uint8_t *src_luma  = src_data[0]
					+ tile_pos(x, y,tile_w_align, total_tile_h_luma) * TILE_SIZE;

				/* chroma source pointer for this tile */
				uint8_t *src_chroma = src_data[0] + luma_size
					+ tile_pos(x, y/2, tile_w_align, total_tile_h_chroma) * TILE_SIZE;
				if (y & 1)
					src_chroma += TILE_SIZE/2;

				/* account for right columns */
				size_t tile_width = row_width;
				if (tile_width > TILE_WIDTH)
					tile_width = TILE_WIDTH;

				/* account for bottom rows */
				size_t tile_height = height;
				if (tile_height > TILE_HEIGHT)
					tile_height = TILE_HEIGHT;

				/* dest luma memory index for this tile */
				size_t luma_idx = y * TILE_HEIGHT * pitch + x * TILE_WIDTH;

				for( ty=0 ; ty < tile_height; ty += 2 ) {
					int tx;

					unsigned char *Y1 = src_luma + ty      * TILE_WIDTH;
					unsigned char *Y2 = src_luma +(ty + 1) * TILE_WIDTH;
					unsigned char *UV = src_chroma + ty / 2  * TILE_WIDTH;

					UINT32 *dst = (UINT32*)data + luma_idx;
					for( tx = 0; tx < tile_width; tx+= 2 ) {
						unsigned char U = *UV++;
						unsigned char V = *UV++;

						dst[0] =        convert(*Y1 - 16, U - 128, V - 128);
						dst[linesize] = convert(*Y2 - 16, U - 128, V - 128);

						Y1++;
						Y2++;
						dst++;

						dst[0] =        convert(*Y1 - 16, U - 128, V - 128);
						dst[linesize] = convert(*Y2 - 16, U - 128, V - 128);

						Y1++;
						Y2++;
						dst++;
					}
					luma_idx += 2*pitch;
				}
				row_width -= TILE_WIDTH;
			}
			height -= TILE_HEIGHT;
		}
	}

#undef TILE_WIDTH
#undef TILE_HEIGHT
#undef TILE_SIZE
#undef TILE_GROUP_SIZE
}

int color_conversion_supported(int colorspace, int pixfmt)
{
	DBG serprintf("color conversion: in pix fmt %d, out fmt %d\n", pixfmt, colorspace);

	switch(colorspace) {
	case AV_IMAGE_YUV_422:
		return (pixfmt == PIXFMT_YUV420P) || (pixfmt == PIXFMT_YUV422P) || (pixfmt == PIXFMT_YUV444P);
	case AV_IMAGE_YV12:
		return (pixfmt == PIXFMT_YUV420P) || (pixfmt == PIXFMT_YUV420P10LE);
	case AV_IMAGE_NV12:
		return (pixfmt == PIXFMT_YUV420P) || (pixfmt == PIXFMT_YUV422P) || (pixfmt == PIXFMT_YUV444P) ||
		       (pixfmt == PIXFMT_QCOM_NV12_TILED) || (pixfmt == PIXFMT_YUV420P10LE);
	case AV_IMAGE_BGRA_32:
	case AV_IMAGE_RGBX_32:
		return (pixfmt == PIXFMT_YUV420P) || (pixfmt == PIXFMT_YUV422P) || (pixfmt == PIXFMT_YUV444P) ||
		       (pixfmt == PIXFMT_NV12) || (pixfmt == PIXFMT_QCOM_NV12_TILED) || (pixfmt == PIXFMT_YUV420P10LE);
	}
	return 0;
}

static void _convert( int pixfmt, unsigned char *src_data[], int src_linesize[], int width, int height, int start, int total_height, VIDEO_FRAME *frame)
{

#ifndef CONFIG_NEON
	//Deactivate deinterlacing on non Neon SoC
	frame->deinterlace = 0;
#endif

#ifdef CONFIG_LIBYUV
	int (*convert_libyuv)(const uint8*, int, const uint8*, int, const uint8*, int, uint8*, int, int, int) = NULL;
	switch( frame->colorspace ) {
        case AV_IMAGE_YUV_422:
                switch( pixfmt ) {
                case PIXFMT_YUV420P:
                        convert_420P_to_UYVY( src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0] / 2 );
                        break;
                case PIXFMT_YUV420P10LE:
                        convert_420P10b_to_UYVY( src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0] / 2 );
                        break;
                case PIXFMT_YUV422P:
                        convert_422P_to_UYVY( src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0] / 2 );
                        break;
                case PIXFMT_YUV444P:
                        convert_444P_to_UYVY( src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0] / 2 );
                        break;
                }
                break;
        case AV_IMAGE_YV12:
                switch( pixfmt ) {
                case PIXFMT_YUV420P:
                        convert_420P_to_YV12( src_data, src_linesize, width, height, start, frame->data, frame->linestep, frame->deinterlace );
                        break;
                case PIXFMT_YUV420P10LE:
                        convert_420P10b_to_YV12( src_data, src_linesize, width, height, start, frame->data, frame->linestep );
                        break;
                case PIXFMT_YUV422P:
                        break;
                }
                break;
        case AV_IMAGE_NV12:
                switch( pixfmt ) {
                case PIXFMT_YUV420P:
                        convert_420P_to_NV12( src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0], frame->data[1], frame->linestep[1]);
                        break;
                case PIXFMT_YUV422P:
                        convert_422P_to_NV12( src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0], frame->data[1], frame->linestep[1]);
                        break;
                case PIXFMT_YUV420P10LE:
                        convert_420P10b_to_NV12( src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0], frame->data[1], frame->linestep[1]);
                        break;
                case PIXFMT_QCOM_NV12_TILED:
                        convert_QCOM_NV12_TILED_to_NV12( src_data, src_linesize, width, height, start, total_height, frame->data, frame->linestep );
                        break;
                }
                break;
        case AV_IMAGE_BGRA_32:
                switch( pixfmt ) {
                case PIXFMT_YUV420P:
                        convert_libyuv = I420ToARGB;
                        break;
                case PIXFMT_YUV422P:
                        convert_libyuv = I422ToARGB;
                        break;
                case PIXFMT_YUV444P:
                        convert_444P_to_RGB( frame->colorspace, src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0]);
			break;
                case PIXFMT_NV12:
                        convert_NV12_to_RGB( frame->colorspace, src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0]);
                        break;
                case PIXFMT_QCOM_NV12_TILED:
                        convert_QCOM_NV12_TILED_to_RGB( frame->colorspace, src_data, src_linesize, width, height, start, total_height, frame->data[0], frame->linestep[0]);
                        break;
                case PIXFMT_YUV420P10LE:
                        convert_420P10b_to_RGB( frame->colorspace, src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0]);
                        break;
                }
		break;
	case AV_IMAGE_RGBX_32:
                switch( pixfmt ) {
                case PIXFMT_YUV420P:
                        convert_libyuv = I420ToABGR;
                        break;
                case PIXFMT_YUV422P:
			convert_libyuv = I422ToABGR;
                        break;
                case PIXFMT_YUV444P:
                        convert_444P_to_RGB( frame->colorspace, src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0]);
                        break;
                case PIXFMT_NV12:
                        convert_NV12_to_RGB( frame->colorspace, src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0]);
                        break;
                case PIXFMT_QCOM_NV12_TILED:
                        convert_QCOM_NV12_TILED_to_RGB( frame->colorspace, src_data, src_linesize, width, height, start, total_height, frame->data[0], frame->linestep[0]);
                        break;
                case PIXFMT_YUV420P10LE:
                        convert_420P10b_to_RGB( frame->colorspace, src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0]);
                        break;
                }
		break;
	}
	if (convert_libyuv)
                       convert_libyuv(src_data[0] + start * src_linesize[0], src_linesize[0],
                                  src_data[1] + start / 2 * src_linesize[1], src_linesize[1],
                                  src_data[2] + start / 2 * src_linesize[2], src_linesize[2],
                                  frame->data[0] + start * frame->linestep[0]*4, frame->linestep[0]*4,
                                  width, height);
#else
        switch( frame->colorspace ) {
	case AV_IMAGE_YUV_422:
		switch( pixfmt ) {
		case PIXFMT_YUV420P:
			convert_420P_to_UYVY( src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0] / 2 );
			break;
		case PIXFMT_YUV420P10LE:
			convert_420P10b_to_UYVY( src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0] / 2 );
			break;
		case PIXFMT_YUV422P:
			convert_422P_to_UYVY( src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0] / 2 );
			break;
		case PIXFMT_YUV444P:
			convert_444P_to_UYVY( src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0] / 2 );
			break;
		}
		break;
	case AV_IMAGE_YV12:
		switch( pixfmt ) {
		case PIXFMT_YUV420P:
			convert_420P_to_YV12( src_data, src_linesize, width, height, start, frame->data, frame->linestep, frame->deinterlace );
			break;
		case PIXFMT_YUV420P10LE:
			convert_420P10b_to_YV12( src_data, src_linesize, width, height, start, frame->data, frame->linestep );
			break;
		case PIXFMT_YUV422P:
			break;
		}
		break;
	case AV_IMAGE_NV12:
		switch( pixfmt ) {
		case PIXFMT_YUV420P:
			convert_420P_to_NV12( src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0], frame->data[1], frame->linestep[1]);
			break;
		case PIXFMT_YUV422P:
			convert_422P_to_NV12( src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0], frame->data[1], frame->linestep[1]);
			break;
		case PIXFMT_YUV420P10LE:
			convert_420P10b_to_NV12( src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0], frame->data[1], frame->linestep[1]);
			break;
		case PIXFMT_QCOM_NV12_TILED:
			convert_QCOM_NV12_TILED_to_NV12( src_data, src_linesize, width, height, start, total_height, frame->data, frame->linestep );
			break;
		}
		break;
	case AV_IMAGE_BGRA_32:
	case AV_IMAGE_RGBX_32:
		switch( pixfmt ) {
		case PIXFMT_YUV420P:
			convert_420P_to_RGB( frame->colorspace, src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0], frame->deinterlace);
			break;
		case PIXFMT_YUV422P:
			convert_422P_to_RGB( frame->colorspace, src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0]);
			break;
		case PIXFMT_YUV444P:
			convert_444P_to_RGB( frame->colorspace, src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0]);
			break;
		case PIXFMT_NV12:
			convert_NV12_to_RGB( frame->colorspace, src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0]);
			break;
		case PIXFMT_QCOM_NV12_TILED:
			convert_QCOM_NV12_TILED_to_RGB( frame->colorspace, src_data, src_linesize, width, height, start, total_height, frame->data[0], frame->linestep[0]);
			break;
		case PIXFMT_YUV420P10LE:
			convert_420P10b_to_RGB( frame->colorspace, src_data, src_linesize, width, height, start, frame->data[0], frame->linestep[0]);
			break;
		}
		break;
	}
#endif
}

void codec_convert_pixel_format( int pixfmt, unsigned char *src_data[], int src_linesize[], int width, int height, VIDEO_FRAME *frame )
{
	_convert( pixfmt, src_data, src_linesize, width, height, 0, height, frame );
}

#include <pthread.h>

#define MAX_WORKERS 8

typedef struct work {
	int    index;
	
	pthread_mutex_t	work_mutex;
	pthread_cond_t  work_cond;
	int             work_stop;
	
	pthread_mutex_t	done_mutex;
	pthread_cond_t  done_cond;
	
	int    pixfmt; 
	unsigned char **data;
	int    *linestep;
	int    width;
	int    height;
	int    start;
	int    total_height;
	VIDEO_FRAME *frame;
} work_t;

typedef struct convert {
	int    		work_num;
	pthread_t	work_thread_handle[MAX_WORKERS];
	work_t		work[MAX_WORKERS];
} convert_t;

static void *work_thread(void *ctx)
{
	work_t *w = (work_t*)ctx;
	
DBG serprintf("work_thread %d started!\n", w->index);
	while (!w->work_stop) {
		pthread_mutex_lock(&w->work_mutex);
		while (!w->work_stop && !w->frame) {
//serprintf("<<<");
			pthread_cond_wait(&w->work_cond, &w->work_mutex);
//serprintf(">>>");
		}
		pthread_mutex_unlock(&w->work_mutex);
		if( w->work_stop ) {
			break;
		}
		int start = time_update_time();
		_convert( w->pixfmt, w->data, w->linestep, w->width, w->height, w->start, w->total_height, w->frame );
DBG serprintf("work %d/%2d/%2d\n", w->index, w->frame->index, time_update_time() - start );
		w->frame = NULL;

		pthread_mutex_lock(&w->done_mutex);
		pthread_cond_signal(&w->done_cond);
		pthread_mutex_unlock(&w->done_mutex);
	}
DBG serprintf("work_thread %d stopped!\n", w->index);
	return NULL;
}

void codec_convert_mt( void *ctx, int pixfmt, unsigned char *data[], int linesize[], int width, int height, VIDEO_FRAME *frame )
{
	convert_t *c = (convert_t*)ctx;
	
	int start = time_update_time();
	// start the workers
	int i;
	int pos   = 0;
	int slice = (height / (c->work_num + 0)) & ~0x0f;
	for( i = 0; i < c->work_num; i++ ) {
		int h = (i == c->work_num - 1) ? height - pos : slice;
//DBG 
serprintf("pos %d  %4d/%4d\n", i, pos, h);
		pthread_mutex_lock(&c->work[i].work_mutex);
		c->work[i].data     = data;
		c->work[i].linestep = linesize;
		c->work[i].pixfmt   = pixfmt;
		c->work[i].width    = width;
		c->work[i].height   = h;
		c->work[i].start    = pos;
		c->work[i].total_height = height;
		c->work[i].frame    = frame;

		pthread_cond_signal(&c->work[i].work_cond);
		pthread_mutex_unlock(&c->work[i].work_mutex);

		pos += h;
	}
	// wait for workers, since we need them all it does
	// not matter in which order...
	for( i = 0; i < c->work_num; i++ ) {
		pthread_mutex_lock(&c->work[i].done_mutex);
		while ( c->work[i].frame) {
			pthread_cond_wait(&c->work[i].done_cond, &c->work[i].done_mutex);
		}
		pthread_mutex_unlock(&c->work[i].done_mutex);
DBG serprintf("stop %d  %d\n", i, time_update_time() - start);
	}
}

void *codec_convert_mt_init( int work_num )
{
	if( !work_num || work_num > MAX_WORKERS ) {
serprintf("cannot create convert_mt for %d\n", work_num );
		return NULL;
	}	
	
	convert_t *c = acalloc( 1, sizeof( struct convert ) );
	
	c->work_num = work_num;

	int i;
	for( i = 0; i < c->work_num; i++ ) {
		c->work[i].index = i;
		pthread_mutex_init(&c->work[i].work_mutex, NULL);
		pthread_cond_init (&c->work[i].work_cond,  NULL);
		pthread_mutex_init(&c->work[i].done_mutex, NULL);
		pthread_cond_init (&c->work[i].done_cond,  NULL);

		pthread_create(&c->work_thread_handle[i], 0, work_thread, (void*)&c->work[i]);
	}

	return c;
}

int codec_convert_mt_exit( void *ctx )
{
	convert_t *c = (convert_t*)ctx;
	
	if( !c )
		return 1;
		
	int i;
	for( i = 0; i < c->work_num; i++ ) {
DBG serprintf("stop WORK %d\n", i );
		pthread_mutex_lock(&c->work[i].work_mutex);
		c->work[i].work_stop = 1;
		pthread_cond_signal(&c->work[i].work_cond);
		pthread_mutex_unlock(&c->work[i].work_mutex);
		pthread_join(c->work_thread_handle[i], NULL);
DBG serprintf("stop WORK %d done\n", i );
	}
	
	afree( c );
	
	return 0;
}

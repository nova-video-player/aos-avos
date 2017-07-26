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

#include "av.h"
#include "debug.h"
#include "file.h"
#include "bmp.h"
#include "color.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static inline UCHAR clip( int x )
{
	if( x < 0 )   return 0;
	if( x > 255 ) return 255;
	return x;
} 

static void YYUVtoRGB24( int Y1, int Y2, int U, int V, rgba *rgb1, rgba *rgb2 )
{
	int C1 = Y1 - 16;
	int C2 = Y2 - 16;
	
	int D  = U - 128;
	int E  = V - 128;

	rgb1->r = clip(( 298 * C1           + 409 * E + 128) >> 8);
	rgb1->g = clip(( 298 * C1 - 100 * D - 208 * E + 128) >> 8);
	rgb1->b = clip(( 298 * C1 + 516 * D           + 128) >> 8);
	
	rgb2->r = clip(( 298 * C2           + 409 * E + 128) >> 8);
	rgb2->g = clip(( 298 * C2 - 100 * D - 208 * E + 128) >> 8);
	rgb2->b = clip(( 298 * C2 + 516 * D           + 128) >> 8);
}

static void YUYVtoRGB24( unsigned short yu, unsigned short yv, rgba *rgb1, rgba *rgb2 )
{
	int Y1 = yu >> 8;
	int U  = yu & 0xFF;
	
	int Y2 = yv >> 8;
	int V  = yv & 0xFF;
	
	YYUVtoRGB24( Y1, Y2, U, V, rgb1, rgb2 );
}

// ********************************************************
//
//	av_dump_video_frame
//
// *********************************************************
void av_dump_video_frame( VIDEO_FRAME* frame )
{
	if( !frame ) {
		return;
	}
	
	// make sure to create a new file
	int fd = -1;
	
	int i;
	for( i = 0; i < 64; i++) {
		char filename[64];
		sprintf( filename, HDD_ROOT"/screenshot_%i.bmp", i + 1 );
		fd = file_open( filename, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH );
		if( fd != -1 ) {
serprintf("dump video frame to %s\n", filename);
			break;
		}
	}
	if( fd == -1 ) 
		return;

	BMP_write_header( fd, frame->width, frame->height, 24 );

	if( frame->colorspace == AV_IMAGE_YUV_422 ) {
		USHORT *video_buffer = (USHORT *)frame->data[0];
		int y;
		for( y = frame->height - 1; y >= 0; y-- ) {
			USHORT *vid_line = video_buffer + y * frame->linestep[0] / 2;

			int x;
			for( x = 0; x < frame->width; x += 2 ) {
				// get 2 video pixels in 24 bit
				rgba pix_a, pix_b;
				YUYVtoRGB24( vid_line[x], vid_line[x + 1], &pix_a, &pix_b );
				pix_a.a = 255;
				pix_b.a = 255;

				// write the blended pixels
				unsigned char tc;
				tc = pix_a.b; write( fd, &tc, 1 );
				tc = pix_a.g; write( fd, &tc, 1 );
				tc = pix_a.r; write( fd, &tc, 1 );

				tc = pix_b.b; write( fd, &tc, 1 );
				tc = pix_b.g; write( fd, &tc, 1 );
				tc = pix_b.r; write( fd, &tc, 1 );
			}
		}
	} else if( frame->colorspace == AV_IMAGE_NV12 ) {
		UCHAR *Y_buffer  = (UCHAR*)frame->data[0];
		UCHAR *UV_buffer = (UCHAR*)frame->data[1];
		int y;
		for( y = frame->height - 1; y >= 0; y-- ) {
			UCHAR *Y_line   =  Y_buffer +  y      * frame->linestep[0];
			UCHAR *UV_line  = UV_buffer + (y / 2) * frame->linestep[1];
			
			int x;
			for( x = 0; x < frame->width; x += 2 ) {
				// get 2 video pixels in 24 bit
				rgba pix_a, pix_b;
				
				UCHAR   U = UV_line[x];
				UCHAR   V = UV_line[x + 1];
				USHORT Y1 =  Y_line[x];
				USHORT Y2 =  Y_line[x + 1];
				YYUVtoRGB24( Y1, Y2, U, V, &pix_a, &pix_b );
				pix_a.a = 255;
				pix_b.a = 255;

				// write the blended pixels
				unsigned char tc;
				tc = pix_a.b; write( fd, &tc, 1 );
				tc = pix_a.g; write( fd, &tc, 1 );
				tc = pix_a.r; write( fd, &tc, 1 );

				tc = pix_b.b; write( fd, &tc, 1 );
				tc = pix_b.g; write( fd, &tc, 1 );
				tc = pix_b.r; write( fd, &tc, 1 );
			}
		}
	
	}
	close( fd );
}

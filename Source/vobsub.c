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

#ifndef STANDALONE

#include "types.h"
#include "global.h"
#include "debug.h"

#define DBGV1 if(Debug[DBG_SUB] > 1 )
#define DBGV2 if(Debug[DBG_SUB] > 2 )

#endif

#include "vobsub.h"
#include "image.h"
#include "astdlib.h"
#include "util.h"

#include <string.h>

#ifdef CONFIG_VOBSUB
//#define DUMP_PPM

#ifdef DUMP_PPM
static int ppm_count = 0;
#endif

//#define INDEXED_8BIT
#define GRAYSCALE
#define AUTO_PALETTE

#define DBGBB if(0)
typedef struct BB {
	int left;
	int right;
	int top;
	int bottom;
} BB;
 
VOBSUB *VOBSUB_create( unsigned char *palette )
{
DBGV1 serprintf("VOBSUB_create\r\n");
	VOBSUB *vs = amalloc( sizeof( VOBSUB ) );
	if( vs ) {
		memset( vs, 0, sizeof( VOBSUB ) );
	}
	if( palette ) {
		vs->has_palette = 1;
		memcpy( vs->palette, palette, sizeof( vs->palette ) );
		int i;
		for( i = 0; i < 16; i++ ) {
DBGV1 serprintf("%06X ", vs->palette[i] );	
		}
DBGV1 serprintf("\n");	
	}  
#ifdef DUMP_PPM
	ppm_count = 0;
#endif
	return vs;
}

void VOBSUB_destroy( VOBSUB *vs )
{
DBGV1 serprintf("VOBSUB_destroy\r\n");
	if( vs ) {
		if( vs->data )
			afree( vs->data );
		afree( vs );
	}
}

void VOBSUB_reset( VOBSUB *vs )
{
DBGV1 serprintf("VOBSUB_reset\r\n");
	if( vs ) {
		if( vs->data )
			afree( vs->data );
		memset( vs, 0, sizeof( *vs ) );
	}
}

int __get16( UCHAR *data )
{
	int w = *data++ << 8;
	w += *data;
	return w; 
}

int __get8( UCHAR *data )
{
	return *data; 
}

#ifdef DUMP_PPM
#include <stdio.h>

#ifdef INDEXED_8BIT
static void ppm_save_ppm8(const char *filename, UINT8 *bitmap, int pitch, int w, int h, int *rgba_palette)
{
	int x, y, v;
	FILE *f;

	f = fopen(filename, "w");
	if (!f) {
		return;
	}
	fprintf(f, "P6\n"
	"%d %d\n"
	"%d\n",
	w, h, 255);
	for(y = 0; y < h; y++) {
		for(x = 0; x < w; x++) {
			v = rgba_palette[bitmap[y * pitch + x]];
			int r  = (v & 0xF800) >> 8;
			int g  = (v & 0x7E0)  >> 3;
			int b  = (v & 0x1F)   << 3;

			putc(r, f);
			putc(g, f);
			putc(b, f);
		}
	}
	fclose(f);
}
#else
#ifdef GRAYSCALE
static void ppm_save_pgm8(const char *filename, UINT8 *bitmap, int pitch, int w, int h )
{
	int x, y, v;
	FILE *f;

	f = fopen(filename, "w");
	if (!f) {
		return;
	}
	fprintf(f, "P5\n"
	"%d %d\n"
	"%d\n",
	w, h, 255);
	for(y = 0; y < h; y++) {
		for(x = 0; x < w; x++) {
			v = bitmap[y * pitch + x];

			putc(v, f);
		}
	}
	fclose(f);
}
#else
static void ppm_save_ppm16(const char *filename, USHORT *bitmap, int pitch, int w, int h, int *rgba_palette)
{
	int x, y;
	FILE *f;

	f = fopen(filename, "w");
	if (!f) {
		return;
	}
	fprintf(f, "P6\n"
	"%d %d\n"
	"%d\n",
	w, h, 255);
	for(y = 0; y < h; y++) {
		for(x = 0; x < w; x++) {
			USHORT v = bitmap[y * pitch/2 + x];
			int r  = (v & 0xF800) >> 8;
			int g  = (v & 0x7E0)  >> 3;
			int b  = (v & 0x1F)   << 3;

			putc(r, f);
			putc(g, f);
			putc(b, f);
		}
	}
	fclose(f);
}
#endif
#endif

static void ppm_save( VIDEO_FRAME *frame, int *rgba_palette)
{
	int w        = frame->window.width;
	int h        = frame->window.height;
	UCHAR *data  = frame->data[0] + frame->linestep[0] * frame->window.y + frame->window.x;
	int linestep = frame->linestep[0];

	char filename[32];
	sprintf( filename, "/tmp/sub%04d.ppm", ppm_count++ );
	
#ifdef INDEXED_8BIT
	ppm_save_ppm8( filename, data, linestep, w, h, rgb_palette );
#else
#ifdef GRAYSCALE
	//ppm_save_pgm8( filename, frame->data[0], frame->linestep[0], w, h );
	ppm_save_pgm8( filename, data, linestep, w, h );
#else
	ppm_save_ppm16( filename, data, linestep, w, h, rgb_palette );
#endif
#endif
}

#endif

static int get_nibble(const UCHAR *data, int offset)
{
    return (data[offset >> 1] >> ((1 - (offset & 1)) << 2)) & 0xf;
}

void get_pixels( UCHAR *image, int pitch, int width, int height, UCHAR *data, int offset, int size, int *rgba_palette, int *cw, BB *bb)
{
DBGV1 serprintf("get_pixels: %d/%d x %d\r\n", width, pitch, height );
	unsigned int v;
	int x = 0;
	int y = 0; 
	int len, color;
#if defined INDEXED_8BIT || defined GRAYSCALE
	UCHAR  *d = image;
#else
	USHORT *d = (USHORT*) image;
#endif
	int end = size * 2;

	int seen[4] = {  0 };
	int rank[4] = { -1, -1, -1, -1 };
	int rr    = 0;
	int top   = 1;
	int lcol  = -1;
	int llen  = 0;
	int rcol  = -1;
	int rlen  = 0;
	if( bb ) {
		bb->left  = width;
		bb->right = width;
	}
	while( 1 ) {
DBGBB if( x == 0 ) serprintf("\r\n");
		if ( offset >= end )
			break;
		v = get_nibble(data, offset++);
		if (v < 0x4) {
			v = (v << 4) | get_nibble(data, offset++);
			if (v < 0x10) {
				v = (v << 4) | get_nibble(data, offset++);
				if (v < 0x040) {
					v = (v << 4) | get_nibble(data, offset++);
					if (v < 4) {
						v |= (width - x) << 2;
					}
				}
			}
		}
		len = v >> 2;
		if (len > (width - x))
			len = (width - x);
		color = v & 0x03;
DBGBB serprintf("(%d %d) ", len, color );		
		if( bb ) {
			if( lcol == -1 ) {
				// left side color
				lcol = color;
				llen = len;
			} else if( lcol == color ) {
				llen += len;
			} else {
				lcol = -2;
			}
			if( rcol != color ) {
				// right side color
				rcol = color;
				rlen = len;
			} else {
				rlen += len;
			}
		}
		if( cw ) {
			// note which color we see appear 1st from left to middle
			if( !seen[color] ) {
				seen[color] = 1;
				rank[rr++] = color;
//DBGBB serprintf("(seen %d  rr %d) ", color, rr );
			}
			x += len;
		} else {
#ifdef INDEXED_8BIT
			memset(d + x, color, len);
			x += len;
#else
#ifdef GRAYSCALE
			memset(d + x, rgba_palette[color], len);
			x += len;
#else
			int l;
			for( l = 0; l < len; l++ ) {
				d[x++] = rgba_palette[color];
			}	
#endif		
#endif
		}
		if (x >= width) {
			y++;
			if (y > height)
				break;
			d += pitch;
			x = 0;
			// byte align
			offset += (offset & 1);

			if( cw ) {
				// perform a ranking of the colors, depending on which one
				// appears first from the edge to the middle of the image
				int i;
				for( i = 0; i < 4; i++ ) {
					if( rank[i] != -1 ) {
						cw[rank[i]] += 4 - i;
//DBGBB serprintf("[%d %d %d] ", i, rank[i], cw[rank[i]] );
					}
				}
				// reset the ranking
				seen[0] = seen[1] = seen[2] = seen[3] = 0;
				rank[0] = rank[1] = rank[2] = rank[3] = -1;
				rr   = 0;
			} 
			if ( bb ) {
				if( llen == width ) {
					if( top ) {
						bb->top = y;
					}
				} else {
					top = 0;
					bb->bottom = y;			
				}
DBGBB serprintf("l %3d  r %3d", llen, rlen );	
				if( llen && llen < bb->left )
					bb->left = llen;
				if( rlen && rlen < bb->right )
					bb->right = rlen;	 
				lcol  = -1;
				llen  = 0;
				rcol  = -1;
				rlen  = 0;
			} 	
		}
	}
	if( bb ) {
DBGV1 serprintf("top %3d  bottom %3d  left %3d  right %3d\r\n", bb->top, bb->bottom, bb->left, bb->right );	
	}
}

static void get_frame( VIDEO_FRAME *frame, int w, int h, int ofs1, int ofs2, UCHAR *data, int size, int *rgb_palette, int *cw, BB *bb )
{
	int ls = frame->linestep[0];
#if defined INDEXED_8BIT || defined GRAYSCALE
	get_pixels( frame->data[0],      ls * 2, w, h / 2, data, ofs1 * 2, size, rgb_palette, cw, bb);
	get_pixels( frame->data[0] + ls, ls * 2, w, h / 2, data, ofs2 * 2, size, rgb_palette, cw, bb ? bb + 1 : NULL);
#else
	get_pixels( frame->data[0],      ls,     w, h / 2, data, ofs1 * 2, size, rgb_palette, cw, bb);
	get_pixels( frame->data[0] + ls, ls,     w, h / 2, data, ofs2 * 2, size, rgb_palette, cw, bb ? bb + 1 : NULL);
#endif
}

#ifdef AUTO_PALETTE
static void auto_palette( int *rgb, int *cw, int *alpha )
{
DBGV2 serprintf("cw:   %4d %4d %4d %4d\r\n", cw[0], cw[1], cw[2], cw[3] );
	// sort the color weights
	int idx[4] = { 0, 1, 2, 3 };
	int i,j;
	for( j = 0; j < 3; j++ ) {
		for( i = 0; i < 3; i++ ) {
			if( cw[idx[i]] > cw[idx[i + 1]] ) {
				int x      = idx[i];
				idx[i]     = idx[i + 1];
				idx[i + 1] = x;  
			}
		}	
	}
DBGV2 serprintf("sort: %4d %4d %4d %4d\r\n", cw[idx[0]], cw[idx[1]], cw[idx[2]], cw[idx[3]] );
	// get the used colors:
	int used = 0;
	for( i = 0; i < 4; i++ ) {
		if( alpha[i] && cw[i] ) {
			used ++;
		} else {
			cw[i] = 0;
		}	
	}
DBGV2 serprintf("used: %4d %4d %4d %4d -> %d\r\n", cw[0], cw[1], cw[2], cw[3], used );
	
	int count = used;
	for( i = 0; i < 4; i++ ) {
		if( cw[idx[i]] ) {
			// make the last color almost black aka outline...
			rgb[idx[i]] = (count == 1 ) ? 0x01 : 0xff * count / used;
			count --;
		} else {
			rgb[idx[i]] = 0;
		}
	}
DBGV1 serprintf("auto: %02X %02X %02X %02X\r\n", rgb[0], rgb[1], rgb[2], rgb[3] );
}
#else
static void make_palette( int *rgb, int *palette, int *alpha )
{
	int i;
	int color_used[16] = { 0 };
	int num_colors = 0;
	
	for( i = 0; i < 4; i ++ ) {
#ifdef GRAYSCALE
		rgb[i] = 0x00;
#else
		rgb[i] = 0xFD7F;
#endif
		if( alpha[i] && !color_used[palette[i]] ) {
			color_used[palette[i]] = 1;
			num_colors ++;
		}
	}
DBGV2 serprintf("used: %d\r\n", num_colors );
		 
	int col = num_colors; 
	for( i = 0; i < 4; i++ ) {
		if( alpha[i] ) {
#ifdef GRAYSCALE
			rgb[i] = 0xff * col / num_colors;
#else
			int r = 0xff * col / num_colors;
			int g = 0xff * col / num_colors;
			int b = 0xff * col / num_colors;

			r = (r >> 3) & 0x1f;
			g = (g >> 2) & 0x3f;
			b = (b >> 3) & 0x1f;
			rgb[i] = (r << 11) | (g << 5) | (b);
#endif
			col --;
DBGV1 serprintf("COLOR(%d) = %08X\r\n", i, rgb[i] );
		} 
	}
}
#endif

int VOBSUB_parse_chunk( VOBSUB *vs, UCHAR *data, int size, int time, VIDEO_FRAME **pframe )
{
DBGV1 serprintf("VOBSUB_parse_chunk: %08X, %d\r\n", data, size );
	if( !pframe || !data || !size )
		return 1;
	
	if( !vs->size ) {
		vs->write = 0;
		vs->size = __get16( data );
		vs->time = time;
DBGV1 serprintf("pkt_size = %d\r\n", vs->size );
		// bad packet
		if( !vs->size ) {
DBGV1 serprintf("0 pkt ERR!\r\n");
			return 1;
		}
		// new packet
		if( vs->size > vs->data_size ) {
			if( vs->data )
				afree( vs->data );
			vs->data_size = vs->size;
			vs->data = amalloc( vs->data_size );
			if( !vs->data ) {
				vs->data_size = 0;
				goto ErrorExit;
			}
		}
	}

	// add the data
DBGV1 serprintf("add %d at: %d\r\n", size, vs->write);

	// make sure we do not overwrite the buffer!
	size = MIN( size, vs->data_size - vs->write );
	memcpy( vs->data + vs->write, data, size );
	vs->write += size;

	if( vs->write < vs->size ) {
DBGV1 serprintf("need more!\r\n");
		// indicate no output!
		*pframe = NULL;
		return 0;
	}

	data = vs->data;
	size = vs->size;
	int cmd_pos = __get16( data + 2 );
DBGV2 serprintf("cmd_pos  = %04X\r\n", cmd_pos );	

//	Dump( data + cmd_pos, size - cmd_pos );

	int palette[4] = {0, 0, 0, 0};
	int alpha[4];
	int x1    = 0, x2 = 0, y1 = 0, y2 = 0;
	int ofs1  = 0;
	int ofs2  = 0;		
	int start = 0;
	int end   = 0;	
	while( cmd_pos + 4 < size ) {
		int date = __get16( data + cmd_pos     );
		int next = __get16( data + cmd_pos + 2 );
DBGV2 serprintf("date %5d, next %04X\r\n", date, next );	
		
		int pos = cmd_pos + 4;
		while( pos < size ) {
		
			int cmd  = data[pos++];
DBGV2 serprintf("\tcmd %02X:  ", cmd );	
			switch( cmd ) {
			case 0x00:
DBGV2 serprintf("menu?\r\n");
				// menu subpicture 
				goto end;
				break;
			case 0x01:
				// start time
				start = date * 10;
DBGV2 serprintf("start: %d\r\n", date);
				break;
			case 0x02:
				// end time
				end = date * 10;
DBGV2 serprintf("end:   %d\r\n", date);
				break;
			case 0x03:
				// set palette
				if ((size - pos) < 2)
					goto ErrorExit;
				palette[3] = data[pos] >> 4;
				palette[2] = data[pos] & 0x0f;
				palette[1] = data[pos + 1] >> 4;
				palette[0] = data[pos + 1] & 0x0f;
				pos += 2;
DBGV2 serprintf("palette %X%X%X%X\n", palette[0], palette[1], palette[2], palette[3]);
				break;
			case 0x04:
				// set alpha 
				if ((size - pos) < 2)
					goto ErrorExit;
				alpha[3] = data[pos] >> 4;
				alpha[2] = data[pos] & 0x0f;
				alpha[1] = data[pos + 1] >> 4;
				alpha[0] = data[pos + 1] & 0x0f;
				pos += 2;
DBGV2 serprintf("alpha   %X%X%X%X\n", alpha[0], alpha[1], alpha[2], alpha[3]);
				break;
			case 0x05:
				if ((size - pos) < 6)
					goto ErrorExit;
				x1 = (data[pos] << 4) | (data[pos + 1] >> 4);
				x2 = ((data[pos + 1] & 0x0f) << 8) | data[pos + 2];
				y1 = (data[pos + 3] << 4) | (data[pos + 4] >> 4);
				y2 = ((data[pos + 4] & 0x0f) << 8) | data[pos + 5];
DBGV2 serprintf("x1 %d  x2 %d  y1 %d  y2 %d\n", x1, x2, y1, y2);
				pos += 6;
				break;
			case 0x06:
				if ((size - pos) < 4)
					goto ErrorExit;
				ofs1 = __get16(data + pos);
				ofs2 = __get16(data + pos + 2);
DBGV2 serprintf("ofs1 %04X  ofs2 %04X\n", ofs1, ofs2);
				pos += 4;
				break;
			case 0xff:
DBGV2 serprintf("end!\r\n");
				goto end;
			default:
DBGV2 serprintf("unknown\r\n");
				goto end;			
			}
		}
end:
		if( cmd_pos == next ) {
			break;
		}
		cmd_pos = next;
	}

	vs->size = 0;

	if (ofs1 >= 0) {
		VIDEO_FRAME *frame = *pframe;
		int w = MAX( 0, x2 - x1 + 1);
		int h = MAX( 0, y2 - y1 );
		
		if( w > frame->width || h > frame->height ) {
serprintf("VOBSUB: frame too small %d x %d < %d x %d\r\n", frame->width, frame->height, w, h );	
			goto ErrorExit;
		}
			
		if (w > 0 && h > 0) {
			int rgb_palette[4];
			int cw[4] = {0};
			BB  bb[2] = {0};
			get_frame( frame, w, h, ofs1, ofs2, data, size, NULL, cw, bb );
			if( vs->has_palette ) {
				int i;
				for( i = 0; i < 4; i++ ) {
					rgb_palette[i] = vs->palette[palette[i]];
#ifdef GRAYSCALE
					// convert YUV to grayscale!
					rgb_palette[i] >>= 16;
					if( !alpha[i] )
						rgb_palette[i] = 0;
#endif	
				}
DBGV1 serprintf("pal: %06x %06X %06X %06x\r\n", rgb_palette[0], rgb_palette[1], rgb_palette[2], rgb_palette[3]);	
			} else {
#ifdef AUTO_PALETTE
				auto_palette( rgb_palette, cw,      alpha );
#else
				make_palette( rgb_palette, palette, alpha ); 
#endif
			}
			// create unified bouding box;
			bb[0].bottom --;
			bb[1].bottom --;
			int top    = MIN( bb[0].top    * 2, bb[0].top    * 2 + 1 ); 
			int bottom = MAX( bb[0].bottom * 2, bb[0].bottom * 2 + 1 ); 
			// make the bounding box symmetric on horizontal:
			int left   = MIN( MIN(bb[0].left, bb[0].left), MIN( bb[0].right, bb[0].right ));
			int right  = w - left;
DBGV1 serprintf("top %d  bottom %d  left %d  right %d\r\n", top, bottom, left, right );	

			get_frame( frame, w, h, ofs1, ofs2, data, size, rgb_palette, NULL, NULL );

			frame->window.x      = left;
			frame->window.width  = right - left + 1;
			frame->window.y      = top;
			frame->window.height = bottom - top + 1;
			frame->time          = vs->time + start;
			frame->duration      = end - start; 
DBGV1 serprintf("time %8d  dur %8d\r\n", frame->time, frame->duration );	
			if( frame->duration <= 0 ) {
				// if no end given, let it be shown for a long time
				frame->duration = 10000;
			}
#ifdef INDEXED_8BIT
			frame->colorspace    = AV_IMAGE_8BIT_INDEXED;
			int i;
			for( i = 0; i < 4; i++ )
				frame->palette[i] = rgb_palette[i];
#else
#ifdef GRAYSCALE
			frame->colorspace    = AV_IMAGE_GRAYSCALE;
#else
			frame->colorspace    = AV_IMAGE_RGB_16_AUTO_ALPHA;
#endif
#endif

#ifdef DUMP_PPM
ppm_save( frame, rgb_palette );
#endif
			return 0;
		}
	}
	
	// indicate no output!
	*pframe = NULL;

	return 0;

ErrorExit:
	// indicate no output!
	*pframe = NULL;

	vs->size = 0;
	return 1;
}

#endif

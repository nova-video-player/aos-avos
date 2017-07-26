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

#include "types.h"
#include "global.h"
#include "debug.h"
#include "mpg4.h"
#include "stream.h"
#include "cbe.h"
#include "bits.h"
#include "util.h"
#include "get.h"

#include <stdlib.h>

#ifdef CONFIG_MPG4

#ifndef STANDALONE

#define DBG if(Debug[DBG_MANGLER])
#define DBGP if(Debug[DBG_PARSER])

#endif

// *****************************************************************************
//
//	_patch_vol_header
//
// *****************************************************************************
static void _patch_vol_header( UCHAR *data ) 
{
	BITS _bits;
	BITS *bits = &_bits;
	BITS_init( bits, data, 16 * 8 );

	BITS_get1( bits ); // random access
	int type = BITS_get(bits, 8);
	int ver;
	if (BITS_get1( bits ) != 0) { /* is_ol_id */
		ver = BITS_get( bits, 4); /* vo_ver_id */
		BITS_get( bits, 3); /* vo_priority */
	} else {
		ver = 1;
	}
DBG serprintf("vo type: %d\n", type);
DBG serprintf("vo ver : %d\n", ver );
	int aspect = BITS_get( bits, 4);
	if(aspect == 0x15 ){
		int num = BITS_get( bits, 8); // par_width
		int den = BITS_get( bits, 8); // par_height
DBG serprintf("aspect : %d X %d\r\n", num, den );			
	} else {
DBG serprintf("aspect : %d\r\n", aspect );			
	}

	if ( BITS_get1(bits) )  { 
DBG serprintf("VOL_CONTROL\r\n");
		if(BITS_get(bits, 2) != 1){
DBG serprintf("illegal chroma format\r\n");
		}
		int low_delay = BITS_peek1(bits);
DBG serprintf("LOW DELAY: %d\r\n", low_delay );
		// force low delay bit always to zero
		BITS_poke1( bits, 0 );
		BITS_get1( bits );
	}
}

// *****************************************************************************
//
//	MPG4_fix_vol_header
//
// *****************************************************************************
void MPG4_fix_vol_header( UCHAR *data, int size )
{
	int 	count = 0;
	UINT	code = 0;
	UCHAR 	*start = data;
	
	while( count < size - 4 ) {
		code = ( code << 8 ) | *data++;
		if( code == 0x00000120 ) {
DBG serprintf("found vol header at %d\r\n", data - start - 4);			
			_patch_vol_header( data );
		}
		count ++;
	}
}

static const int h263_par[16][2] = {
	{ 0,  1},
	{ 1,  1},
	{12, 11},
	{10, 11},
	{16, 11},
	{40, 33},
	{ 0,  1},
	{ 0,  1},
	{ 0,  1},
	{ 0,  1},
	{ 0,  1},
	{ 0,  1},
	{ 0,  1},
	{ 0,  1},
	{ 0,  1},
	{ 0,  1},
};

#define RECT_SHAPE     0
#define BIN_SHAPE      1
#define BIN_ONLY_SHAPE 2
#define GRAY_SHAPE     3

#define STATIC_SPRITE  1
#define GMC_SPRITE     2

// *****************************************************************************
//
//	_parse_vol_header
//
// *****************************************************************************
static void _parse_vol_header( VIDEO_PROPERTIES *video, const UCHAR *data, int len ) 
{
	BITS _bits;
	BITS *bits = &_bits;
	BITS_init( bits, (UCHAR*)data, len * 8 );

	BITS_get1( bits ); // random access
	int type = BITS_get(bits, 8);
	int vo_ver_id;
	if (BITS_get1( bits ) != 0) { /* is_ol_id */
		vo_ver_id = BITS_get( bits, 4); /* vo_ver_id */
		BITS_get( bits, 3); /* vo_priority */
	} else {
		vo_ver_id = 1;
	}
DBGP serprintf("vo type: %d\n", type);
DBGP serprintf("vo ver : %d\n", vo_ver_id );
	
	int aspect     = BITS_get( bits, 4);
	int aspect_num = 1;
	int aspect_den = 1;
	if(aspect == 0xf ){
		aspect_num = BITS_get( bits, 8); // par_width
		aspect_den = BITS_get( bits, 8); // par_height
DBGP serprintf("aspect : ext: %d X %d\r\n", aspect_num, aspect_den );			
	} else {
		aspect_num = h263_par[aspect][0];
		aspect_den = h263_par[aspect][1];
DBGP serprintf("aspect : %2d: %d X %d\r\n", aspect, aspect_num, aspect_den );			
	}

	if ( BITS_get1(bits) )  { 
DBGP serprintf("VOL_CONTROL\r\n");
		if(BITS_get(bits, 2) != 1){
DBGP serprintf("illegal chroma format\r\n");
		}
		int low_delay = BITS_peek1(bits);
DBGP serprintf("LOW DELAY: %d\r\n", low_delay );
		// force low delay bit always to zero
		BITS_poke1( bits, 0 );
		// and go to next bit.
		BITS_get1(bits);

		if(BITS_get1(bits)){ 		/* vbv parameters */
			BITS_get(bits, 15);   	/* first_half_bitrate */
			BITS_get1(bits);     		/* marker */
			BITS_get(bits, 15);   		/* latter_half_bitrate */
			BITS_get1(bits);     		/* marker */
			BITS_get(bits, 15);   		/* first_half_vbv_buffer_size */
			BITS_get1(bits);     		/* marker */
			BITS_get(bits, 3);    		/* latter_half_vbv_buffer_size */
			BITS_get(bits, 11);   		/* first_half_vbv_occupancy */
			BITS_get1(bits);     		/* marker */
			BITS_get(bits, 15);   		/* latter_half_vbv_occupancy */
			BITS_get1(bits);     		/* marker */
		}
	}
	
	int shape = BITS_get(bits, 2); /* vol shape */
	if( shape != RECT_SHAPE) {
DBGP serprintf("only rectangular vol supported\n");
	}
	if( shape == GRAY_SHAPE && vo_ver_id != 1 ) {
DBGP serprintf("Gray shape not supported\n");
		BITS_get(bits, 4);  //video_object_layer_shape_extension
	}

	BITS_get1( bits );

	int time_base_den = BITS_get(bits, 16);
	if(!time_base_den){
DBGP serprintf("time_base.den==0\r\n");
	}

	int time_increment_bits = alog2( time_base_den - 1) + 1;
    	if (time_increment_bits < 1)
        	time_increment_bits = 1;

	BITS_get1(bits );
	
	int time_base_num;
	if (BITS_get1(bits) != 0) {   /* fixed_vop_rate  */
		time_base_num = BITS_get(bits, time_increment_bits);
	}else {
		time_base_num = 1;
	}
DBGP serprintf("time_base num %d  den %d\r\n", time_base_num, time_base_den);

	if (shape != BIN_ONLY_SHAPE) {
		if (shape == RECT_SHAPE) {
	    		BITS_get1(bits);   /* marker */
	    		video->width = BITS_get(bits, 13);
	    		BITS_get1(bits);   /* marker */
	    		video->height = BITS_get(bits, 13);
	    		BITS_get1(bits);   /* marker */
		
			// ok, we got width and height, the rest is easy:
			video->fourcc   = VIDEO_FOURCC_MP4V;
			video->format   = VIDEO_FORMAT_MPG4;
			video->scale    = time_base_num;
			video->rate     = time_base_den;
			video->aspect_n = aspect_num;
			video->aspect_d = aspect_den;
			video->vol      = 1;
			video->valid    = 1;
		}
		
		int progressive_sequence = BITS_get1( bits ) ^ 1;
DBGP serprintf("MPG4: progressive_sequence %d\n", progressive_sequence );

		if ( !BITS_get1( bits ) ) { 				// OBMC Disable
DBGP serprintf( "MPG4: OBMC unsupported!\n" );
		}												
		int sprite_usage;
		if ( vo_ver_id == 1 ) {
			sprite_usage = BITS_get1( bits );	
		} else {
			sprite_usage = BITS_get( bits, 2 );	
		}

DBGP serprintf("MPG4: sprite_usage %d\n", sprite_usage );
		if ( sprite_usage == STATIC_SPRITE ) {
DBGP serprintf("MPG4: static sprites!!!\n" );
		} else if ( sprite_usage == STATIC_SPRITE ) {
DBGP serprintf("MPG4: GMC sprites!!!\n" );
		}
		if ( sprite_usage == STATIC_SPRITE || sprite_usage == GMC_SPRITE ) {
			video->fourcc = VIDEO_FOURCC_4GMC;
			video->format = VIDEO_FORMAT_MPG4GMC;
			if ( sprite_usage == STATIC_SPRITE ) {
				int width = BITS_get( bits, 13 );
				BITS_get1( bits );	
				int height = BITS_get( bits, 13 );
				BITS_get1( bits );
				int left = BITS_get( bits, 13 );
				BITS_get1( bits );
				int top = BITS_get( bits, 13 );
				BITS_get1( bits );
DBGP serprintf("MPG4: sprite %d/%d/%d/%d\n", width, height, left, top  );
			}
			int num_sprite_warping_points = BITS_get( bits, 6 );
DBGP serprintf("MPG4: num_sprite_warping_points %d\n", num_sprite_warping_points  );
			if ( num_sprite_warping_points > 3 ) {
DBGP serprintf("MPG4: num sprite_warping_points %d > 3!!!\n", num_sprite_warping_points );
			}
			int sprite_warping_accuracy  = BITS_get( bits, 2 );
			int sprite_brightness_change = BITS_get1( bits );
DBGP serprintf("MPG4: sprite_warping_accuracy   %d\n", sprite_warping_accuracy  );
DBGP serprintf("MPG4: sprite_brightness_change  %d\n",sprite_brightness_change   );
			if ( sprite_usage == STATIC_SPRITE ) {
				int low_latency_sprite = BITS_get1( bits );
DBGP serprintf("MPG4: low_latency_sprite        %d\n",low_latency_sprite   );
			}
		}
		video->sprite_usage = sprite_usage;

	}
DBGP serprintf("VOL: %d x %d \r\n", video->width, video->height );	
}

// *****************************************************************************
//
//	MPG4_get_video_props
//
// *****************************************************************************
int MPG4_get_video_props( VIDEO_PROPERTIES *video, const UCHAR *data, int size )
{
	int 	count = 0;
	UINT	code = 0;
	const UCHAR 	*start = data;
	
	while( count < size - 4 ) {
		code = ( code << 8 ) | *data++;
		if( code == 0x00000120 ) {
DBGP serprintf("found vol header at %d\r\n", data - start - 4);			
			_parse_vol_header( video, data, size - (data - start) );
			return 0;
		}
		count ++;
	}
	return 1;
}

// *****************************************************************************
//
//	MPG4_find_start_code
//
// *****************************************************************************
int MPG4_find_start_code( UCHAR *data, int size, int *type )
{
	int 	count = 0;
	UINT	code = 0;
	UCHAR 	*start = data;
	
	while( count < size - 4 ) {
		code = ( code << 8 ) | *data++;
		if( (code & 0xFFFFFF00) == 0x100 && count > 2 ) {
			if( type )
				*type = code & 0xFF;
			return data - start - 4;
		}
		count ++;
	}
	return -1;
}

// *****************************************************************************
//
//	MPG4_find_video_sc
//
// *****************************************************************************
uint8_t *MPG4_find_video_sc( uint8_t *p, int len )
{
	int i;

	for (i = 0; i < len; i++, p++) {
		if ( get32BE( p ) == 0x000001B0 ) 
			return p;
	}
	return NULL;
}

// *****************************************************************************
//
//	MPG4_findFrame
//
// *****************************************************************************
int MPG4_findFrame( UCHAR *data, int ofs, int size )
{
	int 	count = 0;
	UINT	code = 0;
	UCHAR 	*start = data;
	
	size -= ofs;
	data += ofs;

	while( count < size - 4 ) {
		code = ( code << 8 ) | *data++;
		if( code == 0x000001B6 && count > 3 ) {
//serprintf("found frame header at %d\r\n", data - start - 4);			
			return data - start - 4;
		}
		count ++;
	}
	return -1;
}

// *****************************************************************************
//
//	MPG4_get_frame_size
//
// *****************************************************************************
int MPG4_get_frame_size( UCHAR *data, int size, int fix_user_data )
{
	int 	count = 0;
	UINT	code = 0;
	int	first = 1;
	
	while( count < size - 4 ) {
		code = ( code << 8 ) | *data++;
		if( code == 0x000001B6 ) {
//serprintf("start at: %d\r\n", count );		
			if( first )
				first = 0;
			else
				return count - 3;
		} else if( fix_user_data && code == 0x000001B2 ) {
DBGP serprintf("found user_data\r\n");			
DBGP Dump( data, 32 );		
			// kill user data
			int   c = count;
			char *d = data;
			while( *d && c < size - 4 ) {
				*d++ = 'x';
				c++;
			}
		}
		
		count ++;
	}
	return 0;
}

// *****************************************************************************
//
//	_find_frame
//
// *****************************************************************************
static int _find_frame( UCHAR *data, int *pos, int max )
{
	unsigned int p = *pos;
	if( max == 0 )
		max = 65536;
			
	while( max-- > 0 ) {
		if ( data[ p     ] == 0x00 )
		if ( data[ p + 1 ] == 0x00 )
		if ( data[ p + 2 ] == 0x01 )
		if ( data[ p + 3 ] == 0xB6 ) {
			*pos = p;
			return 1;
		}
		p++;
	}
serprintf("MPG4:FrameNotFound\n");	
	return 0;
} 

// *****************************************************************************
//
//	MPG4_checkIFrame
//
// *****************************************************************************
int MPG4_checkIFrame( UCHAR *data, int max, int *new )
{
	int _new = 0;
	if( _find_frame( data, &_new, max ) ) {
		if ( (data[ _new + 4 ] & 0xC0) == 0 ) {
			if( new )
				*new = _new;
			return 1;
		}
	}
	return 0;	
}

// *****************************************************************************
//
//	MPG4_get_frame_type
//
// *****************************************************************************
int MPG4_get_frame_type( UCHAR *data, int max, int *type )
{
	int pos = 0;
	if( _find_frame( data, &pos, max ) ) {
		switch( (data[ pos + 4 ] & 0xC0) ) {
		case 0x00: *type = I_VOP; return 0;
		case 0x40: *type = P_VOP; return 0;
		case 0x80: *type = B_VOP; return 0;
		}
	}
	return 1;	
}

// ******************************************
//
//	MPEG4_get_VOL_len
//
// ******************************************
int MPG4_get_VOL_len( UCHAR *data, int size )
{
	static const UCHAR seq[] = { 0x00, 0x00, 0x01, 0x20 };
	
	int len = 0;
	
	while( len < size - 4 ) {
		if( !memcmp( data + len, seq, 4 ) ) {
			// VOL start code, break 
			break;
		}
		len++;
	}

	if( len >= size - 4 ) {
		// no VOL!
		return 0;
	}

	len ++;
	
	while( len < size - 4 ) {
		if( !memcmp( data + len, seq, 3 ) ) {
			// other start code, break 
			break;
		}
		len++;
	}
	
	if( len >= size - 4 ) {
		// no OTHER!
		return 0;
	}
		
	return len;
}

int MPG4_get_extradata( VIDEO_PROPERTIES *video, UCHAR *data, int size )
{
	int vol = MPG4_get_VOL_len( data, size );
	if( !vol )
		return 1;
	video->extraDataSize = MIN( vol, sizeof( video->extraData ) );
	memcpy( video->extraData, data, video->extraDataSize  );
	return 0;
}
#endif

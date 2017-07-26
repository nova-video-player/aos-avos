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
#include "wmv.h"
#include "get.h"
#include "astdlib.h"
#include "bits.h"
#include "util.h"

#include <string.h>

#define DBGP  if(Debug[DBG_PARSER])
#define DBGP4 if(Debug[DBG_PARSER]> 3)

int WMV_get_video_props( VIDEO_PROPERTIES *video )
{
	if( !video || !video->extraDataSize )
		return 1; 
	
	if( video->format != VIDEO_FORMAT_WMV3 && video->format != VIDEO_FORMAT_VC1 )
		return 1;
		
	int ofs = 0;
	if ( video->format == VIDEO_FORMAT_VC1 ) {
		// look for SEQ header in extradata:
		int i;
		for( i = 0; i < video->extraDataSize - 16; i++ ) {
			if( get32BE( video->extraData + i ) == 0x0000010F ) {
				ofs = i + 4;
				break;
			}
		}
	}

	BITS _bits;
	BITS *bits = &_bits;

	BITS_init( bits, video->extraData + ofs, video->extraDataSize - ofs );
	
	video->profile = BITS_get( bits, 2 );
	char *n = "";
	switch( video->profile ) {
	case WMV_PROFILE_MAIN:		n = "Main"; 	break;
	case WMV_PROFILE_COMPLEX:	n = "Complex"; 	break;
	case WMV_PROFILE_ADVANCED:	n = "Advanced"; break;
	case WMV_PROFILE_SIMPLE:	n = "Simple"; 	break;
	}
		
	strcpy( video->profile_name, n ); 
serprintf("WMV: head: %02X  profile: %d [%s]\n", video->extraData[ofs], video->profile, video->profile_name );
 
	if(video->profile == WMV_PROFILE_ADVANCED) {
		video->level      = BITS_get(bits, 3);
		__attribute__((unused)) int chromaformat  = BITS_get(bits, 2);
		__attribute__((unused)) int frmrtq_pp     = BITS_get(bits, 3);
		__attribute__((unused)) int bitrtq_pp     = BITS_get(bits, 5);
		__attribute__((unused)) int pp_flag       = BITS_get(bits, 1);
		int coded_width   = (BITS_get(bits, 12) + 1) << 1;
		int coded_height  = (BITS_get(bits, 12) + 1) << 1;
		__attribute__((unused)) int broadcast     = BITS_get(bits, 1);
		video->interlaced = BITS_get(bits, 1);
serprintf("VC1: level: %d, WxH %dx%d  interlaced: %d\n", video->level, coded_width, coded_height, video->interlaced);
		return 0;
	}
	
	UNUSED int res_y411   = BITS_get( bits, 1 );
	UNUSED int res_sprite = BITS_get( bits, 1 );

	UNUSED int frmrtq_postproc = BITS_get( bits, 3 );
	UNUSED int bitrtq_postproc = BITS_get( bits, 5 );
	UNUSED int loop_filter     = BITS_get( bits, 1 );

	UNUSED int res_x8    = BITS_get( bits, 1 );
	UNUSED int multires   = BITS_get( bits, 1 );
	UNUSED int res_fasttx = BITS_get( bits, 1 );

	UNUSED int fastuvmc    = BITS_get( bits, 1 );
	UNUSED int extended_mv = BITS_get( bits, 1 );
	UNUSED int dquant      = BITS_get( bits, 2 );
	UNUSED int vstransform = BITS_get( bits, 1 );

	UNUSED int res_transtab = BITS_get( bits, 1 );

	UNUSED int overlap = BITS_get( bits, 1 );

	UNUSED int resync_marker = BITS_get( bits, 1 );
	UNUSED int angered = BITS_get( bits, 1 );

	UNUSED int max_b_frames = BITS_get( bits, 3 );
	UNUSED int quantizer_mode = BITS_get( bits, 2 );

	UNUSED int finterpflag = BITS_get( bits, 1 );

	int res_rtm = res_sprite ? 0 : BITS_get( bits, 1 );
	
	if (!res_rtm) {
serprintf("\nWMV3 BETA version!\n");
		// force BETA format
		video->fourcc = VIDEO_FOURCC_WM3B;
		video->format = VIDEO_FORMAT_WMV3B;
	}

	return 0;
}

typedef struct __PACKED__ RCV_V2_HEADER {
	UCHAR	frames[3];
	UCHAR	flags;
	UINT	ext_len;
	UCHAR	extra[4];
	UINT	height;
	UINT	width;
	UINT	marker;
	UCHAR	hrd[3];
	UCHAR	level_cbr;
	UINT	reserved[2];
} RCV_V2_HEADER;

unsigned char *WMV_get_rcv_header( VIDEO_PROPERTIES *video, int *size )
{
	RCV_V2_HEADER *rcv;
	*size = sizeof(RCV_V2_HEADER);
	int ofs = 0;
	if ( video->format == VIDEO_FORMAT_VC1 ) {
		// look for SEQ header in extradata:
		int i;
		for( i = 0; i < video->extraDataSize - 16; i++ ) {
			if( get32BE( video->extraData + i ) == 0x0000010F ) {
				ofs = i;
				break;
			}
		}
		*size = video->extraDataSize-ofs;
		rcv = amalloc (*size);
		memcpy( rcv, video->extraData + ofs, *size);
	} else {
		*size = sizeof(RCV_V2_HEADER);
		rcv =  amalloc(*size);
		rcv->frames[0] = 0xFF;
		rcv->frames[1] = 0xFF;
		rcv->frames[2] = 0xFF;
		rcv->flags       = 0xC5;        // V2
		rcv->ext_len     = 4;
		memcpy( rcv->extra, video->extraData , 4);
		rcv->height      = video->height;
		rcv->width       = video->width;
		rcv->marker = 0xC;
		rcv->hrd[0] = rcv->hrd[1] = rcv->hrd[2] = 0x00;
		rcv->level_cbr = video->level << 4;
		rcv->reserved[0] = 0;
		rcv->reserved[1] = 0xFFFFFFFF;
	}
	DBGP serprintf("\nWMV_get_rcv_header:\n");
	DBGP Dump((unsigned char*)rcv, *size);
	return (unsigned char*)rcv;
}

void VC1_add_sync( CBE *cbe, int *out_size )
{
DBGP4 serprintf("VC1_add_sync\n");	
	static const UCHAR vc1_sync[4] = { 0x00, 0x00, 0x01, 0x0D };
	cbe_write( cbe, vc1_sync, 4 );
        *out_size += 4;
}

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

/*
	generic MPEG2 support stuff
*/

#include "global.h"
#include "types.h"
#include "debug.h"
#include "stream.h"
#include "cbe.h"
#include "mpeg2.h"
#include "mpg4.h"
#include "h264.h"
#include "get.h"

#include <string.h>

#ifdef CONFIG_MPEG2
extern int stream_no_reorder;

#ifdef STANDALONE
#define DBG if(0)
#else
#define DBG  if(Debug[DBG_MANGLER] > 1)
#define DBGP if(Debug[DBG_PARSER])
#endif

#define	EXTENSION_START_CODE		0x000001B5
#define	PICTURE_START_CODE		0x00000100
#define PICTURE_CODING_EXT_ID		0x8

static const int aspect_ratio_mpeg1[15] = {
    10000,
     6735,
     7031,
    
     7615,
     8055,
     8437,
     8935,

     9157,
     9815,
    10255,
    10695,

    10950,
    11575,
    12015,
};

// see ISO/IEC 13818-2 Table 6-3
static const int aspect_ratio_numerator[4]   = { 1, 4, 16, 221 };
static const int aspect_ratio_denominator[4] = { 1, 3,  9, 100 };

// see ISO/IEC 13818-2 Table 6-4
static const int frame_rate_rate[8] = { 24000, 24, 25, 30000, 30, 50, 60000, 60 };
static const int frame_rate_scale[8] = { 1001,  1,  1,  1001,  1,  1,  1001,  1 };

static int MPEG2_get_video_props( VIDEO_PROPERTIES *video, const UCHAR *p, int mpeg )
{
	int horizontal_size_value    = ( get8( p + 4 )         << 4) + (get8( p + 5 ) >> 4);
	int vertical_size_value      = ((get8( p + 5 ) & 0x0F) << 8) + (get8( p + 6 )     );
	int aspect_ratio_information = ( get8( p + 7 ) & 0xF0) >> 4;
	int frame_rate_code          =   get8( p + 7 ) & 0x0F;
	int bitrate                  = 0; // this bitrate is highly inaccurate: (get24BE( p + 8 ) >> 6) * 400;
	
	if( !mpeg ) {
serprintf("MPEG%d: error\r\n", mpeg);		
		return 1;
	}
	video->width  = horizontal_size_value;
	video->height = vertical_size_value;
DBGP serprintf("MPEG%d: aspect info: %d\r\n", mpeg, aspect_ratio_information);		
	if ( aspect_ratio_information == 0 ) {
serprintf("MPEG%d: bad aspect!\r\n", mpeg);		
		return 1;
	} else if( mpeg == 1 ) {
		// MPEG1
		video->aspect_n = 10000;
		video->aspect_d = aspect_ratio_mpeg1[ aspect_ratio_information - 1 ];
	} else if ( aspect_ratio_information == 1 ) {
		// MPEG2, special case for 1:1
		video->aspect_n = 1;
		video->aspect_d = 1;
	} else if ( aspect_ratio_information > 1 && aspect_ratio_information < 5 ) {
		// MPEG2
		video->aspect_n = video->height * aspect_ratio_numerator  [ aspect_ratio_information - 1 ] 
		                               / aspect_ratio_denominator[ aspect_ratio_information - 1 ]; 
		video->aspect_d = video->width;
	} else {
serprintf("MPEG2: aspect error: %d\r\n", aspect_ratio_information );		
		return 1;
	}
	
	if ( frame_rate_code > 0 && frame_rate_code < 9 ) {
		video->scale = frame_rate_scale[ frame_rate_code - 1 ];
		video->rate  = frame_rate_rate [ frame_rate_code - 1 ];
	} else {
serprintf("MPEG%d: frame rate error\r\n", mpeg);		
		return 1;
	}

	video->bytesPerSec = bitrate / 8;
	video->fourcc = (mpeg == 1) ? VIDEO_FOURCC_MPG1 : VIDEO_FOURCC_MPG2;
	video->format = VIDEO_FORMAT_MPEG;
	video->valid  = 1;

	return 0;
}

static int MPEG2_find_psc( UCHAR *data, int size, int *tref, int *type )
{
	int 	count = 0;
	UINT	code = 0;
	
	while( count < size - 4 ) {
		code = ( code << 8 ) | *data++;
		if( code == PICTURE_START_CODE ) {
			if( tref )
				*tref = ( (*data) << 2) | (*(data + 1) >> 6 );
			if( type )
				*type = ((*(data + 1) >> 3) & 0x07) - 1;
			return count - 3;
		}
		count ++;
	}
	return -1;
}

static int MPEG2_find_coding_ext( UCHAR *data, int size )
{
	int 	count = 0;
	UINT	code = 0;
	
	while( count < size - 4 ) {
		code = ( code << 8 ) | *data++;
		if( code == EXTENSION_START_CODE && (*data >> 4) == PICTURE_CODING_EXT_ID  ) {
			return count - 3;
		}
		count ++;
	}
	return -1;
}

// ******************************************
//
//	MPEG2_get_SEQ_len
//
// ******************************************
int MPEG2_get_SEQ_len( UCHAR *data, int size )
{
	static const UCHAR seq[] = { 0x00, 0x00, 0x01, 0xB3 };
	static const UCHAR gsc[] = { 0x00, 0x00, 0x01, 0xB8 };
	static const UCHAR psc[] = { 0x00, 0x00, 0x01, 0x00 };
	
	if( memcmp( data, seq, 4 ) ) {
	 	// no SEQ at start!
		return 0;
	} 

	int len = 4;
	
	while( len < size - 4 ) {
		if( !memcmp( data + len, gsc, 4 ) ) {
			// GroupStartCode, break 
			break;
		}
		if( !memcmp( data + len, psc, 4 ) ) {
			// PictureStartCode, break 
			break;
		}
		len++;
	}
	
	if( len == size - 4 ) {
		// no SEQ!
		return 0;
	}
		
	return len;
}

// ******************************************
//
//	MPEG2_find_video_sc
//
// ******************************************
static uint8_t *MPEG2_find_video_sc( uint8_t *p, int len )
{
	int i;

	for (i = 0; i < len; i++, p++) {
		if ( get32BE( p ) == 0x001B3 ) 
			return p;
	}
	return NULL;
}

// ************************************************
//
//!     MPEG_get_video_props
//!
//! 	get video parameters from stream, look starting at p for
//!
//!	@param video		pointer to VIDEO_PROPERTIES structure with current stream information
//!	@param new		pointer to VIDEO_PROPERTIES structure with NEW stream information
//!	@param p		pointer to ES video stream in memory
//!	@param maxsearch	number of bytes to search for start code
//!	@param buflen		size of buffer
//!
//!	@return 1 if no start code or no valid information was found
// ************************************************
int MPEG_get_video_props( int format, VIDEO_PROPERTIES *new, uint8_t *p, int maxsearch, int buflen )
{
	memset( new, 0, sizeof( VIDEO_PROPERTIES ) );
	
	if ( format == VIDEO_FORMAT_MPEG ) {
		uint8_t *vp = MPEG2_find_video_sc( p, maxsearch );
		 
		 if ( !vp )
		 	return 1;
			
		 // MPEG2 sequence_header found

		if (MPEG2_get_video_props( new, vp, 2 )) {
			return 1;
		} 
	}
#ifdef CONFIG_MPG4
	else if ( format == VIDEO_FORMAT_MPG4 ) {
		uint8_t *vp = MPG4_find_video_sc( p, maxsearch );
		 
		 if ( !vp )
		 	return 1;
			
		int len = buflen - ( vp - p );
		// MPG4 VOL header found
		if( MPG4_get_video_props( new, vp, len ) ) {
			return 1;
		} 
	}
#endif
#ifdef CONFIG_H264
	else if ( format == VIDEO_FORMAT_H264 ) {
		int h264 = H264_find_AUD( p, maxsearch );
		
		if ( h264 == -1 )
		 	return 1;
		
DBGP serprintf("H264 %d maxsearch %d\r\n", h264, buflen - h264 - 8 );
		if ( H264_get_video_props( new, p + h264, buflen - h264 - 8, &new->sps ) ) {
//Dump( p + h264, buflen - h264 - 8);
DBGP serprintf(" no NAL_SPS found\r\n");							
			return 1;
		}
	}
#endif
	else {
DBGP		serprintf("no valid video format %d specified!\n", format);
		return 1;
	}

	// reduce the aspect ratio as much as possible
	av__reduce( &new->aspect_n, &new->aspect_d );  

	return 0;
}

// ************************************************
//
//!     MPEG_check_video_changed
//!
//! 	only overwrite the parameters if they are different from last call
//!
//!	@param video		pointer to VIDEO_PROPERTIES structure with current stream information
//!	@param new		pointer to VIDEO_PROPERTIES structure with NEW stream information
//!	@param changed		if available, will be set to 1 if the video properties changed
//!
//!	@return 1 if something changed
// ************************************************
int MPEG_check_video_changed( VIDEO_PROPERTIES *video, VIDEO_PROPERTIES *new, int *changed )
{
	if( changed ) {
		*changed = 0;
	}

	if (	new->width     == video->width    &&
		new->height    == video->height   &&
		new->aspect_n  == video->aspect_n &&
		new->aspect_d  == video->aspect_d &&
		new->scale     == video->scale    && 
		new->rate      == video->rate     &&
		new->fourcc    == video->fourcc   &&
		new->profile   == video->profile  &&
		new->level     == video->level     
	) {
		return 0; // nothing changed...
	}


	if ( video->valid == 1 ) {
		// we already have valid video, report change
serprintf("\r\nMPEG: video props changed:\r\n");
serprintf("%4d %4d\n", new->width     , video->width    );
serprintf("%4d %4d\n", new->height    , video->height   ); 
serprintf("%4d %4d\n", new->aspect_n  , video->aspect_n );
serprintf("%4d %4d\n", new->aspect_d  , video->aspect_d );
serprintf("%4d %4d\n", new->scale     , video->scale    );
serprintf("%4d %4d\n", new->rate      , video->rate     );
serprintf("%.4s %4.4s\n", &new->fourcc    , &video->fourcc   );
serprintf("%4d %4d\n", new->profile   , video->profile  );
serprintf("%4d %4d\n\n", new->level     , video->level    );
		if( changed ) {
			*changed = 1;
		}
	}

	video->width	= new->width;
	video->height	= new->height;
	video->aspect_n = new->aspect_n;
	video->aspect_d = new->aspect_d; 
	video->scale	= new->scale;
	video->rate	= new->rate;
	video->fourcc	= new->fourcc;
	video->sps	= new->sps;
	video->profile	= new->profile;
	video->level	= new->level;
	video->bytesPerSec = new->bytesPerSec;

	if( video->rate && video->scale ) {
		video->msPerFrame   = 1000 * (UINT64)video->scale / (UINT64)video->rate;		
		video->framesPerSec = (UINT64)video->rate / (UINT64)video->scale;
serprintf("\n\n\nMSPERFRAME 3 %d / %d\n\n\n\n", video->msPerFrame, video->framesPerSec );
	}
	show_video_props( video );

	return 1;
}

#define TREF_MAX 1024
struct m {
	int valid;
	int time;
	int pic;
	int tff;
	int rff;
	int pro;
};

typedef struct M_PRIV 
{
	int fields;
	int last_tff;
	int ref_time;
	int last_time;
	int fake_time;

	int ref_count;
	
	struct m m[TREF_MAX];
} M_PRIV;

static int _init( STREAM *s, int time )
{
	if( s->video->reorder_pts )
		return 0;
		
	if( !s->mangler_priv ) {
		s->mangler_priv = acalloc( 1, sizeof( M_PRIV ) );
	}
	M_PRIV *p = s->mangler_priv;
	
	int i;
serprintf("init_mangler_MPEG2( %d )\r\n", time );	
	for ( i = 0; i < TREF_MAX; i++ ) {
		p->m[i].valid = 0;
		p->m[i].time  = -1;
	}
	p->fields    = 0;
	p->ref_time  = time;
	p->last_time = 0;
	p->ref_count = 0;	
	return 0;
}

static int _pre( STREAM *s, CBE *cbe, STREAM_CDATA *cdata )
{
	if( s->video->reorder_pts )
		return 0;

	M_PRIV *p = s->mangler_priv;

	int tref = -1;
	int type;
	int psc = MPEG2_find_psc( cbe_get_p( cbe ), cdata->size, &tref, &type );
	int ofs = -1;
	
	if( s->video->subfmt == VIDEO_SUBFMT_MPEG2 )
		ofs = MPEG2_find_coding_ext( cbe_get_p( cbe ), cdata->size );

	if( psc != -1 ) {
		int pic = 0;
		int tff = 0;
		int rff = 0;
		int pro = 0;
		if( ofs != -1 ) {
			//
			// MPEG2
			//
			UCHAR *ext = cbe_get_p( cbe ) + ofs;
			pic = (ext[6]      ) & 0x02;
			tff = (ext[7] >> 7 ) & 0x01;
			rff = (ext[7] >> 1 ) & 0x01;
			pro = (ext[8] >> 7 ) & 0x01;
		}
DBG serprintf("%s", cdata->time == -1 ? "  " : "TS" );	
		if( tref < TREF_MAX ) {
			p->m[tref].valid = 1;
			if( p->m[tref].pic == 0 && pic == 0x02 && cdata->time == -1 && p->m[tref].time != -1 ) {
				// keep the time from the previous one
			} else {
				p->m[tref].time  = cdata->time;
			}
			p->m[tref].pic   = pic;
			p->m[tref].tff   = tff;
			p->m[tref].rff   = rff;
			p->m[tref].pro   = pro;
		}
//DBG serprintf("  ofs %03d/%3d", psc, ofs );
DBG serprintf("  %c %2d  trpp %d%d%d%d]", frame_type( type ), tref, tff, rff, pro, pic );
		if( cdata->time == -1 ) {
			cdata->time = ++p->fake_time;
		} else {
			p->fake_time = cdata->time;
		}
DBG serprintf("  %8d ", cdata->time );
		
		cdata->user_ID  = tref;
		cdata->frm_type = type;
	} else {
DBG serprintf("  NOPSC! \r\n" );
DBG Dump( cbe_get_p( cbe ), 32 );
	}	
	return 0;
}

static VIDEO_FRAME **_reorder( STREAM *s, VIDEO_FRAME **p_frame )
{
	M_PRIV *p = s->mangler_priv;

	if( (*p_frame)->type == B_VOP ) {
		if( p->ref_count < 2 ) {
DBG serprintf(" ! ");
			(*p_frame)->valid = 0;
		} else {
DBG serprintf(" = ");
		}
		// B frames we just output
		return p_frame; 
	} else if( (*p_frame)->type == I_VOP ) {
		p->ref_count ++;
	} else if( (*p_frame)->type == P_VOP && p->ref_count ) {
		p->ref_count ++;
	}
DBG serprintf(" > ");
	
	return p_frame;
}

static int _post( STREAM *s, VIDEO_FRAME **p_frame )
{
	if( s->video->reorder_pts )
		return 0;

	M_PRIV *p = s->mangler_priv;
	VIDEO_FRAME *frame = *p_frame;

	if( !frame ) {
DBG serprintf(" -NF- \r\n");
		return 0;	
	}

DBG serprintf("<%c %2d  %08X> ", frame_type(frame->type), frame->user_ID, frame->data[0] );
	
	if( !stream_no_reorder) {
		p_frame = _reorder( s, p_frame );
	}
	frame = *p_frame;

	if( !frame->valid ) {
DBG serprintf("[ -- \r\n");
		return 0;	
	}
	int tref = frame->user_ID;

DBG serprintf("[ %c %2d  %08X", frame_type(frame->type), tref, frame->data[0] );

	if( tref < TREF_MAX && p->m[tref].valid ) {
		if( p->m[tref].time != -1 ) {
			p->fields   = 0;
			p->ref_time = p->m[tref].time;
			frame->time = p->m[tref].time;
DBG serprintf(" TS");
		} else {
			if( p->last_tff == p->m[tref].tff ) 
				p->fields += 2;
			else
				p->fields += 3;
			
			// calculate where this field should be in time
			frame->time = p->ref_time + (p->fields * 500 * s->video->scale / s->video->rate );
DBG serprintf("   ");
		}
		// remember last tff
		p->last_tff = p->m[tref].tff;
	} else {
DBG serprintf("  REF");
		// we must not leave an undefined "-1" time!
		frame->time = p->ref_time;	
	}
	
DBG serprintf(" %3d | %8d ]\r\n", frame->time - p->last_time, frame->time );

	if( frame->time == -1 ) {
serprintf("\r\nMPG2 MNG ERROR  ref %d  fields %d\r\n", p->ref_time, p->fields );
	}	
	p->last_time = frame->time;
	return 0;
}

static int _flush( STREAM *s )
{
	if( s->video->reorder_pts )
		return 0;
	
	M_PRIV *p = s->mangler_priv;
	p->ref_count = 0;
	return 0;
}

STREAM_VIDEO_MANGLER stream_video_mangler_MPEG2 = 
{
	"MPEG2",
	_init,
	_pre,
	_post,
	_flush,
};
#endif // of CONFIG_MPEG2

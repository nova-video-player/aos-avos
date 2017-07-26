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
	generic RealVideo support stuff
*/

#include "global.h"
#include "types.h"
#include "debug.h"
#include "stream.h"
#include "cbe.h"
#include "get.h"
#include "bits.h"

#ifdef CONFIG_REALVIDEO

#ifdef STANDALONE
#define DBGV if(0)
#else
#define DBGMNG if(Debug[DBG_MANGLER])
#define DBGV   if(Debug[DBG_CV])
#endif

// from RealFormatSDK
#define RV40_SPO_BITS_NUMRESAMPLE_IMAGES    	0x00070000
#define RV40_SPO_BITS_NUMRESAMPLE_IMAGES_SHIFT 	16

int realvideo_get_dimensions( VIDEO_PROPERTIES *video, UINT32 *dimensions )
{
	if( video->extraDataSize < 8 ) {
		return 0;
	}
	
	int ulInvariants    = get32BE( video->extraData );

	// set the sizes!
	int num_sizes = (ulInvariants & RV40_SPO_BITS_NUMRESAMPLE_IMAGES) >> RV40_SPO_BITS_NUMRESAMPLE_IMAGES_SHIFT;
DBGV serprintf("num_sizes: %d\r\n", num_sizes);
	dimensions[0] = video->width;		
	dimensions[1] = video->height;		
        	
	// extract dimensions
	UCHAR *data = video->extraData + 8;
	int i;
	for( i = 0; i < num_sizes; i ++ ) {
		dimensions[2 * i + 2] = *data++ << 2;			
		dimensions[2 * i + 3] = *data++ << 2;			
	}
		
	for( i = 0; i < num_sizes + 1; i ++ ) {
DBGV serprintf("\t%d: %3d x %3d\r\n", i, dimensions[2 * i], dimensions[2 * i + 1]  );			
	}
	
	return num_sizes;
}

int UNUSED realvideo40_get_pts( UCHAR *data, int *type )
{
	BITS _bits;
	BITS *bits = &_bits;

	BITS_init( bits, (UCHAR*)data, 16 * 8 );

	*type = -1;

	if( BITS_get( bits, 1 ) ) {
serprintf("RV40 slice error\n");
		return -1;
	}

	*type  = BITS_get( bits, 2 ); 
	UNUSED int quant = BITS_get( bits, 5 );
	if( BITS_get( bits, 2  ) ) {
serprintf("RV40 slice error\n");
		return -1;
	}
	UNUSED int vlc = BITS_get( bits, 2 ); 
	BITS_get( bits, 1 );

	return BITS_get( bits, 13 ); 
}

int UNUSED realvideo30_get_pts( UCHAR *data, int *type )
{
	BITS _bits;
	BITS *bits = &_bits;

	BITS_init( bits, (UCHAR*)data, 16 * 8);

	*type = -1;

	if( BITS_get( bits, 3 ) ) {
serprintf("RV30 slice error\n");
		return -1;
	}

	*type  = BITS_get( bits, 2 ); 
	if( BITS_get( bits, 1  ) ) {
serprintf("RV30 slice error\n");
		return -1;
	}
	UNUSED int quant = BITS_get( bits, 5 );
	BITS_get1( bits );

	return BITS_get( bits, 13 ); 
}

#ifndef STANDALONE
typedef struct M_PRIV 
{
	int ref1_ts;
	int ref1_pts;
	int ref2_ts;
	int ref2_pts;
} M_PRIV;

static int _init( STREAM *s, int time )
{
	if( !s->mangler_priv ) {
		s->mangler_priv = acalloc( 1, sizeof( M_PRIV ) );
	}
	M_PRIV *p = s->mangler_priv;

	p->ref1_ts = -1;
	p->ref2_ts = -1;
DBGMNG serprintf("init_mangler_REAL( %d )\r\n", time );	
	return 0;
}

static int _pre( STREAM *s, CBE *cbe, STREAM_CDATA *cdata )
{
	M_PRIV *p = s->mangler_priv;

	UCHAR *data = cbe_get_p( cbe );
	int pts = 0;
	int type = -1;
	if( s->video->format == VIDEO_FORMAT_RV40 ) {
		pts = realvideo40_get_pts( data, &type );
	} else if( s->video->format == VIDEO_FORMAT_RV30 ) {
		pts = realvideo30_get_pts( data, &type );
	}
	if( type != -1 ) {
		cdata->frm_type = type ? type - 1 : I_VOP;
	}
	int diff = 0;
	int out_ts = cdata->time;
	if( cdata->frm_type == B_VOP ) {
		if( p->ref2_ts != -1 ) {
			diff = (pts - p->ref2_pts + 8192) & 0x1FFF;
			out_ts = p->ref2_ts + diff;
		}
	} else {
		p->ref2_ts  = p->ref1_ts;
		p->ref2_pts = p->ref1_pts;
		p->ref1_pts = pts;
		p->ref1_ts  = cdata->time;
	}
DBGMNG serprintf("pre: typ %c  ts %8d  pts %4d  diff %4d  out %8d\n", frame_type( cdata->frm_type ), cdata->time, pts, diff, out_ts );
	cdata->time = out_ts;
	return 0;
}

static int _post( STREAM *s, VIDEO_FRAME **p_frame )
{
	return 0;
}

static int _flush( STREAM *s )
{
	return 0;
}

STREAM_VIDEO_MANGLER stream_video_mangler_REAL = 
{
	"REAL",
	_init,
	_pre,
	_post,
	_flush,
};
#endif

#endif


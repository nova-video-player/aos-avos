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
#include "stream_filter_audio.h"
#include "debug.h"
#include "atime.h"
#include "compress.h"
#include "astdlib.h"
#include "util.h"

struct ctx {
	struct Compressor *cmp;
	struct CompressorConfig *cfg;
	int level;
	int nightmode;
};

int _delete( STREAM_FILTER_AUDIO *f )
{
serprintf("facomp: delete\n" );
	if( f && f->priv ) {
		struct ctx *ctx = f->priv;		
	        Compressor_delete(ctx->cmp);
	}
	afree(f);
	return 0;
}

static void setup( struct ctx *ctx )
{	
  /* When there is no nightmode:
   * look for highest peak value in the last 16384 audio packet (which correspond to 98 minutes for a 44.1kHz with 256 stereo audio samples packets
   * allow maximum highest gain dince gain will move really slowly
   * Disable is gain unchanged */
  
  /* When there is nightmode on:
   * look for highest peak value in the last 10 audio packet (which correspond to 50 msec for a 44.1kHz with 256 stereo audio samples packets
   * Smooth by a factor of 256
   * allow maximum highest gain dince gain will move really slowly
   * Disable is gain unchanged */
  
  /* To get the duration of the history, compute t = 256*history/44100 */
	int p[8][4] = {
	{     0,   0, 8, 65536 },	// 0 = off;
	{ 20480,   0, 8, 65536 }, // 1
	{ 28672,   0, 8, 65536 }, // 2
	{ 32767,   0, 8, 65536 }, // 3
	{ 16384,   8, 8, 65536 },	// 0 + nightmode= on;
	{ 20480,  10, 8, 65536 }, // 1 + nightmode
	{ 28672,  12, 8, 65536 }, // 2 + nightmode
	{ 32767,  16, 8, 65536 }, // 3 + nightmode
	};

	int lvl = MAX( 0, MIN( 7, ctx->level + 4*ctx->nightmode ));

serprintf("lvl %d  tgt %d  maxg %d  sm %d  hist %d\n", lvl, p[lvl][0], p[lvl][1], p[lvl][2], p[lvl][3] );
 	
	ctx->cfg->target  = p[lvl][0];
	ctx->cfg->maxgain = p[lvl][1];
	ctx->cfg->smooth  = p[lvl][2];
	Compressor_setHistory(ctx->cmp, p[lvl][3]);
}

static int _open( STREAM_FILTER_AUDIO *f, AUDIO_PROPERTIES *audio )
{
serprintf("facomp: open\n" );
	struct ctx *ctx = acalloc( 1, sizeof( struct ctx ) );
	ctx->cmp = Compressor_new(0);
	f->priv = ctx;

	ctx->cfg = Compressor_getConfig(ctx->cmp);
	
	setup( ctx );	

	return 0;
}

static int _close( STREAM_FILTER_AUDIO *f ) 
{
serprintf("facomp: close\n" );
	return 0;
}

static int _filter( STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame )
{
	struct ctx *ctx = f->priv;		
	
	if( (ctx->level + 4*ctx->nightmode) > 0 ) {
		Compressor_Process_int16(ctx->cmp, (int16_t*)frame->data, frame->size / 2 );
	}
	return 0;
}

static int _flush( STREAM_FILTER_AUDIO *f )
{
//serprintf("facomp: flush\n" );
	return 0;
}

static int _set_param( STREAM_FILTER_AUDIO *f, void *params, void *night_on )
{
	int *level = params;
	int *nightmode = night_on;
	struct ctx *ctx = f->priv;		
serprintf("facomp: level %d\n", *level );
	if( (ctx->level != *level ) || (ctx->nightmode != *nightmode)) {
		ctx->level = *level;
		ctx->nightmode = *nightmode;
		setup( ctx );	
	}
	return 0;
}

int _delay( STREAM_FILTER_AUDIO *f )
{
	return 0;
}

STREAM_FILTER_AUDIO *stream_filter_audio_compress_new( void ) 
{ 
	STREAM_FILTER_AUDIO *f = acalloc( 1, sizeof( STREAM_FILTER_AUDIO ) );
	
	if( !f )
		return NULL;

	static char name[] = "compress";
	f->name    = name;
	f->delete  = _delete;
	f->open    = _open;
	f->close   = _close;
	f->filter  = _filter;
	f->flush   = _flush;
	f->set_param = _set_param;
	f->delay   = _delay;

	return f;
}


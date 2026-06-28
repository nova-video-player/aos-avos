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
 * Audio Compression Filter for Nova Video Player
 *
 * This filter provides dynamic range compression for two main use cases:
 * 1. Audio Boost - General volume enhancement for quiet content
 * 2. Night Mode - Dialogue enhancement for night viewing (compress dynamic range)
 *
 * Android UI Integration:
 * - Audio Boost toggle: 0=OFF, 3=ON (VideoPreferencesActivity)
 * - Night Mode toggle: 0=OFF, 1=ON (VideoPreferencesActivity)
 * - Settings stored in SharedPreferences and passed via JNI to avos_mp_setaudiofilter()
 * - Real-time parameter changes supported without audio interruption
 *
 * Filter Modes (lvl = level + 4*nightmode):
 * - lvl=0: OFF (no processing)
 * - lvl=3: Audio Boost only (target=100%, maxgain=0dB, history=98min)
 * - lvl=4: Night Mode only (target=50%, maxgain=8dB, history=98min)
 * - lvl=7: Audio Boost + Night Mode (target=100%, maxgain=16dB, history=98min)
 *
 * Note: Current Audio Boost (lvl=3) has maxgain=0 which limits effectiveness.
 *       Night Mode uses long history (98min) which may be slow for dialogue response.
 */

#include "global.h"
#include "stream_filter_audio.h"
#include "debug.h"
#include "atime.h"
#include "compress.h"
#include "astdlib.h"
#include "util.h"
#include <pthread.h>

#define DBG if(0)

struct ctx {
	struct Compressor *cmp;          // AudioCompress library compressor instance
	struct CompressorConfig *cfg;    // Configuration structure for compressor parameters
	int level;                       // Audio boost level (0=OFF, 3=ON from Android UI)
	int nightmode;                   // Night mode flag (0=OFF, 1=ON from Android UI)
	pthread_mutex_t mutex;           // Protects configuration updates against concurrent rendering
};

int _delete( STREAM_FILTER_AUDIO *f )
{
	serprintf("facomp: delete\n" );
	if( f && f->priv ) {
		struct ctx *ctx = f->priv;
		if( ctx->cmp ) {
			Compressor_delete(ctx->cmp);
		}
		pthread_mutex_destroy( &ctx->mutex );
		afree(f->priv);  // Fix memory leak - free the context
	}
	if( f ) {
		afree(f);
	}
	return 0;
}

static void setup( struct ctx *ctx )
{
	/*
	 * AudioCompress Parameter Configuration
	 *
	 * History calculation: duration = 256 * history / 44100 Hz
	 * - history=65536 -> ~381 seconds (6.3 minutes)
	 * - history=1024  -> ~5.9 seconds
	 *
	 * Target: peak level goal (0-32767, where 32767=100% amplitude)
	 * Maxgain: maximum gain boost in dB (0=no boost)
	 * Smooth: smoothing factor for gain changes (8=default)
	 * History: number of audio packets to analyze for peak detection
	 */

	// Parameter table: [target, maxgain, smooth, history]
	// Android UI uses only levels 0, 3, 4, 7 (other levels unused)
	int p[8][4] = {
		{     0,   0, 8, 65536 },  // lvl=0: OFF (nightmode=0, level=0)
		{ 20480,   0, 8, 65536 },  // lvl=1: UNUSED
		{ 28672,   0, 8, 65536 },  // lvl=2: UNUSED
		{ 32767,   0, 8, 65536 },  // lvl=3: Audio Boost ON (nightmode=0, level=3)
		{ 16384,   8, 8, 65536 },  // lvl=4: Night Mode ON (nightmode=1, level=0)
		{ 20480,  10, 8, 65536 },  // lvl=5: UNUSED
		{ 28672,  12, 8, 65536 },  // lvl=6: UNUSED
		{ 32767,  16, 8, 65536 },  // lvl=7: Night Mode + Audio Boost (nightmode=1, level=3)
	};

	int lvl = MAX( 0, MIN( 7, ctx->level + 4*ctx->nightmode ));

	// Log current configuration with mode description
	const char* mode_desc[] = {
		"OFF", "UNUSED", "UNUSED", "Audio Boost",
		"Night Mode", "UNUSED", "UNUSED", "Night Mode + Audio Boost"
	};

	DBG serprintf("facomp: %s (lvl=%d) -> target=%d maxgain=%ddB smooth=%d history=%d (%.1fsec)\n",
		 mode_desc[lvl], lvl, p[lvl][0], p[lvl][1], p[lvl][2], p[lvl][3],
		 (float)(256 * p[lvl][3]) / 44100.0f);

	// Apply configuration to AudioCompress library
	if( ctx->cfg ) {
		ctx->cfg->target  = p[lvl][0];
		ctx->cfg->maxgain = p[lvl][1];
		ctx->cfg->smooth  = p[lvl][2];
	}

	if( ctx->cmp ) {
		Compressor_setHistory(ctx->cmp, p[lvl][3]);
	}
}

static int _open( STREAM_FILTER_AUDIO *f, AUDIO_PROPERTIES *audio )
{
	DBG serprintf("facomp: open - channels=%d sampleRate=%d bitsPerSample=%d\n",
		 audio->channels, audio->samplesPerSec, audio->bitsPerSample);

	// Allocate filter context
	struct ctx *ctx = acalloc( 1, sizeof( struct ctx ) );
	if( !ctx ) {
		serprintf("facomp: failed to allocate context\n");
		return -1;
	}

	pthread_mutex_init( &ctx->mutex, NULL );

	// Initialize AudioCompress library
	ctx->cmp = Compressor_new(0);
	if( !ctx->cmp ) {
		serprintf("facomp: failed to create compressor\n");
		pthread_mutex_destroy( &ctx->mutex );
		afree(ctx);
		return -1;
	}

	ctx->cfg = Compressor_getConfig(ctx->cmp);
	if( !ctx->cfg ) {
		serprintf("facomp: failed to get compressor config\n");
		Compressor_delete(ctx->cmp);
		pthread_mutex_destroy( &ctx->mutex );
		afree(ctx);
		return -1;
	}

	f->priv = ctx;

	// Initialize with default settings (OFF mode)
	ctx->level = 0;
	ctx->nightmode = 0;
	setup( ctx );

	DBG serprintf("facomp: AudioCompress filter initialized successfully\n");
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

	// Safety checks
	if( !ctx || !ctx->cmp || !frame || !frame->data || frame->size <= 0 ) {
		return 0;
	}

	pthread_mutex_lock( &ctx->mutex );

	// Only process if compression is enabled (level > 0 or nightmode > 0)
	if( (ctx->level + 4*ctx->nightmode) > 0 ) {
		// AudioCompress processes 16-bit signed integer samples
		// frame->size is in bytes, so divide by 2 for sample count
		int sample_count = frame->size / 2;

		Compressor_Process_int16(ctx->cmp, (int16_t*)frame->data, sample_count);

		// Debug: Log processing activity occasionally
		static int process_counter = 0;
		if( ++process_counter % 1000 == 0 ) {  // Every ~20 seconds at 48kHz
			int lvl = ctx->level + 4*ctx->nightmode;
			DBG serprintf("facomp: processed 1000 frames (lvl=%d, samples=%d)\n", lvl, sample_count);
		}
	}

	pthread_mutex_unlock( &ctx->mutex );

	return 0;
}

static int _flush( STREAM_FILTER_AUDIO *f )
{
//serprintf("facomp: flush\n" );
	return 0;
}

static int _set_param( STREAM_FILTER_AUDIO *f, void *params, void *night_on )
{
	if( !f || !f->priv || !params || !night_on ) {
		DBG serprintf("facomp: set_param called with null parameters\n");
		return -1;
	}

	int *level = params;
	int *nightmode = night_on;
	struct ctx *ctx = f->priv;

	pthread_mutex_lock( &ctx->mutex );

	// Validate parameter ranges
	int new_level = MAX(0, MIN(3, *level));      // Audio boost: 0 or 3 only
	int new_nightmode = (*nightmode) ? 1 : 0;    // Night mode: 0 or 1 only

	DBG serprintf("facomp: set_param called - level=%d nightmode=%d\n", new_level, new_nightmode);

	// Only reconfigure if parameters actually changed
	if( (ctx->level != new_level) || (ctx->nightmode != new_nightmode) ) {
		// Log the mode transition
		const char* old_mode = (ctx->level == 0 && ctx->nightmode == 0) ? "OFF" :
		                      (ctx->level == 3 && ctx->nightmode == 0) ? "Audio Boost" :
		                      (ctx->level == 0 && ctx->nightmode == 1) ? "Night Mode" :
		                      "Night Mode + Audio Boost";

		const char* new_mode = (new_level == 0 && new_nightmode == 0) ? "OFF" :
		                      (new_level == 3 && new_nightmode == 0) ? "Audio Boost" :
		                      (new_level == 0 && new_nightmode == 1) ? "Night Mode" :
		                      "Night Mode + Audio Boost";

		DBG serprintf("facomp: mode change %s -> %s\n", old_mode, new_mode);

		ctx->level = new_level;
		ctx->nightmode = new_nightmode;
		setup( ctx );
	} else {
		DBG serprintf("facomp: no parameter change, keeping current configuration\n");
	}

	pthread_mutex_unlock( &ctx->mutex );

	return 0;
}

int _delay( STREAM_FILTER_AUDIO *f )
{
	// AudioCompress processes samples in real-time with minimal latency
	// Return 0 to indicate no additional buffering delay
	return 0;
}

STREAM_FILTER_AUDIO *stream_filter_audio_compress_new( void )
{
	STREAM_FILTER_AUDIO *f = acalloc( 1, sizeof( STREAM_FILTER_AUDIO ) );

	if( !f ) {
		serprintf("facomp: failed to allocate filter structure\n");
		return NULL;
	}

	// Initialize function pointers for audio filter interface
	static char name[] = "AudioCompress";  // Updated name to reflect the library used
	f->name      = name;
	f->delete    = _delete;
	f->open      = _open;
	f->close     = _close;
	f->filter    = _filter;
	f->flush     = _flush;
	f->set_param = _set_param;
	f->delay     = _delay;

	DBG serprintf("facomp: AudioCompress filter created successfully\n");
	return f;
}


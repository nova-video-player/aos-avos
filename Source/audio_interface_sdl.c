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

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>

#include "global.h"
#include "debug.h"
#include "types.h"
#include "util.h"
#include "audio_interface.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>

#define WAVE_FORMAT_UNKNOWN 0x0000
#define WAVE_FORMAT_PCM 0x0001
#define WAVE_FORMAT_ALAW 0x0006
#define WAVE_FORMAT_MULAW 0x0007
#define WAVE_FORMAT_IMA 0x0011
#define WAVE_FORMAT_MPEG 0x0050
#define WAVE_FORMAT_MPEGLAYER3 0x0055
#define WAVE_FORMAT_AAC 0x00FF
#define WAVE_FORMAT_AC3 0x2000
#define WAVE_FORMAT_EAC3 0x4747
#define WAVE_FORMAT_E_AC3_JOC 0x4748

#define DBG if( Debug[DBG_AUDIODEVICE] )
#define DBG2 if( Debug[DBG_AUDIODEVICE] > 1 )
#define ERR if( 1 )

#define SDL_AUDIO_BUFFER_SIZE 16384
#define SDL_AUDIO_BUFFER_COUNT 4

typedef struct {
	uint8_t *data;
	int size;
	int pos;
} audio_buffer_t;

struct audio_ctx {
	SDL_AudioDeviceID device_id;
	SDL_AudioSpec spec;
	audio_buffer_t buffer;
	pthread_mutex_t lock;
	int is_open;
	int is_playing;
	unsigned int channels;
	unsigned int rate;
	int byte_per_sample;
};

static struct audio_ctx *_ctx = NULL;

static void sdl_audio_callback( void *userdata, uint8_t *stream, int len )
{
	struct audio_ctx *ctx = (struct audio_ctx *)userdata;

	pthread_mutex_lock( &ctx->lock );

	if( ctx->buffer.data && ctx->buffer.pos < ctx->buffer.size ) {
		int available = ctx->buffer.size - ctx->buffer.pos;
		int to_copy = ( len < available ) ? len : available;

		if( to_copy > 0 ) {
			memcpy( stream, ctx->buffer.data + ctx->buffer.pos, to_copy );
			ctx->buffer.pos += to_copy;
			len -= to_copy;
			stream += to_copy;
		}

		if( ctx->buffer.pos >= ctx->buffer.size ) {
			ctx->buffer.pos = 0;
			ctx->buffer.size = 0;
		}
	}

	if( len > 0 ) {
		memset( stream, 0, len );
	}

	pthread_mutex_unlock( &ctx->lock );
}

static int sdl_init( void )
{
	if( SDL_InitSubSystem( SDL_INIT_AUDIO ) < 0 ) {
		ERR serprintf( "SDL audio initialization failed: %s\r\n", SDL_GetError() );
		return -1;
	}
	DBG serprintf( "SDL audio initialized\r\n" );
	return 0;
}

static void sdl_exit( void ) { SDL_QuitSubSystem( SDL_INIT_AUDIO ); }

static audio_ctx_t *sdl_open( int _mode )
{
	struct audio_ctx *ctx;

	if( _mode != AUDIO_OUTPUT_MODE ) {
		return NULL;
	}

	if( _ctx != NULL ) {
		DBG serprintf( "SDL audio already open\r\n" );
		return (audio_ctx_t *)_ctx;
	}

	ctx = (struct audio_ctx *)calloc( 1, sizeof( struct audio_ctx ) );
	if( !ctx ) {
		return NULL;
	}

	pthread_mutex_init( &ctx->lock, NULL );
	ctx->buffer.data = (uint8_t *)malloc( SDL_AUDIO_BUFFER_SIZE * SDL_AUDIO_BUFFER_COUNT );
	if( !ctx->buffer.data ) {
		ERR serprintf( "Failed to allocate audio buffer\r\n" );
		pthread_mutex_destroy( &ctx->lock );
		free( ctx );
		return NULL;
	}

	ctx->buffer.size = 0;
	ctx->buffer.pos = 0;
	ctx->is_open = 1;
	ctx->channels = 0;
	ctx->rate = 0;
	ctx->byte_per_sample = 0;
	ctx->device_id = 0;

	_ctx = ctx;
	DBG serprintf( "SDL audio opened\r\n" );
	return (audio_ctx_t *)ctx;
}

static int sdl_close( audio_ctx_t **ctx_ptr )
{
	struct audio_ctx *ctx;

	if( !ctx_ptr ) {
		return -1;
	}

	ctx = (struct audio_ctx *)*ctx_ptr;

	if( !ctx || ctx != _ctx ) {
		return -1;
	}

	if( ctx->device_id != 0 ) {
		SDL_CloseAudioDevice( ctx->device_id );
		ctx->device_id = 0;
	}

	if( ctx->buffer.data ) {
		free( ctx->buffer.data );
		ctx->buffer.data = NULL;
	}

	pthread_mutex_destroy( &ctx->lock );
	ctx->is_open = 0;
	ctx->is_playing = 0;

	free( ctx );
	_ctx = NULL;
	*ctx_ptr = NULL;

	DBG serprintf( "SDL audio closed\r\n" );
	return 0;
}

static int _convert_to_sdl_format( int fmt, SDL_AudioFormat *sdl_fmt )
{
	switch( fmt ) {
	case WAVE_FORMAT_PCM:
	case WAVE_FORMAT_UNKNOWN:
		*sdl_fmt = AUDIO_S16LSB;
		return 0;
	case WAVE_FORMAT_MULAW:
	case WAVE_FORMAT_ALAW:
		*sdl_fmt = AUDIO_S16LSB;
		return 0;
	case WAVE_FORMAT_EAC3:
	case WAVE_FORMAT_E_AC3_JOC:
	case WAVE_FORMAT_AC3:
	case WAVE_FORMAT_AAC:
	case WAVE_FORMAT_MPEGLAYER3:
	case WAVE_FORMAT_MPEG:
	case WAVE_FORMAT_IMA:
		ERR serprintf( "SDL: Audio format 0x%04x (compressed), converting to PCM\r\n", fmt );
		*sdl_fmt = AUDIO_S16LSB;
		return 0;
	default:
		ERR serprintf( "SDL: Unknown format 0x%04x, using S16_LE\r\n", fmt );
		*sdl_fmt = AUDIO_S16LSB;
		return 0;
	}
}

static int _get_bytes_per_sample( SDL_AudioFormat fmt )
{
	switch( fmt ) {
	case AUDIO_U8:
		return 1;
	case AUDIO_S16LSB:
	case AUDIO_S16MSB:
		return 2;
	case AUDIO_S32LSB:
	case AUDIO_S32MSB:
		return 4;
	case AUDIO_F32LSB:
	case AUDIO_F32MSB:
		return 4;
	default:
		return 2;
	}
}

static int sdl_set_output_params( audio_ctx_t *ctx, int freq, int channels, int bits, int format )
{
	struct audio_ctx *c = (struct audio_ctx *)ctx;
	SDL_AudioSpec desired, obtained;
	SDL_AudioFormat sdl_fmt;

	if( !c || !c->is_open ) {
		return -1;
	}

	_convert_to_sdl_format( format, &sdl_fmt );

	DBG serprintf( "SDL_set_output_params: freq=%d channels=%d bits=%d format=0x%x (converted to SDL)\r\n", freq,
				   channels, bits, format );

	c->rate = freq;
	c->channels = channels;
	c->byte_per_sample = _get_bytes_per_sample( sdl_fmt );

	if( c->device_id != 0 ) {
		SDL_CloseAudioDevice( c->device_id );
		c->device_id = 0;
	}

	SDL_zero( desired );
	desired.freq = freq;
	desired.format = sdl_fmt;
	desired.channels = channels;
	desired.samples = SDL_AUDIO_BUFFER_SIZE;
	desired.callback = sdl_audio_callback;
	desired.userdata = (void *)c;

	c->device_id = SDL_OpenAudioDevice( NULL, 0, &desired, &obtained, 0 );
	if( c->device_id == 0 ) {
		ERR serprintf( "Failed to open SDL audio device: %s\r\n", SDL_GetError() );
		return -1;
	}

	DBG serprintf( "SDL audio device opened: freq=%d channels=%d format=%d\r\n", obtained.freq, obtained.channels,
				   obtained.format );

	c->spec = obtained;
	return 0;
}

static int sdl_start( audio_ctx_t *ctx )
{
	struct audio_ctx *c = (struct audio_ctx *)ctx;

	if( !c || c->device_id == 0 ) {
		ERR serprintf( "SDL audio start failed: invalid context or device\r\n" );
		return -1;
	}

	SDL_PauseAudioDevice( c->device_id, 0 );
	c->is_playing = 1;

	DBG serprintf( "SDL audio started\r\n" );
	return 0;
}

static int sdl_stop( audio_ctx_t *ctx )
{
	struct audio_ctx *c = (struct audio_ctx *)ctx;

	if( !c || c->device_id == 0 ) {
		return -1;
	}

	SDL_PauseAudioDevice( c->device_id, 1 );
	c->is_playing = 0;

	DBG serprintf( "SDL audio stopped\r\n" );
	return 0;
}

static int sdl_can_write( audio_ctx_t *ctx, int len )
{
	struct audio_ctx *c = (struct audio_ctx *)ctx;

	if( !c || c->device_id == 0 ) {
		return 0;
	}

	int buffer_total_size = SDL_AUDIO_BUFFER_SIZE * SDL_AUDIO_BUFFER_COUNT;
	int space_left = buffer_total_size - c->buffer.size;
	int can_write = space_left >= len ? 1 : 0;

	return can_write;
}

static int sdl_write( audio_ctx_t *ctx, unsigned char *buffer, int length )
{
	struct audio_ctx *c = (struct audio_ctx *)ctx;
	int written = 0;

	if( !c || c->device_id == 0 || !buffer || length <= 0 ) {
		return 0;
	}

	pthread_mutex_lock( &c->lock );

	int buffer_total_size = SDL_AUDIO_BUFFER_SIZE * SDL_AUDIO_BUFFER_COUNT;
	int available_space = buffer_total_size - c->buffer.size;

	if( available_space > 0 ) {
		int to_write = ( length < available_space ) ? length : available_space;
		memcpy( c->buffer.data + c->buffer.size, buffer, to_write );
		c->buffer.size += to_write;
		written = to_write;
	}

	pthread_mutex_unlock( &c->lock );

	return written;
}

static int sdl_get_delay( audio_ctx_t *ctx )
{
	struct audio_ctx *c = (struct audio_ctx *)ctx;

	if( !c || c->rate == 0 || c->channels == 0 ) {
		return 0;
	}

	pthread_mutex_lock( &c->lock );
	int delay = ( c->buffer.size / ( c->channels * c->byte_per_sample ) ) * 1000 / c->rate;
	pthread_mutex_unlock( &c->lock );

	return delay;
}

static void sdl_flush_output( audio_ctx_t *ctx )
{
	struct audio_ctx *c = (struct audio_ctx *)ctx;

	if( !c ) {
		return;
	}

	pthread_mutex_lock( &c->lock );
	c->buffer.size = 0;
	c->buffer.pos = 0;
	pthread_mutex_unlock( &c->lock );

	DBG serprintf( "SDL audio flushed\r\n" );
}

static int sdl_preload( audio_ctx_t *ctx ) { return -1; }

static int sdl_mute( audio_ctx_t *ctx, BOOL fade )
{
	DBG serprintf( "SDL audio_interface_mute\r\n" );
	return 0;
}

static int sdl_unmute( audio_ctx_t *ctx, BOOL fade, BOOL threaded )
{
	DBG serprintf( "SDL audio_interface_unmute\r\n" );
	return 0;
}

static int sdl_set_output_volume( audio_ctx_t *ctx, int volume, int balance )
{
	DBG serprintf( "SDL audio_interface_set_output_volume\r\n" );
	return 0;
}

static int sdl_set_output_volume_l_r( audio_ctx_t *ctx, int vol_l, int vol_r )
{
	DBG serprintf( "SDL audio_interface_set_output_volume_l_r\r\n" );
	return 0;
}

static int sdl_get_session_id( audio_ctx_t *ctx ) { return 0; }

const audio_interface_impl_t audio_interface_impl_sdl = {
	.name = "sdl",
	.init = sdl_init,
	.exit = sdl_exit,
	.open = sdl_open,
	.close = sdl_close,
	.start = sdl_start,
	.stop = sdl_stop,
	.can_write = sdl_can_write,
	.write = sdl_write,
	.set_output_params = sdl_set_output_params,
	.get_delay = sdl_get_delay,
	.flush_output = sdl_flush_output,
	.preload = sdl_preload,
	.mute = sdl_mute,
	.unmute = sdl_unmute,
	.set_output_volume = sdl_set_output_volume,
	.set_output_volume_l_r = sdl_set_output_volume_l_r,
	.get_session_id = sdl_get_session_id,
};

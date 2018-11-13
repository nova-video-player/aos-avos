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
#include "debug.h"
#include "types.h"
#include "audio_interface.h"
#include "atime.h"
#include "downmix.h"

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/soundcard.h>

#ifndef STANDALONE

#define DBG  if(Debug[DBG_AUDIODEVICE])
#define DBG2 if(Debug[DBG_AUDIODEVICE] > 1)

#endif

static int frag_count     = 4;
static int frag_size_bits = 10;

static int dsp_fd = -1;
static int fsize = -1;
static int fcount = -1;
static int fmt = -1;
static int freq = -1;
static int chan = -1;
static int oss_chan = -1;
static int mode;
static int o_mode;
static int passthrough = 0;
static int oss_max_channels = 6;

DECLARE_DEBUG_PARAM  ("ossc", oss_max_channels );
 
void *mixer_oss_open( int mode );
int mixer_oss_close( void **handle );
int mixer_oss_set_volume( void *handle, int left, int right );

static struct audio_ctx {
	int foo;
} _oss_ctx;

static struct audio_ctx *oss_ctx;

static int oss_init(void)
{
DBG serprintf( "oss_init\n" );
	return 0;
}

static void oss_exit(void)
{
DBG serprintf( "oss_exit\n" );
}

static audio_ctx_t *oss_open(int _mode)
{
DBG serprintf( "oss_open(%i)\r\n", _mode );

	switch ( _mode ) {
	case AUDIO_OUTPUT_MODE:
		o_mode = O_WRONLY;
		break;
	case AUDIO_MIXER_MODE:
		return mixer_oss_open( mode );
	default:
		return NULL;
	}
	mode = _mode;

	dsp_fd = open( "/dev/dsp", o_mode );
	if ( dsp_fd < 0 ) {
serprintf( "OSS: open: %s\n", strerror( errno ) );
		dsp_fd = open( "/dev/snd/dsp", o_mode );
		if ( dsp_fd < 0 ) {
serprintf( "OSS: open: %s\n", strerror( errno ) );
			serprintf( "OSS: opening /dev/dsp failed: \r\n" );
			return NULL;
		}
	}

	fcntl( dsp_fd, F_SETFD, FD_CLOEXEC );

	fmt = -1;		// don't assume the format is known
	chan = -1;
	oss_chan = -1;
	freq = -1;
	fsize = -1;
	fcount = -1;

	oss_ctx = &_oss_ctx;
	
	return oss_ctx;
}

// ************************************************************
//
//      _get_obuffer
//
// ***********************************************************
static int _get_obuffer( int *size, int *free )
{
	audio_buf_info info;
	if ( ioctl( dsp_fd, SNDCTL_DSP_GETOSPACE, &info ) ) {
serprintf( "OSS_getobuffer error: %s\r\n", strerror( errno ) );
		return 1;
	}

	if ( size )
		*size = info.fragstotal * info.fragsize;
	if ( free )
		*free = info.bytes;
//serprintf("OSS_getobuffer %5d %5d %5d %5d\r\n", info.fragments, info.fragstotal, info.fragsize, info.bytes);
	return 0;
}

// ************************************************************
//
//      _sync
//
// ***********************************************************
static int _sync( void )
{
DBG serprintf( "OSS_sync\r\n" );

	if ( o_mode == O_RDONLY ) {
		// recording
		ioctl( dsp_fd, SNDCTL_DSP_SYNC );
	} else {
		// for some reason the below SNDCTL_DSP_SYNC sometimes hangs the audiodevice,
		// so we implement our own sync here by simply waiting until all data has been
		// written....
		while ( 1 ) {
			int size;
			int free;
			if ( _get_obuffer( &size, &free ) ) {
				// error, but why?
				break;
			}
DBG serprintf( " [%5d/%5d ]", size, free );
			if ( size == free ) {
				break;
			}
			msec_sleep( 1 );
		}

		// Full duplex operation
		if ( o_mode == O_RDWR ) {
			ioctl( dsp_fd, SNDCTL_DSP_SYNC );
		}
	}
DBG serprintf( "...sync done\r\n" );
	return 0;
}

static int oss_close(audio_ctx_t **ctx)
{
DBG serprintf( "OSS_close()\r\n" );
	if( *ctx != &_oss_ctx ) {
		return mixer_oss_close( (void**)ctx );
	}

	if( dsp_fd == -1 ) {
		// we are closed
		return 1;
	}
	// damn /dev/dsp hangs whereever it can, we have to sync
	// (using our method) before we can close it!
	_sync();

	close( dsp_fd );

	dsp_fd = -1;
	fmt = -1;
	freq = -1;
	chan = -1;
	oss_chan = -1;
	fsize = -1;
	fcount = -1;

	*ctx = NULL;
	return 0;
}

static int oss_start(audio_ctx_t *ctx)
{
DBG serprintf( "OSS: start\n" );
	return 0;
}

static int oss_stop(audio_ctx_t *ctx)
{
DBG serprintf( "OSS: stop\n" );
	return 0;
}

static int oss_can_write(audio_ctx_t *ctx, int len)
{
	fd_set wfds;
	struct timeval tv;
	int ret;

	if ( dsp_fd == -1 )
		return 0;

	FD_ZERO( &wfds );
	FD_SET( dsp_fd, &wfds );

	tv.tv_sec = 0;
	tv.tv_usec = 0;

	ret = select( dsp_fd + 1, NULL, &wfds, NULL, &tv );

	if ( ret == -1 ) {
serprintf( "OSS: select_error: %s", strerror( errno ) );
		return 1;
	} else if ( ret ) {
		return 1;
	}

	return 0;
}

static int channel_map[8] = { CH_FL, CH_FR, CH_CTR, CH_SUB, CH_BL, CH_BR, CH_SL, CH_SR };

static int oss_write(audio_ctx_t *ctx, unsigned char *data, int data_length)
{
	if( passthrough ) {
//serprintf( "OSS: pt write %d\n", data_length );
		return 0;
	}
	if( chan > oss_chan ) {
		int samples = data_length / chan / 2;
//serprintf( "OSS: must downmix! %d > %d, %d\n", chan, oss_chan, samples );
		uint16_t stereo[samples * 2];

		downmix( stereo, data, samples, chan, 16, channel_map );
		return write( dsp_fd, stereo, samples * 4 );
	} else {
		return write( dsp_fd, data, data_length );
	}
}

static int set_format( int *new )
{
	if ( fmt == *new ) {
		return 0;
	}
DBG serprintf( "OSS: set_format(%i)\r\n", *new );
	fmt = *new;
	if ( -1 == ioctl( dsp_fd, SNDCTL_DSP_SETFMT, &fmt ) ) {
DBG serprintf( "OSS: set_format(%i) failed: %s\r\n", fmt, strerror( errno ));
		return 1;
	}
	*new = fmt;
	return 0;
}

static int set_channels( int *new )
{
	if ( chan == *new ) {
		return 1;
	}
DBG serprintf( "OSS: set_channels(%i)\r\n", *new );
	chan = *new;
	oss_chan = chan;
	if( chan > 2 ) {
		oss_chan = 2;
	}
	if( -1 == ioctl( dsp_fd, SNDCTL_DSP_CHANNELS, &oss_chan ) ) {
DBG serprintf( "OSS: set_channels(%i) failed: %s\r\n", chan, strerror( errno ));
		return 1;
	}
	*new = chan;
	return 0;
}

static int set_rate( int *new )
{
DBG serprintf( "OSS: set_rate(%i)\r\n", *new );
	freq = *new;
	if( -1 == ioctl( dsp_fd, SNDCTL_DSP_SPEED, &freq ) ) {
DBG serprintf( "OSS: set_rate(%i) failed: %s\r\n", freq, strerror( errno ));
		return 1;
	}
	*new = freq;
	return 0;
}

static int set_buffersize( int size_bits, int count )
{
	fsize  = size_bits;
	fcount = count;

	int arg = ( ( fcount & 0xFFFF ) << 16 ) | fsize;
DBG serprintf( "OSS: _set_bufersize(bits %d, count %d) %08x \r\n", fsize, fcount, arg );

	if ( ioctl( dsp_fd, SNDCTL_DSP_SETFRAGMENT, &arg ) < 0 ) {
		serprintf( "OSS_set_buffersize error: %s  %08X\r\n", strerror( errno ), arg );
		return 1;
	}

	return 0;
}

static int oss_set_output_params(audio_ctx_t *ctx, int freq, int channels, int bits, int format)
{
	if( bits != 16 ) {
serprintf( "OSS: %d bits != 16\r\n", bits );
		return 1;
	}
	if( channels > oss_max_channels ) {
serprintf( "OSS: %d channels > %d\r\n", channels, oss_max_channels );
		return 1;
	}
	
	if( freq < 16000 ) {
		frag_size_bits = 8;
	} else if ( freq < 32000 ) {
		frag_size_bits = 9;
	} else {
		frag_size_bits = 10;
	}

	int ossFormat = AFMT_S16_LE;
	
	set_buffersize(frag_size_bits, frag_count);
	
	set_rate( &freq );
	set_channels( &channels );
	set_format( &ossFormat );

	return 0;
}

static int oss_get_delay(audio_ctx_t *ctx)
{
	return 1000 * ((1 << frag_size_bits) * frag_count) / (freq * oss_chan * 2);
}

static void oss_flush_output(audio_ctx_t *ctx) 
{
}

static int oss_preload(audio_ctx_t *ctx)
{
	return -1;
}

static int oss_get_session_id(audio_ctx_t *ctx)
{
	return 0;
}

static int oss_set_passthrough(audio_ctx_t *at, int _passthrough)
{
	passthrough = _passthrough;
	return 0;
}

static int oss_get_passthrough(audio_ctx_t *at)
{
	return passthrough;
}

const audio_interface_impl_t audio_interface_impl_oss = {
	.name              = "oss",
	.init              = oss_init,
	.exit              = oss_exit,
	.open              = oss_open,
	.close             = oss_close,
	.start             = oss_start,
	.stop              = oss_stop,
	.can_write         = oss_can_write,
	.write             = oss_write,
	.set_output_params = oss_set_output_params,
	.get_delay         = oss_get_delay,
	.flush_output      = oss_flush_output,
	.preload           = oss_preload,
	.get_session_id    = oss_get_session_id,
	.set_passthrough   = oss_set_passthrough,
	.get_passthrough   = oss_get_passthrough,
};

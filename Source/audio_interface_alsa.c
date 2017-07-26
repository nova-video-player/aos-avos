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
#include "util.h"
#include "audio_interface.h"

#include <alsa/asoundlib.h>
#include <alsa/pcm.h>
#include <alsa/error.h>

#define DBG  if(Debug[DBG_AUDIODEVICE])
#define DBG2 if(Debug[DBG_AUDIODEVICE] > 1)

#define ERR if(1)

#define ALSA_FRAG_COUNT		1
#define	ALSA_PERIODS		2
#define ALSA_PERIOD_SIZE	4096
#define ALSA_FRAG_SIZE		(ALSA_PERIODS * ALSA_PERIOD_SIZE)

struct stream_properties {
	snd_pcm_t *playbackHandle;
	int p_prepare;
	unsigned int channels;
	snd_pcm_format_t format;
	unsigned int rate;
	snd_pcm_uframes_t buffer_size;
	int byte_per_sample;

	struct audio_interface_alsa_mixer {

	} mixer;
};

static struct stream_properties _stream_properties = {
	.playbackHandle = NULL,
	.p_prepare = 1,
	.channels = 0,
	.format = SND_PCM_FORMAT_UNKNOWN,
	.rate = 0,
	.buffer_size = 0,
	.byte_per_sample = 0,
};

int audio_interface_init(void)
{
	return 0;
}

int audio_interface_exit(void)
{
	return 0;
}

// ************************************************************
//
//	_init
//
// ***********************************************************
static int _init( void )
{
serprintf("ALSA_init!\r\n" ); 
	return 0;
}

// ************************************************************
//
//	_apply_hw_params
//
// ***********************************************************
static int _apply_hw_params(struct stream_properties *props, snd_pcm_t *handle)
{
	snd_pcm_hw_params_t *hwParams;
	int ret = -1;

	if (handle == NULL) {
		DBG serprintf("%s : stream not opened \r\n", __FUNCTION__);
		return -1;
	}

	ret = snd_pcm_hw_params_malloc(&hwParams);
	if (ret<0) {
		ERR serprintf("%s : Error allocating hardware params (%s)\r\n",__FUNCTION__,snd_strerror(ret));
		return -1;
	}
	ret = snd_pcm_hw_params_any(handle, hwParams);
	if (ret) {
		ERR serprintf("%s : Error initializing hardware parameters (%s)\r\n",__FUNCTION__,snd_strerror(ret));
		goto error_exit;
	}
	ret = snd_pcm_hw_params_set_access(handle, hwParams, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (ret) {
		ERR serprintf("%s : Error setting access type (%s)\r\n",__FUNCTION__,snd_strerror(ret));
		goto error_exit;
	}
	ret = snd_pcm_hw_params_set_periods(handle, hwParams, ALSA_PERIODS, 0);
	if (ret) {
		ERR serprintf("%s : Error setting periods (%s)\r\n",__FUNCTION__,snd_strerror(ret));
		goto error_exit;
	}
	ret = snd_pcm_hw_params_set_format(handle, hwParams, props->format);
	if (ret) {
		ERR serprintf("%s : Error setting format periods (%s)\r\n",__FUNCTION__,snd_strerror(ret));
		goto error_exit;
	}
	ret = snd_pcm_hw_params_set_channels(handle, hwParams, props->channels);
	if (ret) {
		ERR serprintf("%s : Error setting channels (%s)\r\n",__FUNCTION__,snd_strerror(ret));
		goto error_exit;
	}
	ret = snd_pcm_hw_params_set_rate(handle, hwParams, props->rate, 0);
	if (ret) {
		ERR serprintf("%s : Error setting sample rate (%s)\r\n",__FUNCTION__,snd_strerror(ret));
		goto error_exit;
	}
	ret = snd_pcm_hw_params_set_buffer_size(handle, hwParams, props->buffer_size);
	if (ret) {
		ERR serprintf("%s : Error setting buffer size (%u, %s)\r\n",__FUNCTION__, props->buffer_size, snd_strerror(ret));
		goto error_exit;
	}
	ret = snd_pcm_hw_params(handle, hwParams);
	if (ret) {
		ERR serprintf("%s : Error setting hardware parameters (%s)\r\n",__FUNCTION__,snd_strerror(ret));
		goto error_exit;
	}
	DBG serprintf("%s OK\r\n", __FUNCTION__);
	ret = 0;

error_exit:
	snd_pcm_hw_params_free(hwParams);
	return ret;
}

// ************************************************************
//
//	_open_playback
//
// ***********************************************************
static int _open_playback(struct stream_properties *props)
{
	if( props->playbackHandle != NULL ) {
		DBG serprintf("%s : playback already opened\r\n", __FUNCTION__);
		return 0;
	}

	char *pcmName = strdup("default");
	if( pcmName == NULL ) {
		ERR serprintf("%s : Error allocating pcm name\r\n",__FUNCTION__);
		return -1;
	}

	snd_pcm_t *handle;
	int err = 0;
	int ret = snd_pcm_open( &handle, pcmName, SND_PCM_STREAM_PLAYBACK, 0 );
	if( ret ) {
		ERR serprintf("%s : Error opening PCM interface (%s)\r\n",__FUNCTION__,snd_strerror(ret));
		err = -1;
		goto error_exit;
	}

	props->playbackHandle = handle;

error_exit:
	if( pcmName ) {
		free( pcmName );
	}
	return err;
}

static struct audio_interface_alsa_mixer *audio_interface_open_mixer(struct stream_properties *props)
{
	return &props->mixer;
}

// ************************************************************
//
//	audio_interface_open
//	
//	@param mode Possible values:
//	- ReadOnlyMode: Open the device for reading only.
//	- WriteOnlyMode: Open the device for writing only.
//	- ReadWriteMode: Open the device for reading and writing. 
//
// ***********************************************************
void *audio_interface_open(int _mode)
{
	struct stream_properties *props = &_stream_properties;
	int ret;

DBG serprintf("ALSA_open(%i)\r\n", _mode);

	switch ( _mode ) {
		case AUDIO_OUTPUT_MODE:
			break;
		case AUDIO_MIXER_MODE:
			return audio_interface_open_mixer(props);
/*
		case ReadOnlyMode:
			break;
		case ReadWriteMode:
			break;
*/
		default:
			return NULL;
	}

	ret = _open_playback(&_stream_properties);
	if (ret)
		return NULL;

	props->channels = 0;
	props->format = SND_PCM_FORMAT_UNKNOWN;
	props->rate = 0;
	props->buffer_size = 0;

	return (void*)&_stream_properties;
}

static int audio_interface_close_mixer(struct audio_interface_alsa_mixer *mixer)
{
serprintf( "ALSA close mixer\r\n");
	return 0;
}

// ************************************************************
//
//	audio_interface_close
//
// ***********************************************************
int audio_interface_close(void **ctx)
{
	if( *ctx != &_stream_properties ) {
		return audio_interface_close_mixer(&_stream_properties.mixer);
	}

	struct stream_properties *props = *ctx;
DBG serprintf("ALSA_close()\r\n");
	if ( _stream_properties.playbackHandle != NULL ) {
DBG serprintf("ALSA_close(): destroying playback data\r\n");
		snd_pcm_close(props->playbackHandle);
		props->playbackHandle = NULL;
	}

	ctx = NULL;
	return 0;
}

// ************************************************************
//
//	_convert_to_alsa_format
//
// ***********************************************************
static int _convert_to_alsa_format( int new, snd_pcm_format_t *fmt )
{
	switch(new) {
	case AFMT_S16_LE:
		if (fmt) {
			*fmt = SND_PCM_FORMAT_S16_LE;
        		_stream_properties.byte_per_sample = 2;
		}
		break;
	default:
		return -1;
	}

	return 0;
}

// ************************************************************
//
//	_convert_to_sound_format
//
// ***********************************************************
static int _convert_to_sound_format( snd_pcm_format_t fmt, int *new )
{
	switch(fmt) {
	case SND_PCM_FORMAT_S16_LE:
		if (new) {
			*new = AFMT_S16_LE;
		}
		break;
	default:
		return -1;
	}

	return 0;
}

// ************************************************************
//
//	_set_format
//
// ***********************************************************
static int _set_format(struct stream_properties *props, int *new)
{
	snd_pcm_format_t fmt;
	int ret;

	ret = _convert_to_alsa_format( *new, &fmt);
	if (ret != 0) {
		ERR serprintf( "ALSA_set_format(%i) not supported\r\n", *new );
		return 1;
	}

	if ( fmt == props->format ) {
		return 0;
	}

	DBG serprintf( "ALSA_set_format(%i)\r\n", *new );
	_stream_properties.format = fmt;

	return 0;
}

// ************************************************************
//
//	_set_channels
//
// ***********************************************************
static int _set_channels(struct stream_properties *props, int *new)
{
	if (props->channels == (unsigned int) *new ) {
		return 0;
	}
	DBG serprintf( "ALSA_set_channels(%u)\r\n", (unsigned int) *new );

	props->channels = (unsigned int) *new;

	return 0;
}

// ************************************************************
//
//	_set_rate
//
// ***********************************************************
static int _set_rate(struct stream_properties *props, int *new )
{
	DBG serprintf( "ALSA_set_rate(%u)\r\n", (unsigned int) *new );
	props->rate = (unsigned int) *new;

	return 0;
}

// ************************************************************
//
//	_set_buffersize
//
//	@param size Size of the buffer in bytes. 
//
// ***********************************************************
static int _set_buffersize(struct stream_properties *props, int size)
{
	int fragcount = size / ALSA_FRAG_SIZE;
	snd_pcm_uframes_t uframe;
	
	if(props->channels==0 || props->byte_per_sample==0) 
		return -1;
	if ( size < ALSA_FRAG_SIZE ) {
		uframe = (ALSA_FRAG_SIZE) / (props->channels * props->byte_per_sample);
	} else {
		uframe = (fragcount * ALSA_FRAG_SIZE) /(props->channels * props->byte_per_sample);
	}

	DBG serprintf( "ALSA_set_bufersize(size %d -> fragsize %d uframe %lu) \r\n", size, ALSA_FRAG_SIZE, (unsigned long) uframe );
	props->buffer_size = uframe;

	return 0;
}

int audio_interface_set_output_params(void *ctx, int freq, int channels, int bits, int format )
{
	struct stream_properties *props = ctx;
DBG serprintf("ALSA_set_output_params\r\n");

	_set_rate(props, &freq);
	_set_channels(props, &channels);
	_set_format(props, &format);

	_set_buffersize(props, ALSA_FRAG_COUNT*ALSA_FRAG_SIZE);

	if (props->playbackHandle != NULL)  {
		_apply_hw_params(props, props->playbackHandle);
	}
DBG serprintf("ALSA_set_output_params3\r\n");
	return 0;
}

// ************************************************************
//
//	_sync
//
// ***********************************************************
static int _sync( void ) 
{
	int ret = 0;
DBG serprintf("ALSA_sync\r\n");
	if ( _stream_properties.playbackHandle != NULL ) {
		ret |= snd_pcm_drain(_stream_properties.playbackHandle);
		_stream_properties.p_prepare = 1;
	}
	return ret;
}

// ************************************************************
//
//	xrun_recovery
//
// ***********************************************************
static int xrun_recovery(snd_pcm_t *handle, int err)
{
	if (err == -EPIPE) {	/* under-run */
		err = snd_pcm_prepare(handle);
		if (err < 0)
			ERR serprintf("%s: Can't recovery from underrun, prepare failed: %s\n", __FUNCTION__, snd_strerror(err));
		return 0;
	} else if (err == -ESTRPIPE) {
		while ((err = snd_pcm_resume(handle)) == -EAGAIN)
			sleep(1);	/* wait until the suspend flag is released */
		if (err < 0) {
			err = snd_pcm_prepare(handle);
			if (err < 0)
				ERR serprintf("%s: Can't recovery from suspend, prepare failed: %s\n", __FUNCTION__, snd_strerror(err));
		}
		return 0;
	}
	return err;
}

// ************************************************************
//
//	_get_obuffer
//
// ***********************************************************
static int _get_obuffer( int *size, int *free )
{
	if ( _stream_properties.playbackHandle != NULL ) {
		int ret;
		snd_pcm_uframes_t buffer_size;
		snd_pcm_uframes_t period_size;
		snd_pcm_sframes_t free_frames;
		snd_pcm_sframes_t used_frames;

		ret = snd_pcm_get_params(_stream_properties.playbackHandle, &buffer_size, &period_size);
		if (ret < 0) {
			ERR serprintf("%s: snd_pcm_get_params failed err (%s)\r\n",__FUNCTION__, snd_strerror(ret));
			return 1;
		}

		free_frames = snd_pcm_avail_update(_stream_properties.playbackHandle) ;
		if (free_frames < 0) {
			int err;
			int state = (int) snd_pcm_state(_stream_properties.playbackHandle);
			DBG serprintf("%s: snd_pcm_avail_update failed err (%s) state %d\r\n",__FUNCTION__, snd_strerror(free_frames), state);
			
			err = xrun_recovery(_stream_properties.playbackHandle, free_frames);
			if (err < 0) {
				ERR serprintf("%s xrun_recovery failed: %s\n", __FUNCTION__, snd_strerror(err));
				return 1;
			}
			free_frames = (snd_pcm_sframes_t) buffer_size;
		}
		used_frames = (snd_pcm_sframes_t) buffer_size - free_frames;

		if ( size )
			*size = (int) buffer_size * _stream_properties.byte_per_sample * _stream_properties.channels;
		if ( free )
			*free = (int) free_frames * _stream_properties.byte_per_sample * _stream_properties.channels;

DBG2 serprintf("ALSA_getobuffer %5lu %5lu %5ld %5ld\r\n", (unsigned long) buffer_size * 4, (unsigned long) period_size * 4, (signed long) free_frames * 4, (signed long) used_frames * 4);
	}
	return 1;
}

int audio_interface_start(void *ctx)
{
DBG serprintf( "ALSA_start\n" );
	return 0;
}

int audio_interface_stop(void *ctx)
{
DBG serprintf( "ALSA_stop\n" );
	return 0;
}

// ************************************************************
//
//	audio_interface_can_write
//
// ***********************************************************
int audio_interface_can_write(void *ctx)
{
	struct stream_properties *props = ctx;
DBG2 serprintf("ALSA_can_write\n");

	if ( props->playbackHandle == NULL ) {
		return 0;
	}

	return 1;
}

// ************************************************************
//
//	audio_interface_write
//
// ***********************************************************
int audio_interface_write(void *ctx, unsigned char *buffer, int length)
{
	struct stream_properties *props = ctx;
	int ret;
	snd_pcm_sframes_t len = 0;
	snd_pcm_uframes_t frames = length /(props->channels * props->byte_per_sample);

DBG2 serprintf("ALSA_write( %u )\n", length );

	if ( props->playbackHandle != NULL ) {
		if (props->p_prepare != 0) {
			if ((ret = snd_pcm_prepare( props->playbackHandle) )<0) {
				ERR serprintf("%s: snd_pcm_prepare failed (%s)\r\n",__FUNCTION__, snd_strerror(ret));
				return 0;
			}
			props->p_prepare = 0;
		}

		len = snd_pcm_writei(props->playbackHandle, buffer, frames);
		if (len < 0) {
			DBG serprintf("%s: Buffer underrun (%s)\r\n",__FUNCTION__, snd_strerror((int)len));
			ret = snd_pcm_recover(props->playbackHandle, len, 0);
			if (ret) {
				ERR serprintf("%s: Error recovering from audio error (%s)\r\n" ,__FUNCTION__, snd_strerror(ret));
			} else {
				props->p_prepare = 1;
			}
		}
		if ((len > 0) && ((unsigned int) len < frames)) {
			ERR serprintf("Short write %d instead of %d frames", (int) len, (int) frames);
			ret = snd_pcm_prepare(props->playbackHandle);
			if (ret) {
				ERR serprintf("%s: Error recovering from audio error (%s)\r\n" ,__FUNCTION__, snd_strerror(ret));
			}
		}
	}
	if (len > 0)
		return ((int) len * 4);
	else
		return (int) len;
}

int audio_interface_get_delay(void *ctx)
{
	struct stream_properties *props = ctx;
DBG serprintf( "ALSA_get_delay\r\n" );
	/* VP ??? */
	return (UINT64)(ALSA_FRAG_COUNT*ALSA_FRAG_SIZE) / (props->rate * props->channels * 2);
}

void audio_interface_flush_output(void *ctx) 
{
}

int audio_interface_preload(void *ctx)
{
	return -1;
}

int audio_interface_mute(void *ctx, BOOL fade)
{
DBG serprintf( "audio_interface_mute\r\n" );
return 0;
}

int audio_interface_unmute(void *ctx, BOOL fade, BOOL threaded)
{
DBG serprintf( "audio_interface_unmute\r\n" );
return 0;
}

int audio_interface_set_output_volume(void *ctx, int volume, int balance)
{
DBG serprintf( "audio_interface_set_output_volume\r\n" );
	return 0;
}

int audio_interface_set_output_volume_l_r(void *ctx, int vol_l, int vol_r)
{
DBG serprintf( "audio_interface_set_output_volume_l_r\r\n" );
	return 0;
}

int audio_interface_get_session_id(void *ctx)
{
	return 0;
}

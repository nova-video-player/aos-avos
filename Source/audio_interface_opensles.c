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

#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <sched.h>
#include <stdatomic.h>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "global.h"
#include "debug.h"
#include "types.h"
#include "audio_interface.h"

#define DBG  if(0)
#define DBG2 if(0)
#define ERR  if(1)

#define LOG(fmt, ...) do { serprintf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); } while (0)
#define RESULT_CHECK(msg) do { \
	if (result != SL_RESULT_SUCCESS) { \
		LOG(msg": (%u)", result); \
		goto err; \
	} \
} while(0)

#define OPENSLES_BUFFERS	32 /* maximum number of buffers */
#define OPENSLES_BUFLATENCY	10 /* ms */

#define DLHELPER_HEADER "dlhelper_opensles.h"
#include "dlhelper.h"

#define DLHELPER_HEADER "dlhelper_audiosystem.h"
#include "dlhelper.h"

#define SLDestroy(a) (*a)->Destroy(a);
#define SLSetPlayState(a, b) (*a)->SetPlayState(a, b)
#define SLRegisterCallback(a, b, c) (*a)->RegisterCallback(a, b, c)
#define SLGetInterface(a, b, c) (*a)->GetInterface(a, b, c)
#define SLRealize(a, b) (*a)->Realize(a, b)
#define SLCreateOutputMix(a, b, c, d, e) (*a)->CreateOutputMix(a, b, c, d, e)
#define SLCreateAudioPlayer(a, b, c, d, e, f, g) (*a)->CreateAudioPlayer(a, b, c, d, e, f, g)
#define SLEnqueue(a, b, c) (*a)->Enqueue(a, b, c)
#define SLClear(a) (*a)->Clear(a)
#define SLGetState(a, b) (*a)->GetState(a, b)
#define SLSetPositionUpdatePeriod(a, b) (*a)->SetPositionUpdatePeriod(a, b)
#define SLSetVolumeLevel(a, b) (*a)->SetVolumeLevel(a, b)
#define SLSetMute(a, b) (*a)->SetMute(a, b)

struct audio_ctx {
	/* OpenSL */
	SLObjectItf			engineObject;
	SLEngineItf			engineEngine;
	SLObjectItf			outputMixObject;
	SLObjectItf			playerObject;
	SLPlayItf			playerPlay;
	SLVolumeItf			playerVolume;
	SLAndroidSimpleBufferQueueItf	playerBufferQueue;

	/* audio_interface */
	int		latency;
	int		started;
	uint8_t		*buffer;
	size_t		buffer_done;
	unsigned int	buffer_in_idx;
	unsigned int	buffer_out_idx;
	unsigned int	buffer_count;
	int		rate;
	size_t		samples_per_buf;
	int		bytes_per_sample;
};

static void opensles_reset(audio_ctx_t* p)
{
	if (p->playerPlay) {
		SLSetPlayState(p->playerPlay, SL_PLAYSTATE_STOPPED);
		p->playerPlay = NULL;
	}
	if (p->playerBufferQueue) {
		SLClear(p->playerBufferQueue);
		p->playerBufferQueue = NULL;
	}
	p->playerVolume = NULL;
	if (p->playerObject) {
		SLDestroy(p->playerObject);
		p->playerObject = NULL;
	}
	if (p->buffer) {
		free(p->buffer);
		p->buffer = NULL;
	}
	p->started = 0;
	p->buffer_count = 0;
	p->buffer_in_idx = p->buffer_out_idx = 0;
	p->samples_per_buf = 0;
}

static int opensles_close(audio_ctx_t **pp)
{
DBG	LOG();

	audio_ctx_t *p = *pp;

	opensles_reset(p);

	if (p->outputMixObject)	{
		SLDestroy(p->outputMixObject);
		p->outputMixObject = NULL;
	}
	if (p->engineObject) {
		SLDestroy(p->engineObject);
		p->engineObject = NULL;
	}
	free(p);
	*pp = NULL;
	return 0;
}

static int opensles_init(void)
{
	if (dlhelper_osl_init()) {
		LOG("dlhelper_osl_init failed");
		return 1;
	}
#ifdef CONFIG_ANDROID
	dlhelper_as_init();
#endif
	return 0;
}

static void opensles_exit(void)
{
}

static audio_ctx_t *opensles_open(int mode)
{
	audio_ctx_t *p;
	SLresult result;

	if (mode != AUDIO_OUTPUT_MODE)
		return NULL;

	LOG("name: opensles / mode: %i", mode);

	p = (audio_ctx_t *)calloc(1, sizeof(audio_ctx_t));

	if (!p) {
ERR		LOG("malloc failed\n");
		return NULL;
	}


	// create engine
	result = dl_osl.slCreateEngine(&p->engineObject, 0, NULL, 0, NULL, NULL);
	RESULT_CHECK("Failed to create engine");

	// realize the engine in synchronous mode
	result = SLRealize(p->engineObject, SL_BOOLEAN_FALSE);
	RESULT_CHECK("Failed to realize engine");

	// get the engine interface, needed to create other objects
	result = SLGetInterface(p->engineObject, *(dl_osl.SL_IID_ENGINE), &p->engineEngine);
	RESULT_CHECK("Failed to get the engine interface");

	// create output mix, with environmental reverb specified as a non-required interface
	const SLInterfaceID ids1[] = { *(dl_osl.SL_IID_VOLUME) };
	const SLboolean req1[] = { SL_BOOLEAN_FALSE };
	result = SLCreateOutputMix(p->engineEngine, &p->outputMixObject, 1, ids1, req1);
	RESULT_CHECK("Failed to create output mix");

	// realize the output mix in synchronous mode
	result = SLRealize(p->outputMixObject, SL_BOOLEAN_FALSE);
	RESULT_CHECK("Failed to realize output mix");

	return p;
err:
	if (p)
		opensles_close(&p);
	return NULL;
}

static void androidSimpleBufferQueueCallback(SLAndroidSimpleBufferQueueItf caller, void *pContext)
{
	audio_ctx_t *p = pContext;

	p->started = 1;
	__sync_fetch_and_sub(&p->buffer_count, 1);
}

static int opensles_set_output_params(audio_ctx_t *p, int rate, int channels, int bits, int format)
{
	SLresult result;
	SLuint16 bitsPerSample;
	SLuint32 channelMask;
	int ret = -1;

	LOG("rate %d, channels %d, bits %d", rate, channels, bits);

	opensles_reset(p);

	switch (bits) {
	case 8:
		bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_8;
		break;
	case 16:
		bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_16;
		break;
	case 24:
		bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_24;
		break;
	case 32:
		bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_32;
		break;
	default:
		LOG("audio format invalid");
		goto err;
	}

	switch (channels) {
	case 2:
		channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;
		break;
	case 3:
		channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT |
			SL_SPEAKER_LOW_FREQUENCY;
		break;
	case 4:
		channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT |
			SL_SPEAKER_FRONT_CENTER | SL_SPEAKER_BACK_CENTER;
		break;
	case 6:
		channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT |
			SL_SPEAKER_FRONT_CENTER | SL_SPEAKER_LOW_FREQUENCY | 
			SL_SPEAKER_BACK_LEFT | SL_SPEAKER_BACK_RIGHT;
		break;
	case 8:
		channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT |
			SL_SPEAKER_FRONT_CENTER | SL_SPEAKER_LOW_FREQUENCY | 
			SL_SPEAKER_BACK_LEFT | SL_SPEAKER_BACK_RIGHT |
			SL_SPEAKER_SIDE_LEFT | SL_SPEAKER_SIDE_RIGHT;
		break;
	default:
		channelMask = 0;
		break;
	}
	p->bytes_per_sample = bits / 8 * channels;


	SLDataLocator_AndroidSimpleBufferQueue bufferQueue = {
		SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
		OPENSLES_BUFFERS
	};

	SLDataFormat_PCM pcm;
	pcm.formatType    = SL_DATAFORMAT_PCM;
	pcm.numChannels	  = channels;
	pcm.samplesPerSec = ((SLuint32) rate * 1000); // (in milliHertz) 
	pcm.bitsPerSample = bitsPerSample;
	pcm.containerSize = bitsPerSample;
	pcm.channelMask   = channelMask;
	pcm.endianness    = SL_BYTEORDER_LITTLEENDIAN;
	
	SLDataSource audioSource = {&bufferQueue, &pcm};

	SLDataLocator_OutputMix locator_outputmix = {
		SL_DATALOCATOR_OUTPUTMIX,
		p->outputMixObject
	};
	SLDataSink audioSink = {&locator_outputmix, NULL};

	const SLInterfaceID ids2[] = { *(dl_osl.SL_IID_ANDROIDSIMPLEBUFFERQUEUE), *(dl_osl.SL_IID_VOLUME) };
	static const SLboolean req2[] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
	result = SLCreateAudioPlayer(p->engineEngine, &p->playerObject, &audioSource,
					&audioSink, sizeof(ids2) / sizeof(*ids2), ids2, req2);
	RESULT_CHECK("failed to create audio player");

	result = SLRealize(p->playerObject, SL_BOOLEAN_FALSE);
	RESULT_CHECK("failed to realize audio player");

	result = SLGetInterface(p->playerObject, *(dl_osl.SL_IID_PLAY), &p->playerPlay);
	RESULT_CHECK("failed to get player itf");

	result = SLGetInterface(p->playerObject, *(dl_osl.SL_IID_VOLUME), &p->playerVolume);
	RESULT_CHECK("failed to get volume itf");

	result = SLGetInterface(p->playerObject, *(dl_osl.SL_IID_ANDROIDSIMPLEBUFFERQUEUE), &p->playerBufferQueue);
	RESULT_CHECK("failed to get buffer queue itf");

	result = SLRegisterCallback(p->playerBufferQueue, androidSimpleBufferQueueCallback, (void *) p);
	RESULT_CHECK("failed to register buffer queue itf");

	p->samples_per_buf = OPENSLES_BUFLATENCY * rate / 1000;
	p->buffer = malloc(OPENSLES_BUFFERS * p->samples_per_buf * p->bytes_per_sample);
	if (!p->buffer) {
		LOG("buffer malloc failed");
		goto err;
	}

	p->latency = 0;
#ifdef CONFIG_ANDROID
	if (dl_as.getOutputLatency != NULL)
		dl_as.getOutputLatency(&p->latency, -1 /* AUDIO_STREAM_DEFAULT */);
#endif
	p->latency += OPENSLES_BUFFERS * OPENSLES_BUFLATENCY;

	ret = 0;
err:
	return ret;
}

static int opensles_start(audio_ctx_t *p)
{
DBG	LOG();

	if (p->playerPlay) {
		SLresult result = SLSetPlayState(p->playerPlay, SL_PLAYSTATE_PLAYING);
		RESULT_CHECK("failed to switch to play state");
	}

	return 0;
err:
	return -1;
}

static int opensles_stop(audio_ctx_t *p)
{
DBG	LOG();

	if (p->playerPlay) {
		SLresult result = SLSetPlayState(p->playerPlay, SL_PLAYSTATE_STOPPED);
		RESULT_CHECK("failed to switch to pause state");
	}
	SLClear(p->playerBufferQueue);

	p->started = 0;
	p->buffer_count = 0;
	p->buffer_in_idx = p->buffer_out_idx = 0;

	return 0;
err:
	return -1;
}

static int opensles_can_write(audio_ctx_t *p, int len)
{
	const size_t unit_size = p->samples_per_buf * p->bytes_per_sample;
	if (!unit_size)
		return 0;
	int nb_buf = len / unit_size + 1;
	int ret;

	ret = OPENSLES_BUFFERS - p->buffer_count > nb_buf;
	return ret;
}

static int opensles_write(audio_ctx_t *p, unsigned char *in, int in_len)
{
	const size_t unit_size = p->samples_per_buf * p->bytes_per_sample;
	size_t in_done = 0;
	SLresult result;

DBG2	LOG("len %d / %zd", in_len, unit_size);



	while (in_done < in_len) {
		size_t cur = in_len - in_done;

		if (cur > unit_size - p->buffer_done)
			cur = unit_size - p->buffer_done;

		memcpy(&p->buffer[unit_size * (p->buffer_in_idx % OPENSLES_BUFFERS) + p->buffer_done], in + in_done, cur);
		in_done += cur;
		p->buffer_done += cur;

		if (p->buffer_done == unit_size) {
			++p->buffer_in_idx;
			p->buffer_done = 0;
		}
	}

	while (p->buffer_in_idx > p->buffer_out_idx) {
		result = SLEnqueue(p->playerBufferQueue, &p->buffer[unit_size * (p->buffer_out_idx % OPENSLES_BUFFERS)], unit_size);
		RESULT_CHECK("failed to enqueue buffer");

		++p->buffer_out_idx;
		__sync_fetch_and_add(&p->buffer_count, 1);
	}

	return in_done;
err:
	return 0;
}

static int opensles_get_delay(audio_ctx_t *p)
{
DBG	LOG("delay %d", p->latency);

	return p->latency;
}

static void opensles_flush_output_locked(audio_ctx_t *p)
{
	SLSetPlayState(p->playerPlay, SL_PLAYSTATE_STOPPED);
	SLClear(p->playerBufferQueue);
	SLSetPlayState(p->playerPlay, SL_PLAYSTATE_PLAYING);

	p->started = 0;
	p->buffer_count = 0;
	p->buffer_in_idx = p->buffer_out_idx = 0;
}

static void opensles_flush_output(audio_ctx_t *p) 
{
DBG	LOG();
	opensles_flush_output_locked(p);
}

static int opensles_preload(audio_ctx_t *p)
{
	const size_t len = OPENSLES_BUFFERS * p->samples_per_buf * p->bytes_per_sample;
	const size_t unit_size = p->samples_per_buf * p->bytes_per_sample;

DBG	LOG();

	if (p->started)
		opensles_flush_output_locked(p);

	memset(p->buffer, 0, len);

	p->buffer_in_idx = OPENSLES_BUFFERS - 1;

	while (p->buffer_in_idx > p->buffer_out_idx) {
		SLresult result = SLEnqueue(p->playerBufferQueue, &p->buffer[unit_size * (p->buffer_out_idx % OPENSLES_BUFFERS)], unit_size);
		RESULT_CHECK("failed to enqueue buffer");

		++p->buffer_out_idx;
		__sync_fetch_and_add(&p->buffer_count, 1);
	}

	return 0;
err:
	return -1;
}

static int opensles_mute(audio_ctx_t *p, BOOL fade)
{
	SLSetMute(p->playerVolume, 1);
	return 0;
}

static int opensles_unmute(audio_ctx_t *p, BOOL fade, BOOL threaded)
{
	SLSetMute(p->playerVolume, 0);
	return 0;
}

const audio_interface_impl_t audio_interface_impl_opensles = {
	.name = "opensles",
	.init = opensles_init,
	.exit = opensles_exit,
	.open = opensles_open,
	.close = opensles_close,
	.start = opensles_start,
	.stop = opensles_stop,
	.can_write = opensles_can_write,
	.write = opensles_write,
	.set_output_params = opensles_set_output_params,
	.get_delay = opensles_get_delay,
	.flush_output = opensles_flush_output,
	.preload = opensles_preload,
	.mute = opensles_mute,
	.unmute = opensles_unmute
};

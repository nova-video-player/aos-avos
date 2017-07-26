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

#include "global.h"
#include "debug.h"
#include "types.h"
#include "audio_interface.h"
#include "android_audio.h"
#include "device_config.h"

#define DBG  if(0)
#define DBG2 if(0)
#define ERR  if(1)

#define LOG(fmt, ...) do { serprintf("%s(%p): " fmt "\n", __FUNCTION__, at, ##__VA_ARGS__); } while (0)

#define AUDIOTRACK_SIZE 512
typedef unsigned char bool;

#define NO_ERROR 0

#define DLHELPER_HEADER "dlhelper_audiotrack.h"
#include "dlhelper.h"

struct audio_ctx {
	int valid;
	uint8_t obj[];
};

static int audiotrack_init(void)
{
	if (dlhelper_at_init()) {
		LOG("dlhelper_at_init failed");
		return 1;
	} else {
		return 0;
	}
}

static void audiotrack_exit(void)
{
}

static audio_ctx_t *audiotrack_open(int mode)
{
	struct audio_ctx *at;

	if (mode != AUDIO_OUTPUT_MODE)
		return NULL;

	at = (audio_ctx_t *)calloc(1, sizeof(audio_ctx_t) + AUDIOTRACK_SIZE);
	if (!at) {
ERR		LOG("malloc failed");
		return NULL;
	}

	LOG("mode: %i", mode);

	return at;
}

static int audiotrack_close(audio_ctx_t **pat)
{
	audio_ctx_t *at = *pat;

DBG	LOG();
	
	if (at->valid) {
		dl_at.dtor(at->obj);
		at->valid = 0;
	}
	free(at);
	pat = NULL;
	return 0;
}

static uint32_t _convert_to_chanmask(int channels)
{
	switch (channels) {
	case 1:
		return AUDIO_CHANNEL_OUT_MONO;
	case 2:
		return AUDIO_CHANNEL_OUT_STEREO;
	case 3:
		return AUDIO_CHANNEL_OUT_STEREO | AUDIO_CHANNEL_OUT_LOW_FREQUENCY;
	case 4:
		return AUDIO_CHANNEL_OUT_SURROUND;
	case 6:
		return AUDIO_CHANNEL_OUT_5POINT1;
	case 8:
		return AUDIO_CHANNEL_OUT_7POINT1;
	default:
		return AUDIO_CHANNEL_OUT_STEREO;
	}
}

static audio_format_t _convert_to_format(int bits)
{
	switch (bits) {
	case 8:
		return AUDIO_FORMAT_PCM_8_BIT;
	case 16:
		return AUDIO_FORMAT_PCM_16_BIT;
	case 24:
		return AUDIO_FORMAT_PCM_8_24_BIT;
	case 32:
		return AUDIO_FORMAT_PCM_32_BIT;
	}
	return AUDIO_FORMAT_INVALID;
}

static int audiotrack_set_output_params(audio_ctx_t *at, int rate, int channels, int bits, int format)
{
	uint32_t track_chanmask;
	audio_format_t track_format;
	int status;
	int framecount;
	int flags = 0;

DBG	LOG("rate %d, channels %d, bits %d", rate, channels, bits);

	track_chanmask = _convert_to_chanmask(channels);
	if (track_chanmask == 0) {
ERR		LOG("cannot set channel number %d", channels);
		return -1;
	}

	track_format = _convert_to_format(bits);
	if (track_format == AUDIO_FORMAT_INVALID) {
ERR		LOG("cannot set bits %d", bits);
		return -1;
	}

	status = dl_at.getMinFrameCount(&framecount, AUDIO_STREAM_MUSIC, rate);
	if (status != NO_ERROR || framecount <= 0) {
ERR		LOG("cannot compute frame count");
		return -1;
	}

	if (at->valid) {
DBG		LOG("deleting track");
		dl_at.dtor(at->obj);
		at->valid = 0;
	}
	if (device_get_android_version() >= ANDROID_VERSION_JB &&
	    device_has_archos_enhancement()) {
		LOG("use AUDIO_OUTPUT_FLAG_DYNAMIC_RATE");
		flags = AUDIO_OUTPUT_FLAG_DYNAMIC_RATE;
	}
	dl_at.ctor(at->obj, AUDIO_STREAM_MUSIC, rate, track_format,
	    track_chanmask, framecount, flags, NULL, NULL, 0, 0);
	if (dl_at.initCheck(at->obj) != 0) {
ERR		LOG("initCheck failed");
		return -1;
	}
	at->valid = 1;

DBG	LOG("track created");

	return 0;
}

static int audiotrack_start(audio_ctx_t *at)
{
DBG	LOG();
	if (!at->valid) {
ERR		LOG("track not valid, error");
		return -1;
	}
	dl_at.start(at->obj);
	return 0;
}

static int audiotrack_stop(audio_ctx_t *at)
{
DBG	LOG();
	if (!at->valid) {
ERR		LOG("track not valid, error");
		return -1;
	}
	dl_at.stop(at->obj);
	return 0;
}

static int audiotrack_can_write(audio_ctx_t *at, int len)
{
	return 1;
}

static int audiotrack_write(audio_ctx_t *at, unsigned char *buffer, int len)
{
DBG2	LOG("len %d", len);
	if (!at->valid) {
ERR		LOG("track not valid, error");
		return -1;
	}

	if (dl_at.stopped(at->obj)) {
DBG		LOG("force start");
		dl_at.start(at->obj);
	}
	return dl_at.write(at->obj, buffer, len);
}

static int audiotrack_get_delay(audio_ctx_t *at)
{
	if (!at->valid) {
ERR		LOG("track not valid, error");
		return -1;
	}

DBG2	LOG("%d", dl_at.latency(at->obj));
	return dl_at.latency(at->obj);
}

static void audiotrack_flush_output(audio_ctx_t *at) 
{
DBG	LOG();
	if (!at->valid) {
ERR		LOG("track not valid, error");
		return;
	}

	if (!dl_at.stopped(at->obj))
		dl_at.stop(at->obj);
	dl_at.flush(at->obj);
}

static int audiotrack_preload(audio_ctx_t *at)
{
	unsigned char *buffer;
	size_t len;
	ssize_t ret;

DBG	LOG();
	if (!at->valid) {
ERR		LOG("track not valid, error");
		return -1;
	}

	len = dl_at.frameCount(at->obj) * dl_at.frameSize(at->obj) * dl_at.channelCount(at->obj);
	if ((buffer = (unsigned char *)malloc(len)) == NULL)
		return -1;
	memset(buffer, 0, len);
	ret = audio_interface_write(at, buffer, len);
	free(buffer);
	if (ret <= 0) {
ERR		LOG("preload failed\n");
		return -1;
	}

DBG	LOG("preloaded %zu bytes\n", ret);
	return ret;
}

static int audiotrack_get_session_id(audio_ctx_t *at)
{
DBG	LOG();
	if (!at->valid) {
ERR		LOG("track not valid, error");
		return -1;
	}

	return dl_at.getSessionId(at->obj);
}

const audio_interface_impl_t audio_interface_impl_audiotrack = {
	.name = "audiotrack",
	.init = audiotrack_init,
	.exit = audiotrack_exit,
	.open = audiotrack_open,
	.close = audiotrack_close,
	.start = audiotrack_start,
	.stop = audiotrack_stop,
	.can_write = audiotrack_can_write,
	.write = audiotrack_write,
	.set_output_params = audiotrack_set_output_params,
	.get_delay = audiotrack_get_delay,
	.flush_output = audiotrack_flush_output,
	.preload = audiotrack_preload,
	.get_session_id = audiotrack_get_session_id,
};

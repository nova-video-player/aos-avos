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
#include <pthread.h>

#include "global.h"
#include "debug.h"
#include "types.h"
#include "audio_interface.h"
#include "android_audio.h"
#include "device_config.h"
#include "av.h"
#include "atime.h"

#define DBG  if(0)
#define DBG2 if(0)
#define ERR  if(1)

#define LOG(fmt, ...) do { serprintf("%s(%p): " fmt "\n", __FUNCTION__, at, ##__VA_ARGS__); } while (0)

#define AUDIOTRACK_SIZE 1024 
typedef unsigned char bool;

#define NO_ERROR 0

#define DLHELPER_HEADER "dlhelper_audiotrack_new.h"
#include "dlhelper.h"

#define DLHELPER_HEADER "dlhelper_audiosystem.h"
#include "dlhelper.h"

#define DLHELPER_HEADER "dlhelper_utils.h"
#include "dlhelper.h"

struct audio_ctx {
	int init;
	int rate;
	int format;
	int frame_count;
	size_t frame_size;
	int channel_count;
	uint32_t latency;
	int passthrough;
	int direct;
	uint8_t obj[];
};

static int audiotrack_init(void)
{
	if (dlhelper_at_init() || !(dl_at.ctor || dl_at.ctor_legacy)) {
		LOG("dlhelper_at_init failed");
		return 1;
	}
	if (dlhelper_as_init()) {
		LOG("dlhelper_as_init failed");
		return 1;
	}
	if (dlhelper_utils_init()) {
		LOG("dlhelper_utils_init failed");
		return 1;
	}
	return 0;
}

static void audiotrack_exit(void)
{
}

static audio_ctx_t *audiotrack_open(int mode)
{
	audio_ctx_t *at;

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

	if (at->init) {
		dl_at.dtor(at->obj);
		at->init = 0;
	}
	free(at);
	pat = NULL;
	return 0;
}

static int audiotrack_set_output_params(audio_ctx_t *at, int rate, int channels, int bits, int format)
{
	uint32_t track_chanmask;
	audio_format_t track_format;
	int status;
	int flags = 0;

DBG	LOG("rate %d, channels %d, bits %d, format %d, passthrough mode %d", rate, channels, bits, format, at->passthrough);

	at->rate = rate;
	at->channel_count = channels;
	at->format = format;
	switch (at->channel_count) {
		case 1:
			track_chanmask = AUDIO_CHANNEL_OUT_MONO;
			break;
		case 2:
			track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
			break;
		case 3:
			track_chanmask = AUDIO_CHANNEL_OUT_STEREO | AUDIO_CHANNEL_OUT_LOW_FREQUENCY;
			break;
		case 4:
			track_chanmask = AUDIO_CHANNEL_OUT_SURROUND;
			break;
		case 6:
			track_chanmask = AUDIO_CHANNEL_OUT_5POINT1;
			break;
		case 8:
			track_chanmask = AUDIO_CHANNEL_OUT_7POINT1;
			break;
		default:
			track_chanmask = AUDIO_CHANNEL_OUT_STEREO;
			break;
	}

	switch (bits) {
		case 8:
			track_format = AUDIO_FORMAT_PCM_8_BIT;
			break;
		case 16:
			track_format = AUDIO_FORMAT_PCM_16_BIT;
			break;
		case 24:
			track_format = AUDIO_FORMAT_PCM_8_24_BIT;
			break;
		case 32:
			track_format = AUDIO_FORMAT_PCM_32_BIT;
			break;
		default:
			ERR		LOG("cannot set bits %d", bits);
			return -1;
	}
	at->frame_size = bits / 8 * at->channel_count;

	if(at->passthrough == 2) {
		at->frame_size = bits / 8;
		switch (at->format) {
			case WAVE_FORMAT_AC3:
				track_format = AUDIO_FORMAT_AC3;
				break;
			case WAVE_FORMAT_EAC3:
				track_format = AUDIO_FORMAT_E_AC3;
				break;
			case WAVE_FORMAT_DTS:
				track_format = AUDIO_FORMAT_DTS;
				break;
			case WAVE_FORMAT_DTS_HD:
				track_format = AUDIO_FORMAT_DTS_HD;
				break;
			case WAVE_FORMAT_TRUEHD:
				track_format = AUDIO_FORMAT_TRUEHD;
				break;
			default:
				track_format = AUDIO_FORMAT_PCM_16_BIT;
		}
	}

	int audio_stream = AUDIO_STREAM_MUSIC;
	if(at->passthrough == 1 &&
			( device_get_hw_type() == HW_TYPE_RK32 ||
			  device_get_hw_type() == HW_TYPE_RK30 ||
			  device_get_hw_type() == HW_TYPE_RK29))
		audio_stream = AUDIO_STREAM_VOICE_CALL;

	status = dl_at.getMinFrameCount(&at->frame_count, audio_stream, rate);
	if (status != NO_ERROR || at->frame_count <= 0) {
ERR		LOG("cannot compute frame count");
		return -1;
	}

	int reinit = 0;
	if (at->init) {
DBG		LOG("deleting track");
		dl_at.dtor(at->obj);
		memset(at->obj, 0, AUDIOTRACK_SIZE);
		at->init = 0;
		reinit = 1;
	}
	if (device_get_android_version() >= ANDROID_VERSION_JB &&
			device_has_archos_enhancement()) {
		LOG("use AUDIO_OUTPUT_FLAG_DYNAMIC_RATE");
		flags = AUDIO_OUTPUT_FLAG_DYNAMIC_RATE;
	}

	if(at->passthrough || at->direct)
		flags = AUDIO_OUTPUT_FLAG_DIRECT;

	if (dl_at.ctor) { /* API >= 19 (android 4.4 and more) */
		dl_at.ctor(at->obj, audio_stream, rate, track_format,
				track_chanmask, at->frame_count, flags, NULL, NULL, 0, 0, 0, NULL, -1/*uid*/, -1/*pid*/, NULL, 0);
		/*
		 * Increment a refcount so that AudioTrack is not deleted by destructor of by mDeathNotifier.
		 * (delete will crash since we didn't called 'new' to alloc/construct that object)
		 */
		dl_utils.RefBase_incStrong(at->obj, at);
	} else {
		dl_at.ctor_legacy(at->obj, audio_stream, rate, track_format,
				track_chanmask, at->frame_count, flags, NULL, NULL, 0, 0);
	}
	/*
	 * initCheck() that return mStatus is not available via dlsym on jb 4.3
	 */
	int failed = 0;
	status = dl_at.initCheck ? dl_at.initCheck(at->obj) : 0;
	if (status != 0) {
ERR		LOG("audiotrack ctor failed");
		failed = 1;
	}
	status = dl_at.stopped ? dl_at.stopped(at->obj) : 0;
	if (status == 0) {
ERR		LOG("audiotrack ctor failed (checked with stopped)");
		failed = 1;
	}

	if (failed && reinit) {
		msec_sleep( 30 );
		return audiotrack_set_output_params(at, rate, channels, bits, format);
	}

	//Retrying with flag direct
	if(failed && !at->direct) {
		at->direct = 1;
		return audiotrack_set_output_params(at, rate, channels, bits, format);
	}
	if(failed)
		return -1;

	if (dl_as.getOutputLatency(&at->latency, audio_stream) != 0)
		at->latency = 0;
	at->latency += (1000 * at->frame_count) / at->rate;
	at->init = 1;

DBG	LOG("track created");

	return 0;
}

static int audiotrack_set_passthrough(audio_ctx_t *at, int passthrough)
{
	at->passthrough = passthrough;
	audiotrack_set_output_params(at, at->rate, at->channel_count, (passthrough == 2) ? 16 : at->frame_size * 8 / at->channel_count, at->format);
	return 0;
}

static int audiotrack_get_passthrough(audio_ctx_t *at)
{
	return at->passthrough;
}

static int audiotrack_start(audio_ctx_t *at)
{
DBG	LOG();
	if (!at->init) {
ERR		LOG("track not valid, error");
		return -1;
	}
	dl_at.start(at->obj);
	return 0;
}

static int audiotrack_stop(audio_ctx_t *at)
{
DBG	LOG();
	if (!at->init) {
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
	if (!at->init) {
ERR		LOG("track not valid, error");
		return -1;
	}

	if (dl_at.stopped(at->obj)) {
DBG		LOG("force start");
		dl_at.start(at->obj);
	}

	ssize_t ret = dl_at.write(at->obj, buffer, len, 1);
DBG2	LOG("wrote %zu", ret);

	if (at->passthrough && ret == -32) {
		audiotrack_set_passthrough(at, at->passthrough);
	}
	return ret;
}

static int audiotrack_get_delay(audio_ctx_t *at)
{
	if (!at->init) {
ERR		LOG("track not valid, error");
		return -1;
	}
DBG2	LOG("%d:", at->latency);
	return at->latency;
}

static void audiotrack_flush_output(audio_ctx_t *at)
{
DBG	LOG();
	if (!at->init) {
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
	if (!at->init) {
ERR		LOG("track not valid, error");
		return -1;
	}

	len = at->frame_count * at->frame_size * ((at->passthrough == 2) ? 1 : at->channel_count);
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
	return -1;
}

const audio_interface_impl_t audio_interface_impl_audiotrack_new = {
	.name = "audiotrack_new",
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
	.set_passthrough = audiotrack_set_passthrough,
	.get_passthrough = audiotrack_get_passthrough,
};

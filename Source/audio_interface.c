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

#include <stdio.h>
#include <stdlib.h>

#include "global.h"
#include "debug.h"
#include "audio_interface.h"
#include "device_config.h"

static int audio_interface_force = -1;

static float audio_speed = 1.0f;
static float previous_audio_speed = 1.0f;

static int is_audio_speed_enabled = 0;

#ifdef CONFIG_ANDROID
extern const audio_interface_impl_t audio_interface_impl_opensles;
extern const audio_interface_impl_t audio_interface_impl_audiotrack;
extern const audio_interface_impl_t audio_interface_impl_audiotrack_new;
extern const audio_interface_impl_t audio_interface_impl_audiotrack_java;
int spdif_is_passthrough_on();
#else
#ifndef __APPLE__
extern const audio_interface_impl_t audio_interface_impl_oss;
#endif
#endif

extern const audio_interface_impl_t audio_interface_impl_null;

static const audio_interface_impl_t *impl = NULL;

int audio_interface_init(void)
{
#ifdef CONFIG_ANDROID
	int i;
	const audio_interface_impl_t *impl_list[3];

	int audio_interface = device_config_get_audio_interface();

	if (audio_interface_force != -1 ) { // local source code force first
		switch (audio_interface_force) {
		case 0:
			impl_list[0] = &audio_interface_impl_null;
			break;
		case 1:
			impl_list[0] = &audio_interface_impl_audiotrack;
			break;
		case 2:
			impl_list[0] = &audio_interface_impl_audiotrack_new;
			break;
		case 3: impl_list[0] = &audio_interface_impl_audiotrack_java;
			break;
		default:
			impl_list[0] = &audio_interface_impl_opensles;
			break;
		}
		impl_list[1] = NULL;
	} else {
		if (audio_interface != 0) { // libavos forced
			switch (audio_interface) {
			case 1:
				impl_list[0] = &audio_interface_impl_audiotrack_java;
				break;
			case 2:
				impl_list[0] = &audio_interface_impl_opensles;
				break;
			default:
				impl_list[0] = &audio_interface_impl_audiotrack_java;
				break;
			}
			impl_list[1] = NULL;
		} else { // normal selection logic
			if (device_get_android_api() >= 24) // Nougat 7.0
				impl_list[0] = &audio_interface_impl_audiotrack_java;
			else
				impl_list[0] = &audio_interface_impl_audiotrack_new;
			impl_list[1] = &audio_interface_impl_opensles;
			impl_list[2] = NULL;
		}
	}
	for (i = 0; impl_list[i] != NULL && impl == NULL; ++i) {
		if (impl_list[i]->init() == 0) {
			serprintf("audio_interface_init: %s success\n", impl_list[i]->name);
			impl = impl_list[i];
		} else {
			serprintf("audio_interface_init: %s failed, ", impl_list[i]->name);
			if (impl_list[i + 1] != NULL)
				serprintf("trying next: %s\n", impl_list[i + 1]->name);
			else
				serprintf("all interfaces failed, no sound\n");

		}
	}
#else
#ifndef __APPLE__
	if (!audio_interface_impl_oss.init()) {
		impl = &audio_interface_impl_oss;
		serprintf("audio_interface_init: %s\n", impl->name);
	}
#else
 impl = &audio_interface_impl_null;
 serprintf("audio_interface_init: %s\n", impl->name);
#endif
#endif
	
	return impl ? 0 : -1;
}

int audio_interface_exit(void)
{
	if (impl) {
		impl->exit();
		impl = NULL;
	}
	return 0;
}

audio_ctx_t *audio_interface_open(int mode)
{
	return impl ? impl->open(mode) : NULL;
}

int audio_interface_close(audio_ctx_t **ctx)
{
	return impl->close(ctx);
}

int audio_interface_start(audio_ctx_t *ctx)
{
	return impl->start(ctx);
}

int audio_interface_stop(audio_ctx_t *ctx)
{
	return impl->stop(ctx);
}

int audio_interface_can_write(audio_ctx_t *ctx, int len)
{
	return impl->can_write(ctx, len);
}

int audio_interface_write(audio_ctx_t *ctx, unsigned char *data, int data_length)
{
	return impl->write(ctx, data, data_length);
}

int audio_interface_set_output_params(audio_ctx_t *ctx, int freq, int channels, int bits, int format)
{
	return impl->set_output_params(ctx, freq, channels, bits, format);
}

int audio_interface_get_delay(audio_ctx_t *ctx)
{
	return impl->get_delay(ctx);
}

void audio_interface_flush_output(audio_ctx_t *ctx) 
{
	impl->flush_output(ctx);
}

int audio_interface_preload(audio_ctx_t *ctx)
{
	return impl->preload ? impl->preload(ctx) : -1;
}

int audio_interface_mute(audio_ctx_t *ctx, BOOL fade)
{
	return impl->mute ? impl->mute(ctx, fade) : -1;
}

int audio_interface_unmute(audio_ctx_t *ctx, BOOL fade, BOOL threaded)
{
	return impl->unmute ? impl->unmute(ctx, fade, threaded) : -1;
}

int audio_interface_set_output_volume(audio_ctx_t *ctx, int volume, int balance)
{
	return impl->set_output_volume ? impl->set_output_volume(ctx, volume, balance) : -1;
}

int audio_interface_set_output_volume_l_r(audio_ctx_t *ctx, int vol_l, int vol_r)
{
	return impl->set_output_volume_l_r ? impl->set_output_volume_l_r(ctx, vol_l, vol_r) : -1;
}

int audio_interface_get_session_id(audio_ctx_t *ctx)
{
	return impl->get_session_id ? impl->get_session_id(ctx) : -1;
}

int audio_interface_set_passthrough(audio_ctx_t *ctx, int pass)
{
	return impl->set_passthrough ? impl->set_passthrough(ctx, pass) : -1;
}

int audio_interface_get_passthrough(audio_ctx_t *ctx)
{
	return impl->get_passthrough ? impl->get_passthrough(ctx) : 0;
}

void audio_interface_set_audio_speed(float speed)
{
	previous_audio_speed = audio_speed;
	audio_speed = speed;
}

float audio_interface_get_audio_speed()
{
	return audio_speed;
}

float audio_interface_get_previous_audio_speed()
{
	return previous_audio_speed;
}

void audio_interface_enable_audio_speed(int enable)
{
	is_audio_speed_enabled = enable;
}

int audio_interface_is_audio_speed_enabled()
{
	return is_audio_speed_enabled;
}

int audio_interface_change_audio_speed(audio_ctx_t *ctx, float speed) {
	return impl->change_audio_speed ? impl->change_audio_speed(ctx, speed) : -1;
}

#ifdef DEBUG_MSG
static void audio_interface_force_cmd(int argc, char *argv[])
{
	if (argc > 1) {
		audio_interface_force = atoi(argv[1]);
		impl = NULL;
		audio_interface_init();
	}
}
DECLARE_DEBUG_COMMAND("aif", audio_interface_force_cmd);
#endif

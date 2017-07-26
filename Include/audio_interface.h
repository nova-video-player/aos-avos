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

#ifndef _AUDIO_INTERFACE_H
#define _AUDIO_INTERFACE_H

#include "types.h"

#define AUDIO_VOLUME_MAX 100
#define AUDIO_VOLUME_MIN 0
#define AUDIO_VOLUME_DEFAULT AUDIO_VOLUME_MAX

#define AUDIO_BALANCE_MIN (-10)
#define AUDIO_BALANCE_MAX (10)

enum {
	AUDIO_INPUT_MODE,
	AUDIO_OUTPUT_MODE,
	AUDIO_INPUT_OUTPUT_MODE,
	AUDIO_MIXER_MODE,
};

typedef struct audio_ctx audio_ctx_t;

typedef int (*audio_interface_impl_init)(void);
typedef void (*audio_interface_impl_exit)(void);
typedef audio_ctx_t* (*audio_interface_impl_open)(int);
typedef int (*audio_interface_impl_close)(audio_ctx_t **ctx);
typedef int (*audio_interface_impl_start)(audio_ctx_t *ctx);
typedef int (*audio_interface_impl_stop)(audio_ctx_t *ctx);
typedef int (*audio_interface_impl_can_write)(audio_ctx_t *ctx, int len);
typedef int (*audio_interface_impl_write)(audio_ctx_t *ctx, unsigned char *data, int data_length);
typedef int (*audio_interface_impl_set_output_params)(audio_ctx_t *ctx, int freq, int channels, int bits, int format);
typedef int (*audio_interface_impl_get_delay)(audio_ctx_t *ctx);
typedef void (*audio_interface_impl_flush_output)(audio_ctx_t *ctx);
typedef int (*audio_interface_impl_preload)(audio_ctx_t *ctx);
typedef int (*audio_interface_impl_mute)(audio_ctx_t *ctx, BOOL fade);
typedef int (*audio_interface_impl_unmute)(audio_ctx_t *ctx, BOOL fade, BOOL threaded);
typedef int (*audio_interface_impl_set_output_volume)(audio_ctx_t *ctx, int volume, int balance);
typedef int (*audio_interface_impl_set_output_volume_l_r)(audio_ctx_t *ctx, int vol_l, int vol_r);
typedef int (*audio_interface_impl_get_session_id)(audio_ctx_t *ctx);
typedef int (*audio_interface_impl_set_passthrough)(audio_ctx_t *ctx, int pass);
typedef int (*audio_interface_impl_get_passthrough)(audio_ctx_t *ctx);


typedef struct audio_interface_impl {
	const char *name;
	audio_interface_impl_init init;
	audio_interface_impl_exit exit;
	audio_interface_impl_open open;
	audio_interface_impl_close close;
	audio_interface_impl_start start;
	audio_interface_impl_stop stop;
	audio_interface_impl_can_write can_write;
	audio_interface_impl_write write;
	audio_interface_impl_set_output_params set_output_params;
	audio_interface_impl_get_delay get_delay;
	audio_interface_impl_flush_output flush_output;
	audio_interface_impl_preload preload;
	audio_interface_impl_mute mute;
	audio_interface_impl_unmute unmute;
	audio_interface_impl_set_output_volume set_output_volume;
	audio_interface_impl_set_output_volume_l_r set_output_volume_l_r;
	audio_interface_impl_get_session_id get_session_id;
	audio_interface_impl_get_passthrough get_passthrough;
	audio_interface_impl_set_passthrough set_passthrough;
} audio_interface_impl_t;

int audio_interface_init(void);
int audio_interface_exit(void);
audio_ctx_t *audio_interface_open(int mode);
int audio_interface_close(audio_ctx_t **ctx);
int audio_interface_start(audio_ctx_t *ctx);
int audio_interface_stop(audio_ctx_t *ctx);
int audio_interface_can_write(audio_ctx_t *ctx, int len);
int audio_interface_write(audio_ctx_t *ctx, unsigned char *data, int data_length);
int audio_interface_set_output_params(audio_ctx_t *ctx, int freq, int channels, int bits, int format);
int audio_interface_get_delay(audio_ctx_t *ctx);
void audio_interface_flush_output(audio_ctx_t *ctx);
int audio_interface_preload(audio_ctx_t *ctx);
int audio_interface_mute(audio_ctx_t *ctx, BOOL fade);
int audio_interface_unmute(audio_ctx_t *ctx, BOOL fade, BOOL threaded);

int audio_interface_set_output_volume(audio_ctx_t *ctx, int volume, int balance);
int audio_interface_set_output_volume_l_r(audio_ctx_t *ctx, int vol_l, int vol_r);

int audio_interface_get_session_id(audio_ctx_t *ctx);

int audio_interface_set_passthrough(audio_ctx_t *ctx, int pass);
int audio_interface_get_passthrough(audio_ctx_t *ctx);

#endif

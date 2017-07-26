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
#include <pthread.h>

#include "global.h"
#include "debug.h"
#include "types.h"
#include "audio_interface.h"

struct audio_ctx {
};

static int null_close(audio_ctx_t **pp)
{
	audio_ctx_t *p = *pp;
	free(p);
	*pp = NULL;
	return 0;
}

static int null_init(void)
{
	return 0;
}

audio_ctx_t *null_open(int mode)
{
	audio_ctx_t *p;

	p = (audio_ctx_t *)calloc(1, sizeof(audio_ctx_t));

	return p;
}

static int null_set_output_params(audio_ctx_t *p, int rate, int channels, int bits, int format)
{
	return 0;
}

static int null_start(audio_ctx_t *p)
{
	return 0;
}

static int null_stop(audio_ctx_t *p)
{
	return 0;
}

static int null_can_write(audio_ctx_t *p, int len)
{
	return 1;
}

static int null_write(audio_ctx_t *p, unsigned char *in, int in_len)
{
	return in_len;
}

static int null_get_delay(audio_ctx_t *p)
{
	return 0;
}

static void null_flush_output(audio_ctx_t *p) 
{
}

static int null_preload(audio_ctx_t *p)
{
	return 0;
}

const audio_interface_impl_t audio_interface_impl_null = {
	.name = "null",
	.init = null_init,
	.open = null_open,
	.close = null_close,
	.start = null_start,
	.stop = null_stop,
	.can_write = null_can_write,
	.write = null_write,
	.set_output_params = null_set_output_params,
	.get_delay = null_get_delay,
	.flush_output = null_flush_output,
	.preload = null_preload,
};

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
#include <dlfcn.h>

typedef struct sfdec_priv_t sfdec_priv_t;
#include "dec_audio_priv.h"
#include "dec_audio.h"

#undef LOG
#define LOG(fmt, ...) do { \
    printf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
    fflush(stdout); \
} while (0)

struct dec_audio* dec_audio_new(	sfdec_codec_t codec,int64_t duration_us, int input_size,int samplesPerSec, int channels, int bitrate, void *extradata, size_t extradata_size, int64_t codec_delay, int64_t seek_preroll)
{
	struct dec_audio *dec;
	void *itf = NULL;

	dlerror();
	itf = dlsym(RTLD_DEFAULT, "dec_audio_mediacodec");

	if (!itf) {
		LOG("dec_audio_new failed: dlsym error: %s\n", dlerror());
		return NULL;
	}
	dec = calloc(1, sizeof(struct dec_audio));
	if (!dec)
		return NULL;

	dec->itf = (const dec_audio_itf_t *) itf;
	(void)bitrate;
	dec->priv = dec->itf->init(codec, duration_us, input_size,
		    extradata, extradata_size,
		    samplesPerSec, channels,
		    codec_delay, seek_preroll);
	if (!dec->priv) {
		free(dec);
		return NULL;
	}
	return dec;
}

void dec_audio_delete(struct dec_audio *dec)
{
	dec->itf->destroy(dec->priv);
	free(dec);
}

int dec_audio_start(struct dec_audio *dec)
{
	return dec->itf->start(dec->priv);
}

int dec_audio_stop(struct dec_audio *dec)
{
	return dec->itf->stop(dec->priv);
}

ssize_t dec_audio_send_input(struct dec_audio *dec, void *data, size_t size, int64_t time_us, int is_sync_frame, int wait)
{
	return dec->itf->send_input(dec->priv, data, size, time_us, is_sync_frame, wait);
}

int dec_audio_flush(struct dec_audio *dec)
{
	return dec->itf->flush(dec->priv);
}

int dec_audio_stop_input(struct dec_audio *dec)
{
	return dec->itf->stop_input(dec->priv);
}

int dec_audio_read(struct dec_audio *dec, int64_t seek, sfdec_read_out_t *read_out)
{
	return dec->itf->read(dec->priv, seek, read_out);
}

int dec_audio_buf_render(struct dec_audio *dec, sfbuf_t *sfbuf, int render)
{
	return dec->itf->buf_render(dec->priv, sfbuf, render, 1, 0);
}

int dec_audio_buf_release(struct dec_audio *dec, sfbuf_t *sfbuf)
{
	return dec->itf->buf_release(dec->priv, sfbuf);
}

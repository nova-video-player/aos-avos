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
#include "sfdec_priv.h"
#include "dec_audio.h"

#undef LOG
#define LOG(fmt, ...) do { \
    printf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
    fflush(stdout); \
} while (0)

sfdec_t* dec_audio_new(	sfdec_codec_t codec,int64_t duration_us, int input_size,int samplesPerSec, int channels, int bitrate, void *extradata, size_t extradata_size, int64_t codec_delay, int64_t seek_preroll)
{
	sfdec_t *dec_audio;
	void *itf = NULL;

	dlerror();
	itf = dlsym(RTLD_DEFAULT, "dec_audio_mediacodec");
		
	if (!itf) {
		LOG("dec_audio_new failed: dlsym error: %s\n", dlerror());
		return NULL;
	}
	dec_audio = calloc(1, sizeof(sfdec_t));
	if (!dec_audio)
		return NULL;

	dec_audio->itf = (const sfdec_itf_t *) itf;
	dec_audio->priv = dec_audio->itf->init(codec, 0,
		    0,0,0,
		    duration_us,input_size,
		    NULL, extradata, extradata_size,
		    0, samplesPerSec, channels, bitrate,
		    codec_delay, seek_preroll);
	if (!dec_audio->priv) {
		free(dec_audio);
		return NULL;
	}
	return dec_audio;
}

void dec_audio_delete(sfdec_t *dec_audio)
{
	dec_audio->itf->destroy(dec_audio->priv);
	free(dec_audio);
}

int dec_audio_start(sfdec_t *dec_audio)
{
	return dec_audio->itf->start(dec_audio->priv);
}

int dec_audio_stop(sfdec_t *dec_audio)
{
	return dec_audio->itf->stop(dec_audio->priv);
}

ssize_t dec_audio_send_input(sfdec_t *dec_audio, void *data, size_t size, int64_t time_us, int is_sync_frame, int wait)
{
	return dec_audio->itf->send_input(dec_audio->priv, data, size, time_us, is_sync_frame, wait);
}

int dec_audio_flush(sfdec_t *dec_audio)
{
	return dec_audio->itf->flush(dec_audio->priv);
}

int dec_audio_stop_input(sfdec_t *dec_audio)
{
	return dec_audio->itf->stop_input(dec_audio->priv);
}

int dec_audio_read(sfdec_t *dec_audio, int64_t seek, sfdec_read_out_t *read_out)
{
	return dec_audio->itf->read(dec_audio->priv, seek, read_out);
}

int dec_audio_buf_render(sfdec_t *dec_audio, sfbuf_t *sfbuf, int render)
{
	return dec_audio->itf->buf_render(dec_audio->priv, sfbuf, render);
}

int dec_audio_buf_release(sfdec_t *dec_audio, sfbuf_t *sfbuf)
{
	return dec_audio->itf->buf_release(dec_audio->priv, sfbuf);
}

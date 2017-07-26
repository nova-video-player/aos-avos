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

#undef LOG
#define LOG(fmt, ...) do { \
    printf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
    fflush(stdout); \
} while (0)

sfdec_t* sfdec_new(sfdec_type_t type,
    sfdec_codec_t codec,
    sfdec_flags_t flags,
    int *width, int *height, int rotation,
    int64_t duration_us, int input_size,
    void *surface_handle,
    void *extradata, size_t extradata_size,
    int *pts_reorder)
{
	sfdec_t *sfdec;
	void *itf = NULL;

	dlerror();
	switch (type) {
		case SFDEC_TYPE_OMXCODEC:
			itf = dlsym(RTLD_DEFAULT, "sfdec_itf_omxcodec");
			break;
		case SFDEC_TYPE_MEDIACODEC:
			itf = dlsym(RTLD_DEFAULT, "sfdec_itf_mediacodec");
			break;
		case DEC_TYPE_MEDIACODEC_AUDIO:
			itf = dlsym(RTLD_DEFAULT, "dec_audio_mediacodec");
			break;
		default:
			LOG("sfdec_new failed: invalid sfdec_type_t");
			return NULL;
	}
	if (!itf) {
		LOG("sfdec_new failed: dlsym error: %s\n", dlerror());
		return NULL;
	}
	sfdec = calloc(1, sizeof(sfdec_t));
	if (!sfdec)
		return NULL;

	sfdec->itf = (const sfdec_itf_t *) itf;
	sfdec->priv = sfdec->itf->init(codec,
			flags,
			width, height, rotation,
			duration_us, input_size,
			surface_handle,
			extradata, extradata_size, pts_reorder,0,0,0,0,0);
	if (!sfdec->priv) {
		free(sfdec);
		return NULL;
	}
	return sfdec;
}

void sfdec_delete(sfdec_t *sfdec)
{
	sfdec->itf->destroy(sfdec->priv);
	free(sfdec);
}

int sfdec_start(sfdec_t *sfdec)
{
	return sfdec->itf->start(sfdec->priv);
}

int sfdec_stop(sfdec_t *sfdec)
{
	return sfdec->itf->stop(sfdec->priv);
}

ssize_t sfdec_send_input(sfdec_t *sfdec, void *data, size_t size, int64_t time_us, int is_sync_frame, int wait)
{
	return sfdec->itf->send_input(sfdec->priv, data, size, time_us, is_sync_frame, wait);
}

int sfdec_flush(sfdec_t *sfdec)
{
	return sfdec->itf->flush(sfdec->priv);
}

int sfdec_stop_input(sfdec_t *sfdec)
{
	return sfdec->itf->stop_input(sfdec->priv);
}

int sfdec_read(sfdec_t *sfdec, int64_t seek, sfdec_read_out_t *read_out)
{
	return sfdec->itf->read(sfdec->priv, seek, read_out);
}

int sfdec_buf_render(sfdec_t *sfdec, sfbuf_t *sfbuf, int render)
{
	return sfdec->itf->buf_render(sfdec->priv, sfbuf, render);
}

int sfdec_buf_release(sfdec_t *sfdec, sfbuf_t *sfbuf)
{
	return sfdec->itf->buf_release(sfdec->priv, sfbuf);
}

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

#ifndef _SFDEC_PRIV_H
#define _SFDEC_PRIV_H

#include "sfdec.h"

typedef sfdec_priv_t* (*sfdec_init_t)(sfdec_codec_t codec,
		    sfdec_flags_t flags,
		    int *width, int *height, int rotation,
		    int64_t duration_us, int input_size,
		    void *surface_handle,
		    void *extradata, size_t extradata_size,
		    int *pts_reorder, int sampleSize, int channels, int bitrate,
		    int64_t codec_delay, int64_t seek_preroll);

typedef void	(*sfdec_destroy_t)(sfdec_priv_t *sfdec);
typedef int	(*sfdec_start_t)(sfdec_priv_t *);
typedef int	(*sfdec_stop_t)(sfdec_priv_t *);
typedef ssize_t	(*sfdec_send_input_t)(sfdec_priv_t *sfdec, void *data, size_t size, int64_t time_us, int is_sync_frame, int wait);
typedef int	(*sfdec_flush_t)(sfdec_priv_t *sfdec);
typedef int	(*sfdec_stop_input_t)(sfdec_priv_t *sfdec);
typedef int	(*sfdec_read_t)(sfdec_priv_t *sfdec, int64_t seek, sfdec_read_out_t *read_out);
typedef int	(*sfdec_buf_render_t)(sfdec_priv_t *sfdec, sfbuf_t *sfbuf, int render);
typedef int	(*sfdec_buf_release_t)(sfdec_priv_t *sfdec, sfbuf_t *sfbuf);

typedef struct sfdec_itf {
	const char *name;
	sfdec_init_t init;
	sfdec_destroy_t destroy;
	sfdec_start_t start;
	sfdec_stop_t stop;
	sfdec_send_input_t send_input;
	sfdec_flush_t flush;
	sfdec_stop_input_t stop_input;
	sfdec_read_t read;
	sfdec_buf_render_t buf_render;
	sfdec_buf_release_t buf_release;
} sfdec_itf_t;

struct sfdec {
	const sfdec_itf_t *itf;
	sfdec_priv_t *priv;
};

#endif

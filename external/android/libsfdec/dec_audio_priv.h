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

#ifndef _DEC_AUDIO_PRIV_H
#define _DEC_AUDIO_PRIV_H

#include "sfdec.h"

// Audio-specific init: proper int64_t params instead of overloaded video slots
typedef sfdec_priv_t* (*dec_audio_init_t)(sfdec_codec_t codec,
		    int64_t duration_us, int input_size,
		    void *extradata, size_t extradata_size,
		    int samplesPerSec, int channels,
		    int64_t codec_delay, int64_t seek_preroll);

typedef void	(*dec_audio_destroy_t)(sfdec_priv_t *);
typedef int	(*dec_audio_start_t)(sfdec_priv_t *);
typedef int	(*dec_audio_stop_t)(sfdec_priv_t *);
typedef ssize_t	(*dec_audio_send_input_t)(sfdec_priv_t *, void *, size_t, int64_t, int, int);
typedef int	(*dec_audio_flush_t)(sfdec_priv_t *);
typedef int	(*dec_audio_stop_input_t)(sfdec_priv_t *);
typedef int	(*dec_audio_read_t)(sfdec_priv_t *, int64_t, sfdec_read_out_t *);
typedef int	(*dec_audio_buf_render_t)(sfdec_priv_t *, sfbuf_t *, int, int, int64_t);
typedef int	(*dec_audio_buf_release_t)(sfdec_priv_t *, sfbuf_t *);

typedef struct dec_audio_itf {
	const char *name;
	dec_audio_init_t init;
	dec_audio_destroy_t destroy;
	dec_audio_start_t start;
	dec_audio_stop_t stop;
	dec_audio_send_input_t send_input;
	dec_audio_flush_t flush;
	dec_audio_stop_input_t stop_input;
	dec_audio_read_t read;
	dec_audio_buf_render_t buf_render;
	dec_audio_buf_release_t buf_release;
} dec_audio_itf_t;

struct dec_audio {
	const dec_audio_itf_t *itf;
	sfdec_priv_t *priv;
};

#endif

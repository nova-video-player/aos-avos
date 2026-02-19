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

#ifndef _DEC_AUDIO_H
#define _DEC_AUDIO_H
#include "sfdec.h"

#include <unistd.h>
#include <stdint.h>
#if __cplusplus
extern "C" {
#endif

struct dec_audio;

struct dec_audio*	dec_audio_new( sfdec_codec_t codec,int64_t duration_us, int input_size, int samplesPerSec, int channels, int bitrate,
	void *extradata, size_t extradata_size, int64_t codec_delay, int64_t seek_preroll);

void		dec_audio_delete(struct dec_audio *dec);
int		dec_audio_start(struct dec_audio *);
int		dec_audio_stop(struct dec_audio *);
ssize_t		dec_audio_send_input(struct dec_audio *dec, void *data, size_t size, int64_t time_us, int is_sync_frame, int wait);
int		dec_audio_flush(struct dec_audio *dec);
int		dec_audio_stop_input(struct dec_audio *dec);
int		dec_audio_read(struct dec_audio *dec, int64_t seek, sfdec_read_out_t *read_out);
int		dec_audio_buf_render(struct dec_audio *dec, sfbuf_t *sfbuf, int render);
int		dec_audio_buf_release(struct dec_audio *dec, sfbuf_t *sfbuf);

#if __cplusplus
}
#endif

#endif

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

#ifndef _SFDEC_H
#define _SFDEC_H

#include <unistd.h>
#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

typedef struct sfdec sfdec_t;
typedef struct sfbuf sfbuf_t;

typedef enum sfdec_type {
	SFDEC_TYPE_UNKNOWN,
	SFDEC_TYPE_OMXCODEC,
	SFDEC_TYPE_MEDIACODEC,
	DEC_TYPE_MEDIACODEC_AUDIO
} sfdec_type_t;

typedef enum sfdec_codec_type {
	SFDEC_VIDEO_UNKNOWN,
	SFDEC_VIDEO_VP8,
	SFDEC_VIDEO_VP9,
	SFDEC_VIDEO_AVC,
	SFDEC_VIDEO_HEVC,
	SFDEC_VIDEO_MPEG4,
	SFDEC_VIDEO_H263,
	SFDEC_VIDEO_MPEG2,
	SFDEC_VIDEO_WMV,
	SFDEC_VIDEO_RAW,
	SFDEC_AUDIO_AC3,
	SFDEC_AUDIO_EAC3,
	SFDEC_AUDIO_DTS,
	SFDEC_AUDIO_DTS_HD,
	SFDEC_AUDIO_OPUS
} sfdec_codec_t;

typedef enum sfdec_flags {
	SFDEC_FLAG_SWDEC = 0x01,
} sfdec_flags_t;

enum {
	SFDEC_READ_INVALID = 0,
	SFDEC_READ_BUF  = 0x01,
	SFDEC_READ_SIZE = 0x02
};

typedef struct sfdec_read_out {
	int flag;
	struct {
		int64_t time_us;
		sfbuf_t *sfbuf;
	  uint8_t * out;
		int32_t out_size;
	} buf;
	int32_t channels;
	int32_t samplesPerSec;
	int32_t bitRate;
	struct {
		int32_t width;
		int32_t height;
		int32_t interlaced;
	} size;
} sfdec_read_out_t;

sfdec_t*	sfdec_new(sfdec_type_t type,
		    sfdec_codec_t codec,
		    sfdec_flags_t flags,
		    int *width, int *height, int rotation,
		    int64_t duration_us, int input_size,
		    void *surface_handle,
		    void *extradata, size_t extradata_size,
		    int *pts_reorder);

void		sfdec_delete(sfdec_t *sfdec);
int		sfdec_start(sfdec_t *);
int		sfdec_stop(sfdec_t *);
ssize_t		sfdec_send_input(sfdec_t *sfdec, void *data, size_t size, int64_t time_us, int is_sync_frame, int wait);
int		sfdec_flush(sfdec_t *sfdec);
int		sfdec_stop_input(sfdec_t *sfdec);
int		sfdec_read(sfdec_t *sfdec, int64_t seek, sfdec_read_out_t *read_out);
int		sfdec_buf_render(sfdec_t *sfdec, sfbuf_t *sfbuf, int render);
int		sfdec_buf_release(sfdec_t *sfdec, sfbuf_t *sfbuf);

#if __cplusplus
}
#endif

#endif

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

#ifndef _CODEC_UTILS_H_
#define _CODEC_UTILS_H_

#include "av.h"

enum PIXFMT {
	PIXFMT_YUV420P,
	PIXFMT_YUV422P,
	PIXFMT_YUV420P10LE,
	PIXFMT_YUV444P,
	PIXFMT_NV12,
	PIXFMT_QCOM_NV12_TILED,
};

static inline int avimage2pixfmt(int avimage)
{
	switch (avimage) {
		case AV_IMAGE_YUV_422:
			return PIXFMT_YUV422P;
		case AV_IMAGE_NV12:
			return PIXFMT_NV12;
		case AV_IMAGE_QCOM_NV12_TILED:
			return PIXFMT_QCOM_NV12_TILED;
		case AV_IMAGE_YV12:
		default:
			return PIXFMT_YUV420P;
	}
}

int color_conversion_supported(int colorspace, int pixfmt);
void codec_convert_pixel_format( int pixfmt, unsigned char *src_data[], int src_linesize[], int width, int height, VIDEO_FRAME *frame );

void *codec_convert_mt_init( int work_num );
void  codec_convert_mt     ( void *ctx, int pixfmt, unsigned char *data[], int linesize[], int width, int height, VIDEO_FRAME *frame );
int   codec_convert_mt_exit( void *ctx );

#endif

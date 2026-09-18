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

/* Standard inputs retain their AVPixelFormat value, including endianness. */
#include <libavutil/pixfmt.h>

enum PIXFMT {
	PIXFMT_YUV420P = AV_PIX_FMT_YUV420P,
	PIXFMT_YUV422P = AV_PIX_FMT_YUV422P,
	PIXFMT_YUV420P10LE = AV_PIX_FMT_YUV420P10LE,
	PIXFMT_YUV444P = AV_PIX_FMT_YUV444P,
	PIXFMT_NV12 = AV_PIX_FMT_NV12,
	PIXFMT_P010 = AV_PIX_FMT_P010LE,
	PIXFMT_QCOM_NV12_TILED = -2,
	PIXFMT_YV12 = -3, /* AVOS plane order: Y, V, U */
};

static inline int avimage2pixfmt(int avimage)
{
	switch (avimage) {
	case AV_IMAGE_YUV_422: return AV_PIX_FMT_UYVY422;
	case AV_IMAGE_NV12: return PIXFMT_NV12;
	case AV_IMAGE_QCOM_NV12_TILED: return PIXFMT_QCOM_NV12_TILED;
	case AV_IMAGE_YV12: return PIXFMT_YV12;
	default: return AV_PIX_FMT_NONE;
	}
}

int codec_frame_can_hold(const VIDEO_FRAME *frame, int width, int height);
int color_conversion_supported(int colorspace, int pixfmt);
int codec_convert_pixel_format( int pixfmt, unsigned char *src_data[], int src_linesize[], int width, int height, VIDEO_FRAME *frame );

/* Conversion returns 0 on success, a negative AVERROR on failure.
 * Source planes must cover the visible dimensions; strides are bytes and may
 * be negative. Destination RGB linestep remains in pixels for AVOS callers.
 * The context owns reusable padded buffers and a scaler. Calls on a context
 * are serialized internally; exit must follow completion of all calls. The mt
 * names are retained for callers; threading is now owned by libswscale.
 */
void *codec_convert_mt_init( int work_num );
int   codec_convert_mt     ( void *ctx, int pixfmt, unsigned char *data[], int linesize[], int width, int height, VIDEO_FRAME *frame );
int   codec_convert_mt_exit( void *ctx );

#endif

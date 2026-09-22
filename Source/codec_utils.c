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

#include "codec_utils.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#if defined(__APPLE__)
#define AVOS_WEAK_IMPORT __attribute__((weak_import))
#else
#define AVOS_WEAK_IMPORT __attribute__((weak))
#endif

extern void RenderX(unsigned char *, unsigned char *, int, int, int, int) AVOS_WEAK_IMPORT;

typedef struct convert {
	pthread_mutex_t mutex;
	struct SwsContext *sws;
	AVFrame *input;
	AVFrame *output;
	AVFrame *deinterlaced;
	int threads;
	int width, height, src_format, dst_format;
} convert_t;

static enum AVPixelFormat output_format(int colorspace)
{
	switch (colorspace) {
	case AV_IMAGE_BGRA_32: return AV_PIX_FMT_BGRA;
	case AV_IMAGE_RGBX_32: return AV_PIX_FMT_RGBA; /* opaque X byte */
	case AV_IMAGE_YUV_422: return AV_PIX_FMT_UYVY422;
	case AV_IMAGE_YV12: return AV_PIX_FMT_YUV420P;
	case AV_IMAGE_NV12: return AV_PIX_FMT_NV12;
	default: return AV_PIX_FMT_NONE;
	}
}

static enum AVPixelFormat input_format(int pixfmt)
{
	if (pixfmt == PIXFMT_QCOM_NV12_TILED)
		return AV_PIX_FMT_NV12;
	if (pixfmt == PIXFMT_YV12)
		return AV_PIX_FMT_YUV420P;
	return pixfmt;
}

/* Visible bytes/rows, independent of allocation and SIMD alignment. */
static int plane_layout(enum AVPixelFormat fmt, int width, int height,
	int bytes[4], int rows[4])
{
	ptrdiff_t strides[4];
	size_t sizes[4];
	const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
	if (!desc || (desc->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BITSTREAM)) ||
	    av_image_check_size(width, height, 0, NULL) < 0 ||
	    av_image_fill_linesizes(bytes, fmt, width) < 0)
		return AVERROR(EINVAL);
	for (int i = 0; i < 4; i++)
		strides[i] = bytes[i];
	if (av_image_fill_plane_sizes(sizes, fmt, height, strides) < 0)
		return AVERROR(EINVAL);
	if (desc->flags & AV_PIX_FMT_FLAG_PAL)
		bytes[1] = 1024;
	for (int i = 0; i < 4; i++)
		rows[i] = bytes[i] ? sizes[i] / bytes[i] : 0;
	return 0;
}

int color_conversion_supported(int colorspace, int pixfmt)
{
	enum AVPixelFormat src = input_format(pixfmt);
	enum AVPixelFormat dst = output_format(colorspace);
	return src != AV_PIX_FMT_NONE && dst != AV_PIX_FMT_NONE &&
	       sws_isSupportedInput(src) && sws_isSupportedOutput(dst);
}

/* AVOS RGB strides are pixels; all other destination strides are bytes. */
static int destination_layout(const VIDEO_FRAME *frame, uint8_t *data[4], int strides[4])
{
	if (!frame)
		return AVERROR(EINVAL);
	for (int i = 0; i < 3; i++) {
		data[i] = frame->data[i];
		strides[i] = frame->linestep[i];
	}
	data[3] = NULL;
	strides[3] = 0;
	if (frame->colorspace == AV_IMAGE_BGRA_32 || frame->colorspace == AV_IMAGE_RGBX_32) {
		if (strides[0] <= 0 || strides[0] > INT_MAX / 4)
			return AVERROR(EINVAL);
		strides[0] *= 4;
	} else if (frame->colorspace == AV_IMAGE_YV12) {
		data[1] = frame->data[2];
		data[2] = frame->data[1];
		strides[1] = frame->linestep[2];
		strides[2] = frame->linestep[1];
	}
	return 0;
}

int codec_frame_can_hold(const VIDEO_FRAME *frame, int width, int height)
{
	int bytes[4], rows[4], strides[4];
	uint8_t *data[4];
	if (!frame || width <= 0 || height <= 0 ||
	    plane_layout(output_format(frame->colorspace), width, height, bytes, rows) < 0 ||
	    destination_layout(frame, data, strides) < 0)
		return 0;
	for (int i = 0; i < 4; i++) {
		int plane = frame->colorspace == AV_IMAGE_YV12 && i > 0 && i < 3 ? 3 - i : i;
		if (rows[i] && (i >= 3 || !data[i] || strides[i] < bytes[i] ||
		    (int64_t)(rows[i] - 1) * strides[i] + bytes[i] > frame->data_size[plane]))
			return 0;
	}
	return 1;
}

static int prepare_frame(AVFrame **frame, enum AVPixelFormat fmt, int width, int height)
{
	if (*frame && (*frame)->format == fmt && (*frame)->width == width && (*frame)->height == height)
		return 0;
	av_frame_free(frame);
	*frame = av_frame_alloc();
	if (!*frame)
		return AVERROR(ENOMEM);
	(*frame)->format = fmt;
	(*frame)->width = width;
	(*frame)->height = height;
	int ret = av_frame_get_buffer(*frame, 32);
	if (ret < 0)
		av_frame_free(frame);
	return ret;
}

/* Qualcomm 64x32 tiles, arranged in groups of four. */
static size_t tile_pos(size_t x, size_t y, size_t w, size_t h)
{
	size_t pos = x + (y & ~(size_t)1) * w;
	if (y & 1)
		pos += (x & ~(size_t)3) + 2;
	else if (!(h & 1) || y != h - 1)
		pos += (x + 2) & ~(size_t)3;
	return pos;
}

static void detile(const uint8_t *src, AVFrame *dst, int visible_width, int visible_height)
{
	size_t tile_w = ((visible_width + 63) / 64 + 1) & ~(size_t)1;
	size_t luma_h = (visible_height + 31) / 32;
	size_t luma_size = (tile_w * luma_h * 2048 + 8191) & ~(size_t)8191;
	for (int plane = 0; plane < 2; plane++) {
		int height = plane ? (visible_height + 1) / 2 : visible_height;
		int width = plane ? ((visible_width + 1) / 2) * 2 : visible_width;
		size_t tile_h = (height + 31) / 32;
		const uint8_t *base = src + (plane ? luma_size : 0);
		for (int y = 0; y < height; y++) {
			for (int x = 0; x < width; x += 64) {
				size_t offset = tile_pos(x / 64, y / 32, tile_w, tile_h) * 2048 + (y % 32) * 64;
				int count = width - x < 64 ? width - x : 64;
				memcpy(dst->data[plane] + (ptrdiff_t)y * dst->linesize[plane] + x, base + offset, count);
			}
		}
	}
}

static int copy_input(convert_t *c, int pixfmt, unsigned char *data[], int strides[], int width, int height, int padded_width, int padded_height)
{
	int bytes[4], rows[4];
	enum AVPixelFormat fmt = input_format(pixfmt);
	if (!data || !strides || !data[0] || plane_layout(fmt, width, height, bytes, rows) < 0)
		return AVERROR(EINVAL);
	if (pixfmt != PIXFMT_QCOM_NV12_TILED) {
		for (int i = 0; i < 4; i++) {
			int plane = pixfmt == PIXFMT_YV12 && i > 0 && i < 3 ? 3 - i : i;
			if (rows[i] && (!data[plane] ||
			    (!(av_pix_fmt_desc_get(fmt)->flags & AV_PIX_FMT_FLAG_PAL && i == 1) &&
			     llabs((long long)strides[plane]) < bytes[i])))
				return AVERROR(EINVAL);
		}
	}
	int ret = prepare_frame(&c->input, fmt, padded_width, padded_height);
	if (ret < 0)
		return ret;
	if (pixfmt == PIXFMT_QCOM_NV12_TILED)
		detile(data[0], c->input, width, height);
	/* Copy visible bytes, then replicate borders inside owned storage only.
	 * This also avoids partial-block bugs in unscaled conversion kernels. */
	int padded_bytes[4], padded_rows[4], steps[4], components[4];
	if (plane_layout(fmt, padded_width, padded_height, padded_bytes, padded_rows) < 0)
		return AVERROR(EINVAL);
	av_image_fill_max_pixsteps(steps, components, av_pix_fmt_desc_get(fmt));
	for (int i = 0; i < 4; i++) {
		int plane = pixfmt == PIXFMT_YV12 && i > 0 && i < 3 ? 3 - i : i;
		for (int y = 0; y < rows[i]; y++) {
			uint8_t *row = c->input->data[i] + (ptrdiff_t)y * c->input->linesize[i];
			if (pixfmt != PIXFMT_QCOM_NV12_TILED)
				memcpy(row, data[plane] + (ptrdiff_t)y * strides[plane], bytes[i]);
			for (int x = bytes[i]; x < padded_bytes[i] && steps[i]; x += steps[i])
				memcpy(row + x, row + x - steps[i], steps[i]);
		}
		for (int y = rows[i]; y < padded_rows[i]; y++)
			memcpy(c->input->data[i] + (ptrdiff_t)y * c->input->linesize[i],
			       c->input->data[i] + (ptrdiff_t)(rows[i] - 1) * c->input->linesize[i], padded_bytes[i]);
	}
	return 0;
}

static int deinterlace(convert_t *c, AVFrame **src)
{
	/* Retain the existing 8-bit planar deinterlacer as a separate stage. */
	if ((*src)->format != AV_PIX_FMT_YUV420P && (*src)->format != AV_PIX_FMT_YUVJ420P)
		return 0;
	/* The deinterlacer implementation lives in libdeinterlace, which is not
	 * linked in the standalone simulator: skip deinterlacing when absent. */
	if (!RenderX)
		return 0;
	int ret = prepare_frame(&c->deinterlaced, (*src)->format, (*src)->width, (*src)->height);
	if (ret < 0)
		return ret;
	for (int i = 0; i < 3; i++) {
		int width = i ? ((*src)->width + 1) / 2 : (*src)->width;
		int height = i ? ((*src)->height + 1) / 2 : (*src)->height;
		/* RenderX operates on pairs. Preserve an unmatched final row. */
		if (height >= 2)
			RenderX(c->deinterlaced->data[i], (*src)->data[i], width, height & ~1,
			        c->deinterlaced->linesize[i], (*src)->linesize[i]);
		if (height & 1)
			memcpy(c->deinterlaced->data[i] + (ptrdiff_t)(height - 1) * c->deinterlaced->linesize[i],
			       (*src)->data[i] + (ptrdiff_t)(height - 1) * (*src)->linesize[i], width);
	}
	*src = c->deinterlaced;
	return 0;
}

static int prepare_scaler(convert_t *c, enum AVPixelFormat src, enum AVPixelFormat dst, int width, int height)
{
	if (c->sws && c->src_format == src && c->dst_format == dst && c->width == width && c->height == height)
		return 0;
	sws_freeContext(c->sws);
	c->sws = sws_alloc_context();
	if (!c->sws)
		return AVERROR(ENOMEM);
	int ret;
#define SET_OPTION(name, value) do { \
	ret = av_opt_set_int(c->sws, name, value, 0); \
	if (ret < 0) goto fail; \
} while (0)
	SET_OPTION("srcw", width);
	SET_OPTION("srch", height);
	SET_OPTION("dstw", width);
	SET_OPTION("dsth", height);
	SET_OPTION("src_format", src);
	SET_OPTION("dst_format", dst);
	SET_OPTION("sws_flags", SWS_BILINEAR);
	SET_OPTION("threads", c->threads);
#undef SET_OPTION
	ret = sws_init_context(c->sws, NULL, NULL);
	if (ret < 0)
		goto fail;
	c->src_format = src;
	c->dst_format = dst;
	c->width = width;
	c->height = height;
	return 0;
fail:
	sws_freeContext(c->sws);
	c->sws = NULL;
	return ret;
}

static int matrix_coefficients(int colorspace)
{
	switch (colorspace) {
	case AVCOL_SPC_BT709: return SWS_CS_ITU709;
	case AVCOL_SPC_FCC: return SWS_CS_FCC;
	case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
	case AVCOL_SPC_BT2020_NCL: return SWS_CS_BT2020;
	case AVCOL_SPC_UNSPECIFIED:
	case AVCOL_SPC_RGB:
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
	default: return -1; /* Do not silently interpret e.g. constant-luminance 2020 as NCL. */
	}
}

static int convert(convert_t *c, int pixfmt, unsigned char *data[], int strides[], int width, int height, VIDEO_FRAME *frame)
{
	if (!codec_frame_can_hold(frame, width, height) || !color_conversion_supported(frame->colorspace, pixfmt))
		return AVERROR(EINVAL);
	enum AVPixelFormat dstfmt = output_format(frame->colorspace);
	/* Preserve visible dimensions at the boundary. SIMD-friendly working
	 * dimensions and replicated borders are private to this adapter. */
	int padded_width = (width + 31) & ~31;
	int padded_height = (height + 1) & ~1;
	int ret = copy_input(c, pixfmt, data, strides, width, height, padded_width, padded_height);
	if (ret < 0)
		return ret;
	AVFrame *src = c->input;
	if (frame->deinterlace && (ret = deinterlace(c, &src)) < 0)
		return ret;
	if ((ret = prepare_frame(&c->output, dstfmt, padded_width, padded_height)) < 0 ||
	    (ret = prepare_scaler(c, src->format, dstfmt, padded_width, padded_height)) < 0)
		return ret;
	int matrix = matrix_coefficients(frame->color_space);
	int src_rgb = av_pix_fmt_desc_get(src->format)->flags & AV_PIX_FMT_FLAG_RGB;
	int dst_rgb = av_pix_fmt_desc_get(dstfmt)->flags & AV_PIX_FMT_FLAG_RGB;
	if (matrix < 0) {
		if (!!src_rgb != !!dst_rgb)
			return AVERROR(ENOSYS);
		/* Repacking YUV does not change its matrix. */
		matrix = SWS_CS_DEFAULT;
	}
	int full = frame->color_range == AVCOL_RANGE_JPEG || src_rgb ||
		src->format == AV_PIX_FMT_YUVJ420P || src->format == AV_PIX_FMT_YUVJ422P ||
		src->format == AV_PIX_FMT_YUVJ444P || src->format == AV_PIX_FMT_YUVJ440P || src->format == AV_PIX_FMT_YUVJ411P;
	const int *coeffs = sws_getCoefficients(matrix);
	ret = sws_setColorspaceDetails(c->sws, coeffs, full, coeffs, dst_rgb ? 1 : full, 0, 1 << 16, 1 << 16);
	if (ret < 0)
		return ret;
	ret = sws_scale(c->sws, (const uint8_t *const *)src->data, src->linesize, 0, padded_height,
	                c->output->data, c->output->linesize);
	if (ret != padded_height)
		return ret < 0 ? ret : AVERROR(EIO);
	uint8_t *dst[4];
	int dst_stride[4];
	if ((ret = destination_layout(frame, dst, dst_stride)) < 0)
		return ret;
	/* Never expose SIMD writes to tightly sized Android/window buffers. */
	av_image_copy(dst, dst_stride, (const uint8_t **)c->output->data, c->output->linesize, dstfmt, width, height);
	frame->color_range = dst_rgb || full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
	return 0;
}

void *codec_convert_mt_init(int work_num)
{
	if (work_num < 0 || work_num > 8)
		return NULL;
	convert_t *c = calloc(1, sizeof(*c));
	if (c) {
		if (pthread_mutex_init(&c->mutex, NULL)) {
			free(c);
			return NULL;
		}
		c->threads = work_num ? work_num : 1;
	}
	return c;
}

int codec_convert_mt_exit(void *ctx)
{
	convert_t *c = ctx;
	if (c) {
		sws_freeContext(c->sws);
		av_frame_free(&c->input);
		av_frame_free(&c->output);
		av_frame_free(&c->deinterlaced);
		pthread_mutex_destroy(&c->mutex);
		free(c);
	}
	return 0;
}

int codec_convert_mt(void *ctx, int pixfmt, unsigned char *data[], int strides[], int width, int height, VIDEO_FRAME *frame)
{
	if (!ctx)
		return codec_convert_pixel_format(pixfmt, data, strides, width, height, frame);
	convert_t *c = ctx;
	pthread_mutex_lock(&c->mutex);
	int ret = convert(c, pixfmt, data, strides, width, height, frame);
	pthread_mutex_unlock(&c->mutex);
	if (ret < 0 && frame)
		frame->error = 1;
	return ret;
}

int codec_convert_pixel_format(int pixfmt, unsigned char *data[], int strides[], int width, int height, VIDEO_FRAME *frame)
{
	void *ctx = codec_convert_mt_init(1);
	if (!ctx) {
		if (frame) frame->error = 1;
		return AVERROR(ENOMEM);
	}
	int ret = codec_convert_mt(ctx, pixfmt, data, strides, width, height, frame);
	codec_convert_mt_exit(ctx);
	return ret;
}

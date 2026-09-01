/*
 * Copyright (C) 2026 The Nova Video Player Project
 *
 * Dolby Vision tone-map mode, hardware path (S5).
 *
 * MediaCodec decodes the HEVC base layer into an AImageReader-backed
 * surface; each output frame is exported as an AHardwareBuffer descriptor
 * and rendered by dovi_gl through the mpv hwdec_aimagereader import chain
 * (AHardwareBuffer -> eglGetNativeClientBufferANDROID -> EGLImage ->
 * GL_TEXTURE_EXTERNAL_OES -> libplacebo).
 *
 * The Dolby Vision RPU (HEVC NAL type 62) is extracted from every input
 * access unit before MediaCodec sees it (the DV NALs are stripped) and is
 * paired with the decoded frame by timestamp; the renderer parses it with
 * libdovi (pl_hdr_metadata_from_dovi_rpu) so the dynamic L1 trim metadata
 * drives the HDR10 tone-map.
 *
 * Scope: profiles whose base layer is HDR10-compatible (profile 8.x, and
 * profile 7 without an enhancement layer). FEL content (profile 7 with
 * EL) needs YUV-domain reshaping/NLQ composition, which the OES import
 * cannot provide (the GPU sampler has already converted to RGB); such
 * files fail open() here and fall back to the software decoder, which
 * implements the full mpv-parity pipeline.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "debug.h"
#include "av.h"
#include "stream.h"
#include "stream_dec_video.h"
#include "stream_config.h"

#if defined( CONFIG_ANDROID ) && defined( CONFIG_DOVI_TONEMAP )

#include <unistd.h>
#include <dlfcn.h>
#include <android/api-level.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <libavutil/mem.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavcodec/avcodec.h>

#include "dovi_nal.h"
#include "dovi_rpu_meta.h"
#include "dovi_gl.h"

/* NdkMediaImageReader.h is not exposed at APP_PLATFORM 21; the API is
 * dlopen/dlsym'd at runtime (API 26+), so only the opaque types and the
 * format constant are needed here. */
typedef struct AImageReader AImageReader;
typedef struct AImage AImage;
#ifndef AIMAGE_FORMAT_PRIVATE
#define AIMAGE_FORMAT_PRIVATE 0x22
#endif

/* MediaCodec color formats for the byte-buffer (copy) EL/BL decode path —
 * mirrors the values FFmpeg's mediacodecdec_common.c handles */
#define DVHW_COLOR_FormatYUV420Planar            19
#define DVHW_COLOR_FormatYUV420SemiPlanar         21
#define DVHW_COLOR_FormatYUVP010                   0x36
#define DVHW_COLOR_QCOM_FormatYUV420SemiPlanar32m 0x7FA30C04

/* libavos.c: user preference (0 = passthrough) */
extern int libavos_get_dolby_vision_mode(void);
/* stream_sink_video_dovi.c */
extern STREAM_SINK_VIDEO *stream_sink_video_dovi_new(void *surface);

#define TAG "DOVI_HW"

#define DVHW_PENDING_MAX 64
#define DVHW_INPUT_TIMEOUT_US 100000
#define DVHW_EL_INPUT_TIMEOUT_US 0	/* EL feed must never block: its input
					 * queue fills while the BL paces the engine; outputs are
					 * drained non-blocking every call */
#define DVHW_EL_CATCHUP 4		/* EL reorder depth: the EL decoder emits
					 * an AU a few AUs after it is fed, so the
					 * fed-vs-drained budget may run this many
					 * AUs ahead of the 1:1 BL pace */

/* AImage entry points (API 26+), resolved at runtime so libavos.so still
 * loads on older devices. Only AImage_delete stays live (dvhw_frame_release
 * frees stale recycled-frame images from the pre-buffer-mode era of the
 * pool); the former AImageReader surface pipeline was removed with the
 * OES path - the BL now decodes in MediaCodec buffer mode like mpv's
 * mediacodec-copy. */
typedef void (*PFN_AImage_delete)(AImage *image);

static struct {
	void *lib_mediandk;
	PFN_AImage_delete                  imageDelete;
	int loaded;
} dvhw_api;

static int dvhw_api_load(void)
{
	if (dvhw_api.loaded)
		return 0;

	dvhw_api.lib_mediandk = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);
	if (!dvhw_api.lib_mediandk) {
		serprintf(TAG ": cannot dlopen libmediandk.so\n");
		return 1;
	}

	dvhw_api.imageDelete = (PFN_AImage_delete)
		dlsym(dvhw_api.lib_mediandk, "AImage_delete");

	if (!dvhw_api.imageDelete) {
		serprintf(TAG ": AImage entry points incomplete\n");
		dlclose(dvhw_api.lib_mediandk);
		dvhw_api.lib_mediandk = NULL;
		return 1;
	}
	dvhw_api.loaded = 1;
	return 0;
}

// RPU blobs waiting for their decoded frame, keyed by MediaCodec timestamp
typedef struct {
	int	used;
	int64_t	pts_us;
	uint8_t	*rpu;
	int	rpu_size;
} dvhw_pending;

typedef struct {
	AMediaCodec	*codec;
	int	width, height;
	int	nal_length_size;
	dvhw_pending	pending[DVHW_PENDING_MAX];
	int	pending_count;
	/* --- FEL: hardware BL+EL byte-buffer decode (mpv mediacodec-copy shape).
	 * When the file has an enhancement layer, BOTH layers decode through
	 * MediaCodec in buffer mode (no surface): libplacebo's FEL composition
	 * requires the BL in YUV domain with the DV repr (renderer.c gates on
	 * PL_COLOR_SYSTEM_DOLBYVISION + nlq_active), which an OES/RGB import
	 * cannot provide. One YUV420 copy per frame per layer — the same cost
	 * mpv's mediacodec-copy path pays — versus the multi-hundred-ms SW
	 * 4K HEVC decode. */
	int	fel;			/* 1: BL+EL buffer-mode decode active */
	int	buffer_mode;		/* 1: BL byte-buffer output (copy pipeline). Set for
				 * FEL AND for P8.x/P7-BL-only: the YUV-copy path is
				 * the glitch-free one (see dvhw_open). */
	AMediaCodec	*el_codec;
	int	el_width, el_height;
	int	el_stride, el_slice_height;
	int	el_color_format;
	int	el_fed_count;		/* EL AUs fed, throttled against BL AUs consumed */
	int	el_drain_count;		/* EL frames emitted by the decoder (reorder lag tracking) */
	int	bl_fed_count;		/* BL AUs queued to the BL codec (EL feed pacing master) */
	int	el_seen;		/* EL emitted >=1 frame since last flush (warm-up gate, mpv 3b4caf0) */
	int	el_exhausted;		/* parser has no more EL packets (EOF tail evidence,
				 * probed in dvhw_el_feed; cleared on flush) */
	AVFrame		*el_q[DVHW_PENDING_MAX];
	int	el_q_count;
	int	el_nal_length_size;
	int	el_params_sent;	/* 1: in-band VPS/SPS/PPS prepended once */
	/* BL buffer-mode decode (FEL only) */
	int	stride, slice_height, color_format;
	AVFrame		*bl_q[4];
	int	bl_q_count;
} PRIV;

static void dvhw_pending_clear(PRIV *p)
{
	int i;
	for (i = 0; i < DVHW_PENDING_MAX; i++) {
		if (p->pending[i].used) {
			av_free(p->pending[i].rpu);
			p->pending[i].used = 0;
			p->pending[i].rpu = NULL;
			p->pending[i].rpu_size = 0;
		}
	}
	p->pending_count = 0;
}

static void dvhw_pending_push(PRIV *p, int64_t pts_us, uint8_t *rpu, int rpu_size)
{
	int i, oldest = -1;
	for (i = 0; i < DVHW_PENDING_MAX; i++) {
		if (!p->pending[i].used)
			break;
		if (oldest < 0 || p->pending[i].pts_us < p->pending[oldest].pts_us)
			oldest = i;
	}
	if (i == DVHW_PENDING_MAX) {
		// full: drop the oldest (stale) entry
		i = oldest;
		av_free(p->pending[i].rpu);
		p->pending[i].used = 0;
		p->pending_count--;
	}
	p->pending[i].used = 1;
	p->pending[i].pts_us = pts_us;
	p->pending[i].rpu = rpu;
	p->pending[i].rpu_size = rpu_size;
	p->pending_count++;
}

// take the RPU matching pts_us (exact match first, else oldest <= pts_us)
static void dvhw_pending_take(PRIV *p, int64_t pts_us, uint8_t **rpu, int *rpu_size)
{
	int i, best = -1;
	*rpu = NULL;
	*rpu_size = 0;
	for (i = 0; i < DVHW_PENDING_MAX; i++) {
		if (!p->pending[i].used)
			continue;
		if (p->pending[i].pts_us == pts_us) {
			best = i;
			break;
		}
		if (p->pending[i].pts_us <= pts_us &&
		    (best < 0 || p->pending[i].pts_us > p->pending[best].pts_us))
			best = i;
	}
	if (best < 0)
		return;
	*rpu = p->pending[best].rpu;
	*rpu_size = p->pending[best].rpu_size;
	p->pending[best].used = 0;
	p->pending[best].rpu = NULL;
	p->pending_count--;
}

// renderer-side cleanup, invoked by the DV sink after rendering
static void dvhw_frame_release(dovi_hw_frame *f)
{
	if (!f)
		return;
	/* the AHardwareBuffer is owned by the AImage (mpv hwdec_aimagereader
	 * does the same): AImage_delete releases it, an explicit ahbRelease
	 * would underflow the refcount and corrupt the reader's BufferQueue */
	if (f->image && dvhw_api.imageDelete)
		dvhw_api.imageDelete((AImage *) f->image);
	av_free(f->rpu);
	afree(f);
}

static void dvhw_frame_release(dovi_hw_frame *f);

static void dvhw_el_q_clear(PRIV *p);
static void dvhw_bl_q_clear(PRIV *p);

static int dvhw_open(STREAM_DEC_VIDEO *dec, VIDEO_PROPERTIES *video, void *ctx,
                     int *pneed_flush, int *pneed_reorder)
{
	PRIV *p = (PRIV *) dec->priv;
	AMediaFormat *fmt = NULL;
	media_status_t st;

	if (libavos_get_dolby_vision_mode() == 0)
		return 1;		// passthrough mode: sfdec2 owns DV
	if (video->format != VIDEO_FORMAT_DOLBY_VISION)
		return 1;
	if (android_get_device_api_level() < 26) {
		serprintf(TAG ": AHardwareBuffer export needs API 26, using software path\n");
		return 1;
	}
	if (video->dv_profile_source != 7 && video->dv_profile_source != 8)
		return 1;
	if (dvhw_api_load())
		return 1;

	// FEL (profile 7 with EL): libplacebo composes the enhancement layer
	// only from a YUV-domain BL with the DV repr (PL_COLOR_SYSTEM_DOLBYVISION
	// + nlq_active, renderer.c sample_el gate). The OES import is RGB, so for
	// FEL we decode BOTH layers through MediaCodec byte-buffer mode (mpv
	// mediacodec-copy shape) and hand YUV AVFrames to the same
	// dovi_gl_render(bl, el) call the software path uses. Without an EL
	// config (P8.x, P7 BL-only) the zero-copy OES surface path stays.
	memset(p, 0, sizeof(*p));
	p->fel = (video->dv_profile_source == 7 &&
	          video->dv_el_extraData && video->dv_el_extraDataSize > 0) ? 1 : 0;
	/* P8.x (and P7 BL-only) use the same BUFFER-MODE pipeline as FEL, minus
	 * the EL codec: MediaCodec hands YUV420 byte buffers, dvhw_copy_yuv420
	 * copies them to AVFrames and the RPU side datas (DOVI_METADATA + raw
	 * RPU) are attached for pl_map_avframe_ex(map_dovi) in dovi_gl_render -
	 * identical to the proven FEL BL path. The former zero-copy OES
	 * surface path (AImageReader + EGLImage import) showed deterministic
	 * one-frame corruption (colored squares) on SMB Cape Fear P8.1 at
	 * fixed content positions - playing fine locally and fine on desktop
	 * mpv - pointing at the Samsung codec2 GPU-render-to-reader buffer
	 * pipeline being fed in a way this device tolerates only when the
	 * queue stays shallow. The copy pipeline costs one 4K YUV420 copy per
	 * frame (~2ms CPU) and reuses the same bl_q/emit machinery FEL uses. */
	p->buffer_mode = 1;
	p->width = video->width;
	p->height = video->height;
	p->nal_length_size = dovi_hvcc_nal_length_size(video->extraData,
	                                               video->extraDataSize);

	if (p->fel) {
		serprintf(TAG ": profile 7 FEL: hardware BL+EL buffer-mode decode\n");
		// BL codec in BUFFER mode: no surface, YUV420 byte buffers out.
		// Same configure as below otherwise.
		p->codec = AMediaCodec_createDecoderByType("video/hevc");
		if (!p->codec) {
			serprintf(TAG ": no video/hevc decoder available\n");
			goto fail;
		}
		fmt = AMediaFormat_new();
		if (!fmt)
			goto fail;
		AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/hevc");
		AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, video->width);
		AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, video->height);
		/* Request 10-bit output (P010) for Main10 BL: c2.qti.hevc.decoder
		 * advertises YUVP010 (0x36) in byte-buffer mode. Without this the
		 * codec may hand back 8-bit NV12, and an 8-bit BL fed through the
		 * 10-bit-authored DV reshaping polynomial renders wrong (green).
		 * Verified against dumpsys media.player color list. */
		AMediaFormat_setInt32(fmt, "color-format", 0x36 /* YUVP010 */);
		if (video->extraDataSize > 0)
			AMediaFormat_setBuffer(fmt, "csd-0",
			                       video->extraData, video->extraDataSize);
		if (AMediaCodec_configure(p->codec, fmt, NULL, NULL, 0) != 0) {
			serprintf(TAG ": AMediaCodec_configure failed (BL buffer mode)\n");
			goto fail;
		}
		if (AMediaCodec_start(p->codec) != 0) {
			serprintf(TAG ": AMediaCodec_start failed (BL)\n");
			goto fail;
		}

		// EL codec: separate 1080p session in buffer mode, EL hvcC config.
		// The EL hvcC comes from dv_el_extraData: the hvcE BlockAddition
		// mapping config (interleaved) or the EL track CodecPrivate
		// (dual-track), same as the software path uses.
		p->el_nal_length_size = dovi_hvcc_nal_length_size(
			video->dv_el_extraData, video->dv_el_extraDataSize);
		p->el_codec = AMediaCodec_createDecoderByType("video/hevc");
		if (!p->el_codec) {
			serprintf(TAG ": no second video/hevc decoder for EL\n");
			goto fail;
		}
		AMediaFormat_delete(fmt);
		fmt = AMediaFormat_new();
		if (!fmt)
			goto fail;
		AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/hevc");
		/* the EL dimensions (1920x1080 for a spatial-substream FEL) come
		 * from the codec's output-format event; seed with the BL size as a
		 * hint (MediaCodec re-negotiates from the hvcC/SPS anyway) */
		p->el_width  = video->width;
		p->el_height = video->height;
		AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, p->el_width);
		AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, p->el_height);
		/* Main10 EL: request P010 (same as the BL) */
		AMediaFormat_setInt32(fmt, "color-format", 0x36 /* YUVP010 */);
		AMediaFormat_setBuffer(fmt, "csd-0",
		                       video->dv_el_extraData, video->dv_el_extraDataSize);
		if (AMediaCodec_configure(p->el_codec, fmt, NULL, NULL, 0) != 0) {
			serprintf(TAG ": AMediaCodec_configure failed (EL)\n");
			goto fail;
		}
		if (AMediaCodec_start(p->el_codec) != 0) {
			serprintf(TAG ": AMediaCodec_start failed (EL)\n");
			goto fail;
		}
		AMediaFormat_delete(fmt);
		fmt = NULL;
	} else {
	// --- P8.x / P7 BL-only: same BL BUFFER-mode configure as FEL, no EL ---
	serprintf(TAG ": profile %d: hardware BL buffer-mode decode (copy pipeline)\n",
		          video->dv_profile_source);
	p->codec = AMediaCodec_createDecoderByType("video/hevc");
	if (!p->codec) {
		serprintf(TAG ": no video/hevc decoder available\n");
		goto fail;
	}
	fmt = AMediaFormat_new();
	if (!fmt)
		goto fail;
	AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/hevc");
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, video->width);
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, video->height);
	/* Request 10-bit output (P010) for Main10: c2.qti.hevc.decoder
	 * advertises YUVP010 (0x36) in byte-buffer mode. Without this the
	 * codec may hand back 8-bit NV12, and an 8-bit BL fed through the
	 * 10-bit-authored DV reshaping polynomial renders wrong (green).
	 * Same key the FEL BL configure uses. */
	AMediaFormat_setInt32(fmt, "color-format", 0x36 /* YUVP010 */);
	if (video->extraDataSize > 0)
		AMediaFormat_setBuffer(fmt, "csd-0",
		                       video->extraData, video->extraDataSize);
	if (AMediaCodec_configure(p->codec, fmt, NULL, NULL, 0) != 0) {
		serprintf(TAG ": AMediaCodec_configure failed (BL buffer mode)\n");
		goto fail;
	}
	AMediaFormat_delete(fmt);
	fmt = NULL;
	if (AMediaCodec_start(p->codec) != 0) {
		serprintf(TAG ": AMediaCodec_start failed (BL)\n");
		goto fail;
	}
	}	// end BL buffer-mode (non-FEL) path

	dec->ctx = ctx;
	dec->video = &dec->_video;
	memcpy(dec->video, video, sizeof(VIDEO_PROPERTIES));
	dec->is_open = 1;
	if (pneed_flush)
		*pneed_flush = 1;
	if (pneed_reorder)
		*pneed_reorder = 0;	// MediaCodec outputs in display order
	serprintf(TAG ": MediaCodec HEVC decode for DV tone-map, %dx%d nal_length_size %d\n",
	          video->width, video->height, p->nal_length_size);
	return 0;

fail:
	if (fmt)
		AMediaFormat_delete(fmt);
	if (p->codec) {
		AMediaCodec_delete(p->codec);
		p->codec = NULL;
	}
	if (p->el_codec) {
		AMediaCodec_delete(p->el_codec);
		p->el_codec = NULL;
	}
	dvhw_el_q_clear(p);
	dvhw_bl_q_clear(p);
	return 1;
}

// ************************************************************
//
//	FEL byte-buffer helpers: frame queues, YUV420 copy from MediaCodec
//
// ************************************************************
static void dvhw_el_q_clear(PRIV *p)
{
	while (p->el_q_count > 0)
		av_frame_free(&p->el_q[--p->el_q_count]);
	/* seek/flush: EL decoder state restarts, drop the pace accounting
	 * and the warm-up flag (mpv pair_reset) */
	p->el_fed_count = 0;
	p->el_drain_count = 0;
	p->bl_fed_count = 0;
	p->el_seen = 0;
}

static void dvhw_bl_q_clear(PRIV *p)
{
	while (p->bl_q_count > 0)
		av_frame_free(&p->bl_q[--p->bl_q_count]);
}

/* copy one MediaCodec YUV420 byte buffer (index already dequeued) into an
 * AVFrame — FFmpeg mediacodec_sw_buffer_copy_yuv420_* parity: stride and
 * slice-height aware, planar and semiplanar layouts. Releases the codec
 * buffer after the copy. pts lands in the avos ms domain. */
static AVFrame *dvhw_copy_yuv420(AMediaCodec *codec, ssize_t index,
                                 AMediaCodecBufferInfo *info,
                                 int width, int height,
                                 int stride, int slice_height,
                                 int color_format, int crop_right)
{
	AVFrame *f = NULL;
	size_t bsize = 0;
	uint8_t *src = AMediaCodec_getOutputBuffer(codec, (size_t) index, &bsize);
	int y, x;

	if (!src)
		goto done;	/* release the dequeued output slot (an early return here
			 * would permanently lose one of the codec's fixed output
			 * buffers and wedge output after N failures) */
	if (stride <= 0)
		stride = width;
	if (slice_height <= 0)
		slice_height = height;
	/* crop_right: MediaCodec slices may be padded to a stride multiple */
	(void) crop_right;

	f = av_frame_alloc();
	if (!f)
		goto done;
	/* P010 (color 0x36): 16-bit samples, 10-bit code MSB-aligned. The
	 * frame tag is set to AV_PIX_FMT_P010LE below (semiplanar, shift 6)
	 * so libplacebo reads code/1023 - the domain the DV RPU curve and
	 * matrices were authored for (see the p010 copy branch). */
	int p010 = (color_format == DVHW_COLOR_FormatYUVP010);
	f->format = p010 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_YUV420P;
	f->width = width;
	f->height = height;
	if (av_frame_get_buffer(f, 32) < 0) {
		av_frame_free(&f);
		f = NULL;
		goto done;
	}
	f->pts = info->presentationTimeUs / 1000;

	if (p010) {
		/* MediaCodec P010: 16-bit samples with the 10-bit code in the MSBs
		 * (code << 6), NV12-style semiplanar layout (Y, then interleaved
		 * UV), stride in BYTES. Keep the layout EXACTLY and tag the frame
		 * AV_PIX_FMT_P010LE: its pixdesc (comp shift 6, depth 10) makes
		 * libplacebo's pl_plane_data_align produce
		 *   bits = { sample_depth 16, color_depth 10, bit_shift 6 }
		 * so pl_color_repr_normalize nets out to 1.0 and the shader reads
		 * code/1023 - the domain the DV reshape pivots/matrix and the
		 * RPU nonlinear matrix were authored for.
		 * (Tagging the deinterleaved planes YUV420P10LE was wrong: that
		 * descriptor says shift 0, so libplacebo scaled by 64, every
		 * signal above 1/64 passed the PQ EOTF pole, went NaN and
		 * rendered black.) */
		const uint8_t *y_src = src + info->offset;
		const uint8_t *uv_src = y_src + (size_t) stride * slice_height;
		f->format = AV_PIX_FMT_P010LE;
		for (y = 0; y < height; y++)
			memcpy(f->data[0] + y * f->linesize[0],
			       y_src + y * stride, width * 2);
		for (y = 0; y < height / 2; y++)
			memcpy(f->data[1] + y * f->linesize[1],
			       uv_src + y * stride, width * 2);
	} else if (color_format == DVHW_COLOR_FormatYUV420Planar) {
		/* planar fallback: Y, then U, then V planes */
		const uint8_t *y_src = src + info->offset;
		const uint8_t *u_src = y_src + stride * slice_height;
		const uint8_t *v_src = u_src + (stride / 2) * (slice_height / 2);
		for (y = 0; y < height; y++)
			memcpy(f->data[0] + y * f->linesize[0],
			       y_src + y * stride, width);
		for (y = 0; y < height / 2; y++) {
			memcpy(f->data[1] + y * f->linesize[1],
			       u_src + y * (stride / 2), width / 2);
			memcpy(f->data[2] + y * f->linesize[2],
			       v_src + y * (stride / 2), width / 2);
		}
	} else {
		/* semiplanar: Y plane, then interleaved UV (NV12) */
		const uint8_t *y_src = src + info->offset;
		const uint8_t *uv_src = y_src + stride * slice_height;
		for (y = 0; y < height; y++)
			memcpy(f->data[0] + y * f->linesize[0],
			       y_src + y * stride, width);
		for (y = 0; y < height / 2; y++) {
			const uint8_t *uv = uv_src + y * stride;
			uint8_t *u = f->data[1] + y * f->linesize[1];
			uint8_t *v = f->data[2] + y * f->linesize[2];
			for (x = 0; x < width / 2; x++) {
				u[x] = uv[2 * x];
				v[x] = uv[2 * x + 1];
			}
		}
	}

done:
	if (index >= 0)
		AMediaCodec_releaseOutputBuffer(codec, (size_t) index, false);
	return f;
}

static int dvhw_close(STREAM_DEC_VIDEO *dec)
{
	PRIV *p = (PRIV *) dec->priv;

	if (!dec->is_open)
		return 0;
	if (p->codec) {
		AMediaCodec_stop(p->codec);
		AMediaCodec_delete(p->codec);
		p->codec = NULL;
	}
	if (p->el_codec) {
		AMediaCodec_stop(p->el_codec);
		AMediaCodec_delete(p->el_codec);
		p->el_codec = NULL;
	}
	dvhw_el_q_clear(p);
	dvhw_bl_q_clear(p);
	dvhw_pending_clear(p);
	dec->is_open = 0;
	return 0;
}

static int dvhw_prepare(STREAM_DEC_VIDEO *dec, VIDEO_FRAME **frames, int num_frames)
{
	return 0;
}

static int dvhw_cleanup(STREAM_DEC_VIDEO *dec, VIDEO_FRAME **frames, int num_frames)
{
	int i;
	for (i = 0; i < num_frames; i++) {
		VIDEO_FRAME *f = frames[i];
		if (f && f->dec == dec) {
			/* OES path: AImage-backed dovi_hw_frame */
			if (f->handle[0]) {
				dvhw_frame_release((dovi_hw_frame *) f->handle[0]);
				f->handle[0] = NULL;
			}
			/* FEL path: BL/EL AVFrames in priv/handle[1] (same ownership
			 * model as codec_ffmpeg_video's cleanup) */
			if (f->priv) {
				av_frame_free((AVFrame **) &f->priv);
			}
			if (f->handle[1]) {
				av_frame_free((AVFrame **) &f->handle[1]);
				f->handle[1] = NULL;
			}
		}
	}
	return 0;
}



// av_buffer_create free callback: frees the AVDOVIMetadata struct
static void dvhw_meta_buf_free(void *opaque, uint8_t *data)
{
	(void) data;
	av_free(opaque);
}

/* Convert an hvcC/hvcE extradata to an Annex-B byte stream of its
 * parameter-set NAL arrays (VPS/SPS/PPS), prepended with start codes.
 * Samsung codec2 buffer-mode decoders need the parameter sets in-band
 * (csd-0 alone does not always kick off EL decoding); this mirrors what
 * mkvextract writes when extracting an EL track (proven to decode).
 * Returns an av_malloc'd buffer (caller frees) or NULL. */
static uint8_t *dvhw_hvcc_params_annexb(const uint8_t *hvcc, int size,
                                        int *out_size)
{
	/* hvcC layout: 0..21 fixed, 22 = numOfArrays, then per array:
	 *   1 byte (completeness|NAL_type<<1), 2 bytes numNalus, then per
	 *   NAL: 2 bytes length + payload (no emulation needed on copy) */
	int pos, i, a, n;
	uint8_t *buf = NULL;
	size_t total = 0;
	uint8_t *dst;

	if (!hvcc || size < 23)
		return NULL;
	if (!(hvcc[22] & 0xFF)) { /* numOfArrays == 0 */
		*out_size = 0;
		return NULL;
	}
	pos = 23;
	/* pass 1: size */
	for (a = 0; a < hvcc[22]; a++) {
		if (pos + 3 > size)
			return NULL;
		int num = (hvcc[pos + 1] << 8) | hvcc[pos + 2];
		pos += 3;
		for (n = 0; n < num; n++) {
			if (pos + 2 > size)
				return NULL;
			int nal = (hvcc[pos] << 8) | hvcc[pos + 1];
			if (pos + 2 + nal > size)
				return NULL;	/* declared NAL longer than the buffer:
					 * malformed/corrupt extradata - reject instead of
					 * over-reading in pass 2's memcpy (FFmpeg's hvcC
					 * parser bounds-checks every NAL length) */
			pos += 2 + nal;
			total += 4 + (size_t) nal;
		}
	}
	buf = (uint8_t *) av_malloc(total + AV_INPUT_BUFFER_PADDING_SIZE);
	if (!buf)
		return NULL;
	dst = buf;
	pos = 23;
	for (a = 0; a < hvcc[22]; a++) {
		int num = (hvcc[pos + 1] << 8) | hvcc[pos + 2];
		pos += 3;
		for (n = 0; n < num; n++) {
			int nal = (hvcc[pos] << 8) | hvcc[pos + 1];
			pos += 2;
			*dst++ = 0; *dst++ = 0; *dst++ = 0; *dst++ = 1;
			memcpy(dst, hvcc + pos, (size_t) nal);
			dst += nal;
			pos += nal;
		}
	}
	(void) i;
	*out_size = (int) total;
	return buf;
}

/* Convert a length-prefixed HEVC access unit to Annex-B (start codes).
 * Data already in Annex-B (lsize==0) passes through as a copy. */
static uint8_t *dvhw_au_to_annexb(const uint8_t *data, int size, int lsize,
                                  int *out_size)
{
	int prefix = lsize ? lsize : 4;
	size_t total = 0;
	int pos = 0, nal_size, pass;
	const uint8_t *nal;
	uint8_t *buf = NULL, *dst;

	for (pass = 0; pass < 2; pass++) {
		pos = 0;
		dst = buf;
		while ((nal = dovi_next_nal(data, size, lsize, &pos, &nal_size)) != NULL) {
			if (pass == 0) {
				total += 4 + (size_t) nal_size;
			} else {
				*dst++ = 0; *dst++ = 0; *dst++ = 0; *dst++ = 1;
				memcpy(dst, nal, (size_t) nal_size);
				dst += nal_size;
			}
		}
		if (pass == 0) {
			if (!total) {
				*out_size = 0;
				return NULL;
			}
			buf = (uint8_t *) av_malloc(total + AV_INPUT_BUFFER_PADDING_SIZE);
			if (!buf)
				return NULL;
		}
	}
	(void) prefix;
	*out_size = (int) total;
	return buf;
}

/* take the EL frame whose pts matches (exact match). Unlike the software
 * path there is no stale-drop here: the EL queue may legitimately lag
 * (async MediaCodec), and dvhw_fel_emit holds BL frames until their EL
 * arrives or is proven absent. */
static AVFrame *dvhw_el_take_pair(PRIV *p, int64_t bl_pts_ms)
{
	int i;
	AVFrame *el = NULL;
	for (i = 0; i < p->el_q_count; i++) {
		if (p->el_q[i]->pts == bl_pts_ms) {
			el = p->el_q[i];
			memmove(&p->el_q[i], &p->el_q[i + 1],
			        (p->el_q_count - i - 1) * sizeof(p->el_q[0]));
			p->el_q_count--;
			break;
		}
	}
	return el;
}
static void dvhw_el_feed(STREAM_DEC_VIDEO *dec)
{
	PRIV *p = (PRIV *) dec->priv;
	STREAM *s = (STREAM *) dec->ctx;
	AVPacket el_pkt;

	if (!p->el_codec || !s || !s->parser || !s->parser->get_dovi_el_packet)
		return;
	/* mpv f_enhancement_pair parity: the EL decoder is fed at the same
	 * rate the BL AUs are consumed (plus a small window for the EL's
	 * reorder lag). Unthrottled feeding lets the EL decoder race seconds
	 * ahead of the BL output position on locally-buffered files; the
	 * 64-deep el_q then overflows and drops exactly the ELs the upcoming
	 * BLs need, and every frame emits BL-only (measured: elq0 = bl+918ms,
	 * elq=64, 5/8 BL-only) — FEL metadata without the residual renders
	 * green/garbage. The EL must be AHEAD of the BL output, not behind:
	 * the BL emits its Nth frame only after its reorder window is fed, so
	 * pace the EL feed against the BL INPUT count. */
	/* The feed gate paces EL INPUT, but the EL codec's OUTPUT must be
	 * drained unconditionally: MediaCodec buffer-mode wedges when all
	 * output buffers are held and nobody dequeues them (measured: EL
	 * stopped after 36 frames, fed frozen, every BL emitted EL-less).
	 * mpv f_enhancement_pair polls its EL filter pin on every frame
	 * regardless of input pacing. */
	int el_gated = (p->el_fed_count >= p->bl_fed_count + DVHW_EL_CATCHUP);
	/* parser-side EL exhaustion probe: when not gated, attempt one
	 * pull; if the parser has none left, remember it - dvhw_fel_emit's
	 * EOF-tail policy needs 'no future EL exists' as an input (drain
	 * == fed alone is NOT proof mid-file: the EL feed is catch-up
	 * paced, so between AUs the counts can transiently match while
	 * later EL packets are still coming). */
	if (!el_gated) {
		/* reuse the feed-loop's stack packet pattern (declared
		 * uninitialized like el_pkt below; the parser fills or leaves
		 * it untouched on failure - either way nothing to unref when
		 * it returns non-zero, matching the el_pkt handling below) */
		AVPacket probe, *pprobe = &probe;
		if (s->parser->get_dovi_el_packet(s, (void *) pprobe) != 0) {
			p->el_exhausted = 1;
		} else {
			/* parser still has EL packets: return the probe to its
			 * queue semantics - unref the pulled copy */
			av_packet_unref(&probe);
			p->el_exhausted = 0;
		}
	}
	if (el_gated)
		goto drain_el;
	while (!el_gated && s->parser->get_dovi_el_packet(s, &el_pkt) == 0) {
		uint8_t *clean = NULL;
		int clean_size = 0;
		ssize_t idx = AMediaCodec_dequeueInputBuffer(p->el_codec,
			                                        DVHW_EL_INPUT_TIMEOUT_US);
		if (idx < 0) {
			av_packet_unref(&el_pkt);
			break;	// EL codec busy: leave packets queued for the next call
		}
		/* strip any RPU NAL from the EL access unit (the EL's own RPU is
		 * not needed: the BL RPU carries the composition metadata), then
		 * convert to Annex-B: Samsung codec2 buffer-mode decoders need
		 * start-code framing, and the EL parameter sets must arrive
		 * in-band on the first AU (csd-0 alone does not always start
		 * EL decoding — proven by the EL-track extraction test where
		 * in-band VPS/SPS/PPS decode fine) */
		dovi_strip_dv_nals(el_pkt.data, el_pkt.size,
		                   p->el_nal_length_size, &clean, &clean_size);
		{
			uint8_t *annexb = NULL;
			int annexb_size = 0;
			if (clean && clean_size > 0)
				annexb = dvhw_au_to_annexb(clean, clean_size,
							 p->el_nal_length_size,
							 &annexb_size);
			av_free(clean);
			clean = annexb;
			clean_size = annexb_size;
			if (clean && clean_size > 0 && !p->el_params_sent) {
				/* prepend VPS/SPS/PPS from the EL hvcC to the first AU */
				int ps_size = 0;
				uint8_t *ps = dvhw_hvcc_params_annexb(
					(const uint8_t *) dec->video->dv_el_extraData,
					dec->video->dv_el_extraDataSize, &ps_size);
				if (ps && ps_size > 0) {
					uint8_t *merged = (uint8_t *) av_malloc(
						ps_size + clean_size +
						AV_INPUT_BUFFER_PADDING_SIZE);
					if (merged) {
						memcpy(merged, ps, (size_t) ps_size);
						memcpy(merged + ps_size, clean,
						       (size_t) clean_size);
						av_free(clean);
						clean = merged;
						clean_size += ps_size;
					}
					av_free(ps);
				}
				p->el_params_sent = 1;
			}
		}
		if (!clean || clean_size <= 0) {
			av_free(clean);
			av_packet_unref(&el_pkt);
			continue;
		}
		{
			size_t buf_size = 0;
			uint8_t *buf = AMediaCodec_getInputBuffer(p->el_codec,
			                                        (size_t) idx, &buf_size);
			if (buf && clean_size <= (int) buf_size) {
				memcpy(buf, clean, (size_t) clean_size);
				/* EL pts (parser already converted to the avos ms domain)
				 * queues as the MediaCodec timestamp; output pts returns in
				 * the same domain for pairing */
				AMediaCodec_queueInputBuffer(
					p->el_codec, (size_t) idx, 0,
					(size_t) clean_size,
					(int64_t) el_pkt.pts * 1000, 0);
				p->el_fed_count++;
			}
		}
		av_free(clean);
		av_packet_unref(&el_pkt);

		/* fed-vs-drained budget exhausted: stop pulling packets this
		 * call, the next BL AU opens a new one */
		if (p->el_fed_count >= p->bl_fed_count + DVHW_EL_CATCHUP)
			break;
	}

	/* unconditional EL output drain - runs even when input is gated */
drain_el:
	/* pull ALL ready EL frames: EL outputs lag BL by the reorder depth
	 * (B-frames), so draining one-per-input would leave every EL frame
	 * arriving after its BL partner already left (dropped as stale) */
	{
		for (;;) {
			AMediaCodecBufferInfo einfo;
			ssize_t oidx = AMediaCodec_dequeueOutputBuffer(p->el_codec,
			                                         &einfo, 0);
			if (oidx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
				AMediaFormat *ofmt = AMediaCodec_getOutputFormat(p->el_codec);
				int32_t v = 0;
				if (ofmt) {
					if (AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_WIDTH, &v) && v > 0)
						p->el_width = v;
					if (AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_HEIGHT, &v) && v > 0)
						p->el_height = v;
					if (AMediaFormat_getInt32(ofmt, "stride", &v) && v > 0)
						p->el_stride = v;
					if (AMediaFormat_getInt32(ofmt, "slice-height", &v) && v > 0)
						p->el_slice_height = v;
					if (AMediaFormat_getInt32(ofmt, "color-format", &v))
						p->el_color_format = v;
					AMediaFormat_delete(ofmt);
				}
				serprintf(TAG ": EL output format %dx%d stride %d slice %d color %d\n",
				          p->el_width, p->el_height, p->el_stride,
				          p->el_slice_height, p->el_color_format);
				continue;
			}
			if (oidx < 0 || einfo.size <= 0)
				break;
			AVFrame *el = dvhw_copy_yuv420(p->el_codec, oidx, &einfo,
			                               p->el_width, p->el_height,
			                               p->el_stride, p->el_slice_height,
			                               p->el_color_format, 0);
			if (!el)
				break;
			el->color_primaries = AVCOL_PRI_BT2020;
			el->color_trc      = AVCOL_TRC_SMPTE2084;
			el->colorspace     = AVCOL_SPC_BT2020_NCL;
			el->color_range    = AVCOL_RANGE_MPEG;
			p->el_drain_count++;
			p->el_seen = 1;
			if (p->el_q_count >= DVHW_PENDING_MAX) {
				av_frame_free(&p->el_q[0]);
				memmove(&p->el_q[0], &p->el_q[1],
				        (DVHW_PENDING_MAX - 1) * sizeof(p->el_q[0]));
				p->el_q_count--;
			}
			p->el_q[p->el_q_count++] = el;
			continue;
		}
	}
}

/* FEL emission: the BL output queue (bl_q) holds decoded BL frames whose EL
 * has not arrived yet; emit the head when its EL is present, when a later
 * EL proves it absent (pts jumped past), or when the queue is full. Returns
 * the frame to hand out or NULL (nothing ready). mpv f_enhancement_pair
 * pending-queue semantics. */
static AVFrame *dvhw_fel_emit(PRIV *p, AVFrame **el_out)
{
	AVFrame *bl = NULL;
	*el_out = NULL;
	if (!p->bl_q_count)
		return NULL;
	/* No EL decoder (P8.x / P7 BL-only): nothing to pair with, the BL
	 * emits as soon as it decodes. */
	if (!p->fel) {
		bl = p->bl_q[0];
		goto emit;
	}
	bl = p->bl_q[0];
	*el_out = dvhw_el_take_pair(p, bl->pts);
	if (*el_out)
		goto emit;
	/* mpv f_enhancement_pair parity, three affirmative-evidence policies:
	 *
	 * 1. EL older than the oldest BL: its BL partner already left (or
	 *    never existed) - the EL is stale, drop it and retry. Holding it
	 *    only wedges the queue head (mpv: "dropping stale EL").
	 * 2. EL newer than the oldest BL: this BL's EL will never come -
	 *    emit BL-only (mpv give_up: el_newer).
	 * 3. Queue pressure: emit BL-only ONLY after the EL decoder has
	 *    produced at least one frame since the last flush. During EL
	 *    warm-up (startup and after seeks) a full bl_q only means the
	 *    EL decoder is still spinning up - BL-only frames there render
	 *    green (FEL metadata without residual). mpv commit 3b4caf0
	 *    ("don't emit BL-only frames before the EL warms up").
	 */
	while (p->el_q_count && p->el_q[0]->pts < bl->pts) {
		av_frame_free(&p->el_q[0]);
		memmove(&p->el_q[0], &p->el_q[1],
		        (p->el_q_count - 1) * sizeof(p->el_q[0]));
		p->el_q_count--;
	}
	if (p->el_q_count && p->el_q[0]->pts > bl->pts)
		goto emit;	/* proven absent: EL jumped past this BL */
	if (p->bl_q_count >= 4 && p->el_seen)
		goto emit;	/* queue pressure, EL warm */
	/* EL fully drained AND the parser has no more EL packets (probed
	 * in dvhw_el_feed, cleared on flush): the parked BLs' ELs will never
	 * arrive - emit BL-only, mpv's el_eof give-up (f_enhancement_pair.c:
	 * EOF drains the pending queue as BL-only). Without this the last
	 * 1-3 frames of every FEL file stay parked forever (their ELs are
	 * still inside the EL codec's reorder window when the AU stream
	 * ends) and the file end truncates. drain==fed alone is not proof
	 * mid-file (feed is catch-up paced), hence the el_exhausted gate. */
	if (p->el_seen && p->el_exhausted &&
	    p->el_drain_count >= p->el_fed_count &&
	    p->el_q_count == 0)
		goto emit;
	return NULL;
emit:
	memmove(&p->bl_q[0], &p->bl_q[1],
	        (p->bl_q_count - 1) * sizeof(p->bl_q[0]));
	p->bl_q_count--;
	return bl;
}

static int dvhw_decode2(STREAM_DEC_VIDEO *dec, UCHAR *data, int size,
                        VIDEO_FRAME **pin_frame, VIDEO_FRAME **pout_frame,
                        int *_decoded, int *_time)
{
	PRIV *p = (PRIV *) dec->priv;
	VIDEO_FRAME *avos_frame = *pin_frame;
	AMediaCodecBufferInfo info;
	uint8_t *rpu = NULL, *clean = NULL;
	int rpu_size = 0, clean_size = 0;
	int64_t pts_us;
	int decoded = 0;
	ssize_t idx;

	*pin_frame = NULL;
	avos_frame->valid = 0;
	if (_time)
		*_time = 0;
	if (!p->codec)
		return 0;

	pts_us = (int64_t) avos_frame->time * 1000;
	// the whole access unit is consumed by this call once it is queued to
	// MediaCodec: the stream engine advances by *_decoded bytes, and the
	// frame comes back asynchronously via *pout_frame when the codec is done
	int au_consumed = 0;

	// extract the RPU and strip DV-specific NALs before MediaCodec
	dovi_extract_rpu(data, size, p->nal_length_size, &rpu, &rpu_size);
	if (dovi_strip_dv_nals(data, size, p->nal_length_size, &clean, &clean_size) ||
	    !clean || clean_size <= 0) {
		// metadata-only access unit: nothing to decode, input consumed
		av_free(rpu);
		av_free(clean);
		if (_decoded)
			*_decoded = size;
		return 0;
	}

	idx = AMediaCodec_dequeueInputBuffer(p->codec, DVHW_INPUT_TIMEOUT_US);
	if (idx >= 0) {
		size_t buf_size = 0;
		uint8_t *buf = AMediaCodec_getInputBuffer(p->codec, (size_t) idx, &buf_size);
		if (buf && clean_size <= (int) buf_size) {
			memcpy(buf, clean, clean_size);
			if (AMediaCodec_queueInputBuffer(p->codec, (size_t) idx, 0,
			                                 (size_t) clean_size, pts_us, 0) == 0) {
				dvhw_pending_push(p, pts_us, rpu, rpu_size);
				rpu = NULL;	// ownership moved to the pending map
				au_consumed = 1;
				p->bl_fed_count++;
			}
		} else {
			/* oversized AU or no backing buffer: leaving au_consumed=0
			 * would make the engine re-feed the identical AU forever
			 * (measured livelock shape: ~10Hz with the dequeue timeout
			 * as the stall). Consume and drop like the timeout path. */
			au_consumed = 1;
			serprintf(TAG ": AU too large for input buffer (%d > %d), dropping\n",
			          clean_size, (int) buf_size);
		}
	} else {
		serprintf(TAG ": no input buffer, dropping AU\n");
		au_consumed = 1;	// cannot retry: let the engine move on
	}
	av_free(rpu);
	av_free(clean);

	// FEL: drain EL packets into the second codec and collect ready frames
	// so pairing can happen when the BL output lands
	if (p->fel)
		dvhw_el_feed(dec);

	// one output per call keeps the 1:1 pacing with the sink frame pool
	idx = AMediaCodec_dequeueOutputBuffer(p->codec, &info, 0);
	if (p->buffer_mode) {
		// ---- BL byte-buffer output: YUV420 copy, pts-pair with the EL ----
		if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
			AMediaFormat *ofmt = AMediaCodec_getOutputFormat(p->codec);
			int32_t v = 0;
			if (ofmt) {
				if (AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_WIDTH, &v) && v > 0)
					p->width = v;
				if (AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_HEIGHT, &v) && v > 0)
					p->height = v;
				if (AMediaFormat_getInt32(ofmt, "stride", &v) && v > 0)
					p->stride = v;
				if (AMediaFormat_getInt32(ofmt, "slice-height", &v) && v > 0)
					p->slice_height = v;
				if (AMediaFormat_getInt32(ofmt, "color-format", &v))
					p->color_format = v;
				AMediaFormat_delete(ofmt);
			}
				serprintf(TAG ": BL output format %dx%d stride %d slice %d color %d\n",
				  p->width, p->height, p->stride, p->slice_height,
				  p->color_format);
		} else if (idx >= 0 && info.size > 0) {
			AVFrame *bl = dvhw_copy_yuv420(p->codec, idx, &info,
			                               p->width, p->height,
			                               p->stride, p->slice_height,
			                               p->color_format, 0);
			if (bl) {
				/* BT.2020/PQ color identity: pl_frame_from_avframe reads these
				 * from the AVFrame; hevcdec would have set them from the SPS
				 * VUI, the MediaCodec copy needs them explicitly. The DV
				 * repr (PL_COLOR_SYSTEM_DOLBYVISION) overrides .sys via
				 * map_dovi when the metadata side data is present. */
				bl->color_primaries = AVCOL_PRI_BT2020;
				bl->color_trc      = AVCOL_TRC_SMPTE2084;
				bl->colorspace     = AVCOL_SPC_BT2020_NCL;
				bl->color_range    = AVCOL_RANGE_MPEG;
				uint8_t *rpu2 = NULL;
				int rpu2_size = 0;
				/* RPU of this frame, extracted at input time, keyed by the
					 * MediaCodec timestamp we queued with */
				dvhw_pending_take(p, info.presentationTimeUs, &rpu2, &rpu2_size);
				if (rpu2 && rpu2_size > 0) {
					/* pl_map_avframe_ex(map_dovi) consumes BOTH side datas:
					 * AV_FRAME_DATA_DOVI_METADATA drives the full DV repr
					 * (reshape + NLQ -> PL_COLOR_SYSTEM_DOLBYVISION,
					 * nlq_active -> EL composition), and the raw RPU buffer
					 * feeds pl_hdr_metadata_from_dovi_rpu (L1 trim). The
						 * software path gets these from hevcdec; here they are
						 * attached from the extracted RPU NAL. */
					AVDOVIMetadata *meta =
						dovi_rpu_parse_to_avmetadata(rpu2, (size_t) rpu2_size);
					if (meta) {
						AVBufferRef *mbuf = av_buffer_create((uint8_t*) meta,
							sizeof(AVDOVIMetadata),
							dvhw_meta_buf_free, meta, 0);
						if (mbuf) {
							AVFrameSideData *sd =
								av_frame_new_side_data_from_buf(
									bl, AV_FRAME_DATA_DOVI_METADATA, mbuf);
							if (!sd)
								av_buffer_unref(&mbuf);
						} else {
							av_free(meta);
						}
					}
					{
						AVFrameSideData *sd =
							av_frame_new_side_data(bl,
								AV_FRAME_DATA_DOVI_RPU_BUFFER, rpu2_size);
						if (sd)
							memcpy(sd->data, rpu2, rpu2_size);
					}
					av_free(rpu2);
				}
				/* park the BL frame: its EL partner may still be in the EL
				 * codec pipeline (async reorder lag); dvhw_fel_emit pairs */
				if (p->bl_q_count >= 4) {
					/* mpv holds pending BLs through the EL warm-up (QUEUE_MAX=8
				 * + set_extra_hw_frames) and only drops with affirmative
				 * stale-EL evidence; dropping unconditionally here would
				 * eat the first frames after every seek on a cold EL codec.
					 * Once the EL has produced at least one frame (el_seen),
					 * pressure-dropping the oldest BL matches the emit-side
					 * policy. */
					if (p->el_seen) {
						av_frame_free(&p->bl_q[0]);
						memmove(&p->bl_q[0], &p->bl_q[1],
						        3 * sizeof(p->bl_q[0]));
						p->bl_q_count--;
					}
				}
				p->bl_q[p->bl_q_count++] = bl;
			}
		} else if (idx >= 0) {
				AMediaCodec_releaseOutputBuffer(p->codec, (size_t) idx, false);
		}

		/* try to emit the oldest parked BL (pairs with its EL if ready) */
		{
			AVFrame *el_emit = NULL;
			AVFrame *bl_emit = dvhw_fel_emit(p, &el_emit);
			if (bl_emit) {
				av_frame_free((AVFrame**) &avos_frame->handle[1]);
				avos_frame->handle[1] = (void*) el_emit;
				avos_frame->dec = dec;
				avos_frame->priv = (void*) bl_emit;
				avos_frame->valid = 1;
				avos_frame->error = 0;
			/* geometry: the engine compares these against s->video->width
				 * to emit VIDEO_SIZE_CHANGED -> Java onVideoSizeChanged drives
				 * SurfaceController.setVideoSize (initial layout AND the
				 * aspect-ratio button). Without them mVideoWidth stays 0,
				 * updateSurface() early-returns and the AR switch is dead. */
				avos_frame->width  = bl_emit->width;
				avos_frame->height = bl_emit->height;
			avos_frame->pts = bl_emit->pts;
			avos_frame->time = (int) bl_emit->pts;
			avos_frame->type = 2;
			avos_frame->interlaced = 0;
				avos_frame->top_field_first = 0;
			decoded = 1;
			*pout_frame = avos_frame;
		}
		}
	} else if (idx >= 0) {
		/* no EL/buffer pipeline can ever get here (buffer_mode is always
	 * set in dvhw_open); the old OES surface path was removed */
		AMediaCodec_releaseOutputBuffer(p->codec, (size_t) idx, false);
	} else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
		AMediaFormat *ofmt = AMediaCodec_getOutputFormat(p->codec);
		if (ofmt) {
			int32_t w = 0, h = 0;
			if (AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_WIDTH, &w) && w > 0)
				p->width = w;
			if (AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_HEIGHT, &h) && h > 0)
				p->height = h;
			AMediaFormat_delete(ofmt);
		}
		serprintf(TAG ": output format %dx%d\n", p->width, p->height);
	}

	if (_decoded)
		*_decoded = au_consumed ? size : decoded;
	return 0;
}

static int dvhw_flush(STREAM_DEC_VIDEO *dec)
{
	PRIV *p = (PRIV *) dec->priv;
	if (p->codec)
		AMediaCodec_flush(p->codec);
	if (p->el_codec)
		AMediaCodec_flush(p->el_codec);
	dvhw_el_q_clear(p);
	dvhw_bl_q_clear(p);
	dvhw_pending_clear(p);
	p->el_exhausted = 0;	/* seek: the parser refills the EL queue */
	return 0;
}

static int dvhw_render(STREAM_DEC_VIDEO *dec, VIDEO_FRAME *dst, VIDEO_FRAME *src)
{
	// OES path: rendering happens in the DV sink (dovi_gl). FEL path: the
	// BL/EL AVFrames were consumed by dovi_gl_render in sink_put; free them
	// here like codec_ffmpeg_video does (pool recycle point).
	if (src->priv && src->dec == dec) {
		av_frame_free((AVFrame **) &src->priv);
		if (src->handle[1])
			av_frame_free((AVFrame **) &src->handle[1]);
		src->dec = NULL;
	}
	return 0;
}

static int dvhw_get_rc(STREAM_DEC_VIDEO *dec, STREAM_RC *rc)
{
	if (!rc)
		return 1;
	memset(rc, 0, sizeof(STREAM_RC));
	/* Sink-owned frame pool size. Without this the rc stays zeroed and the
	 * dovi sink clamps to a 2-frame pool: 1 frame decoding + 1 rendering,
	 * zero slack — every engine hiccup lands as late-frame drops.
	 * 14 frames gives ~12 frames (~500ms at 24fps)
	 * of pipeline slack; the sink allocates the pool, we hold no buffers. */
	rc->num_frames = 14;
	rc->cpu_type = STREAM_CPU_ARM;
	rc->mem_type = STREAM_MEM_NRM;
	return 0;
}

static int dvhw_destroy(STREAM_DEC_VIDEO *dec)
{
	afree(dec->priv);
	afree(dec);
	return 0;
}

static STREAM_SINK_VIDEO *dvhw_get_sink(STREAM_DEC_VIDEO *dec)
{
	// same libplacebo sink as the software DV path
	if (dec && dec->video && dec->video->format == VIDEO_FORMAT_DOLBY_VISION &&
	    libavos_get_dolby_vision_mode() != 0) {
		STREAM *s = (STREAM *) dec->ctx;
		void *surface = s ? stream_get_surface_handle(s) : NULL;
		if (surface) {
			STREAM_SINK_VIDEO *sink = stream_sink_video_dovi_new(surface);
			if (sink) {
				serprintf(TAG ": Dolby Vision tone-map sink (libplacebo)\n");
				return sink;
			}
		}
	}
	return NULL;
}

static STREAM_DEC_VIDEO *_new(void)
{
	STREAM_DEC_VIDEO *dec = (STREAM_DEC_VIDEO *) amalloc(sizeof(STREAM_DEC_VIDEO));
	if (!dec)
		return NULL;
	memset(dec, 0, sizeof(STREAM_DEC_VIDEO));

	static char name[] = "mediacodec-dovi";
	dec->name = name;
	dec->destroy = dvhw_destroy;
	dec->open = dvhw_open;
	dec->close = dvhw_close;
	dec->prepare = dvhw_prepare;
	dec->cleanup = dvhw_cleanup;
	dec->decode = NULL;
	dec->decode2 = dvhw_decode2;
	dec->flush = dvhw_flush;
	dec->render = dvhw_render;
	dec->get_rc = dvhw_get_rc;
	dec->get_sink = dvhw_get_sink;

	if (!(dec->priv = acalloc(1, sizeof(PRIV)))) {
		afree(dec);
		return NULL;
	}
	return dec;
}

#define MAXW VIDEO_MAX_WIDTH
#define MAXH VIDEO_MAX_HEIGHT

// Registered at SFDEC_OMXCODEC priority (DSP2): below the sfdec2 passthrough
// decoder (DSP3, which rejects DV in tone-map mode) and above the software
// ffmpeg decoder (GPP). FEL content fails open() and lands on the software
// path for full reshaping parity.
STREAM_REGISTER_DEC_VIDEO2( VIDEO_FORMAT_DOLBY_VISION, 0, MAXW, MAXH,
                            0, SFDEC_OMXCODEC, _new,
                            "mediacodec-dovi", NULL );

#endif /* CONFIG_ANDROID && CONFIG_DOVI_TONEMAP */

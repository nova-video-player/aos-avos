/*
 * Copyright (C) 2026 The Nova Video Player Project
 *
 * Dolby Vision -> HDR10 tone-mapping renderer built on EGL + libplacebo,
 * following the mpv vo_gpu_next/libplacebo approach:
 *   - software-decoded HEVC base layer AVFrames are uploaded to the GPU
 *   - AV_FRAME_DATA_DOVI_METADATA is mapped via pl_map_avdovi_metadata
 *     (reshaping curves) and AV_FRAME_DATA_DOVI_RPU_BUFFER via
 *     pl_hdr_metadata_from_dovi_rpu (dynamic L1 metadata, libdovi)
 *   - an optional enhancement-layer AVFrame (Dolby Vision profile 7) is
 *     attached as pl_frame.enhancement_layer for compositing
 *   - output is rendered as HDR10 (BT.2020 + SMPTE2084/PQ) when the EGL
 *     surface supports it, otherwise tone-mapped to SDR
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef _DOVI_GL_H
#define _DOVI_GL_H

#include <stdint.h>

struct AVFrame;

/*
 * Create the renderer bound to the given ANativeWindow.
 *
 * ctx          : receives the opaque renderer context
 * native_window: target ANativeWindow (ownership stays with the caller,
 *                the renderer acquires a reference)
 *
 * Returns 0 on success, non-zero if EGL/libplacebo could not be set up
 * (caller should fall back to another video path).
 */
int dovi_gl_open(void **ctx, void *native_window);

/*
 * Render one frame. `bl` is the decoded base-layer AVFrame (must carry
 * AV_FRAME_DATA_DOVI_METADATA / AV_FRAME_DATA_DOVI_RPU_BUFFER side data
 * for DV reshaping to apply). `el` is an optional enhancement-layer
 * AVFrame (profile 7) and may be NULL.
 *
 * Returns 0 on success.
 */
int dovi_gl_render(void *ctx, struct AVFrame *bl, struct AVFrame *el);

/*
 * Present (eglSwapBuffers) the most recent frame rendered by
 * dovi_gl_render. Split from the render step so all GPU work is
 * enqueued early (mpv draw_frame) and only the blocking swap happens at
 * the frame's blit_time (mpv flip_page). With max_swapchain_depth = 3,
 * swap_buffers fences pace up to three frames in flight.
 */
void dovi_gl_present(void *ctx);

/*
 * Destroy the renderer and release all GPU/EGL resources.
 */
void dovi_gl_close(void *ctx);

/*
 * Hardware-decoded frame descriptor handed from codec_mediacodec_dovi
 * through the DV sink into the renderer. The codec module owns the
 * AImage/AHardwareBuffer lifetime; everything is released through the
 * release() callback once the frame has been rendered.
 */
typedef struct dovi_hw_frame {
	void   *image;		/* AImage * (kept alive until rendered) */
	void   *ahb;		/* AHardwareBuffer * (acquired reference) */
	int     width, height;
	uint8_t *rpu;		/* extracted RPU NAL for this frame (av_malloc'd) */
	int     rpu_size;
	void  (*release)(struct dovi_hw_frame *f);
} dovi_hw_frame;

/*
 * Render one hardware-decoded frame. The MediaCodec output was imported
 * as AHardwareBuffer -> EGLImage -> GL_TEXTURE_EXTERNAL_OES, so the GPU
 * sampler has already converted the base layer to BT.2020/PQ RGB; the
 * per-frame RPU supplies the dynamic (L1 trim) HDR metadata via
 * pl_hdr_metadata_from_dovi_rpu. Pixel-level DV reshaping/NLQ composition
 * requires YUV-domain pixels and stays on the software path (see the plan).
 *
 * Returns 0 on success. The frame's release() callback is invoked by the
 * sink after this returns, regardless of success.
 */
int dovi_gl_render_hw(void *ctx, dovi_hw_frame *hw);

#endif /* _DOVI_GL_H */

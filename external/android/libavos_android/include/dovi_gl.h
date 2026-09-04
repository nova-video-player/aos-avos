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
 * Declare the content frame rate on the ANativeWindow (API 30+
 * ANativeWindow_setFrameRate). Without it, SurfaceFlinger has no frame-rate
 * hint for this surface: back-buffer acquisition serializes on the
 * compositor cadence and eglSwapBuffers measured ~20ms steady-state blocks
 * even at 120Hz displays - a hard ~43 presents/s ceiling on 48fps content
 * with a growing forced-late backlog (measured on S24). With the hint,
 * SF schedules the surface at content rate and the swap fence releases on
 * cadence. Returns 0 if applied (or unsupported and ignored).
 */
int dovi_gl_set_framerate(void *ctx, float fps);

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
 * Present with a desired latch time (EGL_ANDROID_presentation_time).
 * `target_monotonic_ns' is the CLOCK_MONOTONIC nanosecond timestamp at
 * which SurfaceFlinger should composite this buffer; SF latches the
 * frame on the vsync nearest that time instead of "as soon as queued",
 * which paces content-rate frames on any panel mode (48fps@60Hz gets
 * SF's own 5:4 vsync allocation; 48fps@120Hz gets one vsync each).
 * Returns 0 when the hint was applied, -1 when unsupported (the caller
 * presents unscheduled - same as dovi_gl_present).
 */
int dovi_gl_present_at(void *ctx, int64_t target_monotonic_ns);

/*
 * Actual-presentation feedback (EGL_ANDROID_get_frame_timestamps).
 * Queries the display latch time of the frame most recently queued by
 * dovi_gl_present/dovi_gl_present_at: `*actual_monotonic_ns' receives
 * EGL_DISPLAY_PRESENT_TIME_ANDROID (CLOCK_MONOTONIC ns) once the frame
 * has latched on screen.
 * Returns 0 when a valid timestamp is available (timestamp fresh since
 * the last call), -1 when unsupported/absent (caller must skip the
 * feedback path), 1 when still pending (caller should re-poll), 2 when
 * no one-behind frame is queued YET (first present - NOT a capability
 * failure: the caller's probe must stay open until a frame exists).
 */
int dovi_gl_present_feedback(void *ctx, int64_t *actual_monotonic_ns);

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

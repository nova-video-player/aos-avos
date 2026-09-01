/*
 * Copyright (C) 2026 The Nova Video Player Project
 *
 * Dolby Vision -> HDR10 tone-mapping renderer (EGL + libplacebo).
 * See dovi_gl.h for the pipeline description.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "global.h"
#include "debug.h"
#include "dovi_gl.h"

#include <android/native_window.h>
#include <time.h>
#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3.h>

#include <libplacebo/log.h>
#include <libplacebo/opengl.h>
#include <libplacebo/renderer.h>
#include <libplacebo/swapchain.h>
#include <libplacebo/utils/dolbyvision.h>
#include <libplacebo/utils/libav.h>

#include <libavutil/frame.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/pixdesc.h>

#ifndef EGL_GL_COLORSPACE_BT2020_PQ_EXT
#define EGL_GL_COLORSPACE_BT2020_PQ_EXT 0x3340
#endif
#ifndef EGL_GL_COLORSPACE
#define EGL_GL_COLORSPACE 0x309D
#endif
#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
typedef void *GLeglImageOES;

#define TAG "DOVI_GL"

/* Source/libavos.c: user preference, nits (0 = auto) */
extern float libavos_get_dolby_vision_target_nits(void);
extern int libavos_get_dolby_vision_plane_scaler(void);

typedef struct dovi_gl_priv {
	ANativeWindow *window;

	EGLDisplay display;
	EGLSurface surface;
	EGLContext context;
	int        surface_hdr;   /* surface created with BT.2020+PQ colorspace */

	pl_log       log;
	pl_opengl    gl;
	pl_swapchain swap;
	pl_renderer  rr;

	/* backing textures for pl_map_avframe_ex software uploads.
	 * THREE rotating sets (BL + EL each): with the depth-3 render-ahead
	 * pipeline, uploading frame N into the same texture an in-flight
	 * frame N-1/N-2 is still sampling makes glTexSubImage2D block on the
	 * CPU until that draw retires (measured 70ms/frame on Adreno 740).
	 * Rotating gives the GPU a full ring before a texture is reused,
	 * matching the swapchain's 3-buffer pacing. */
	pl_tex       tex[3][4];
	pl_tex       el_tex[3][4];
	int          tex_slot;

	int swap_w, swap_h;
} dovi_gl_priv;

static pl_voidfunc_t dovi_gl_get_proc_addr(const char *name)
{
	return (pl_voidfunc_t) eglGetProcAddress(name);
}

static void dovi_gl_swap_buffers(void *priv_data)
{
	dovi_gl_priv *p = priv_data;
	eglSwapBuffers(p->display, p->surface);
}

static int dovi_gl_has_ext(const char *exts, const char *ext)
{
	const char *s = exts;
	size_t len = strlen(ext);
	while ((s = strstr(s, ext)) != NULL) {
		if ((s == exts || s[-1] == ' ') && (s[len] == ' ' || s[len] == '\0'))
			return 1;
		s += len;
	}
	return 0;
}

static void dovi_gl_log_cb(void *priv, enum pl_log_level level, const char *msg)
{
	if (level <= PL_LOG_WARN)
		serprintf("PLGL: %s\n", msg);
}

/* Create the on-screen EGL context + libplacebo stack for the native
 * window. Kept in this file so the whole DV GL path is self-contained.
 * Returns 0 on success. */
int dovi_gl_open(void **ctx, void *native_window)
{
	dovi_gl_priv *p;
	EGLint num_configs = 0;
	EGLint major = 0, minor = 0;
	EGLConfig config = NULL;
	const EGLint config_attrs[] = {
		EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE,   8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE,  8,
		EGL_ALPHA_SIZE, 0,
		EGL_NONE,
	};
	const EGLint ctx_attrs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 3,
		EGL_NONE,
	};
	const char *exts;

	if (!ctx || !native_window)
		return -1;
	p = calloc(1, sizeof(*p));
	if (!p)
		return -1;
	p->window = (ANativeWindow *) native_window;
	ANativeWindow_acquire(p->window);

	p->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (p->display == EGL_NO_DISPLAY)
		goto fail;
	if (!eglInitialize(p->display, &major, &minor))
		goto fail;
	exts = eglQueryString(p->display, EGL_EXTENSIONS);
	serprintf(TAG ": EGL %d.%d vendor %s\n", major, minor,
	          eglQueryString(p->display, EGL_VENDOR));
	if (!exts || !dovi_gl_has_ext(exts, "EGL_ANDROID_recordable")) {
		serprintf(TAG ": EGL_ANDROID_recordable missing\n");
		goto fail;
	}
	if (!eglChooseConfig(p->display, config_attrs, &config, 1, &num_configs)
	    || num_configs < 1)
		goto fail;

	p->context = eglCreateContext(p->display, config, EGL_NO_CONTEXT,
	                               ctx_attrs);
	if (p->context == EGL_NO_CONTEXT)
		goto fail;

	p->surface = eglCreateWindowSurface(p->display, config, p->window,
	                                     NULL);
	if (p->surface == EGL_NO_SURFACE)
		goto fail;

	/* try to switch the surface to the BT.2020-PQ colorspace; harmless
	 * no-op on EGL < 1.5 without EXT_gl_colorspace_bt2020_pq */
	if (exts && dovi_gl_has_ext(exts, "EGL_KHR_gl_colorspace") &&
	    dovi_gl_has_ext(exts, "EGL_EXT_gl_colorspace_bt2020_pq")) {
		EGLSurface cs_surface = eglCreateWindowSurface(
			p->display, config, p->window,
			(const EGLint[]) { EGL_GL_COLORSPACE,
			                   EGL_GL_COLORSPACE_BT2020_PQ_EXT,
			                   EGL_NONE });
		if (cs_surface != EGL_NO_SURFACE) {
			eglDestroySurface(p->display, p->surface);
			p->surface = cs_surface;
			p->surface_hdr = 1;
		}
	}
	serprintf(TAG ": EGL surface created (%s)\n",
	          p->surface_hdr ? "HDR10 BT.2020+PQ" : "SDR fallback");

	if (!eglMakeCurrent(p->display, p->surface, p->surface, p->context))
		goto fail;

	p->log = pl_log_create(PL_API_VER, pl_log_params(
		.log_cb   = dovi_gl_log_cb,
		.log_level = PL_LOG_INFO,
	));
	if (!p->log)
		goto fail;

	p->gl = pl_opengl_create(p->log, pl_opengl_params(
		.get_proc_addr = dovi_gl_get_proc_addr,
		.allow_software = false,
	));
	if (!p->gl)
		goto fail;

	p->swap = pl_opengl_create_swapchain(p->gl, pl_opengl_swapchain_params(
		.swap_buffers = dovi_gl_swap_buffers,
		/* 3 frames in flight: the venc thread renders ahead into the EGL
		 * back buffers while previous frames wait for their vsync; the
		 * fence-wait inside swap_buffers is the backpressure point */
		.max_swapchain_depth = 3,
		.priv          = p,
	));
	if (!p->swap)
		goto fail;

	p->rr = pl_renderer_create(p->log, p->gl->gpu);
	if (!p->rr)
		goto fail;

	eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	*ctx = p;
	return 0;

fail:
	serprintf(TAG ": open failed (eglErr 0x%x)\n", eglGetError());
	eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	dovi_gl_close(p);
	return -1;
}

static void dovi_gl_target_frame(dovi_gl_priv *p, struct pl_frame *target,
                                 const struct pl_frame *image,
                                 const struct pl_swapchain_frame *sw)
{
	float user_nits = libavos_get_dolby_vision_target_nits();
	memset(target, 0, sizeof(*target));
	target->num_planes = 1;
	target->planes[0] = (struct pl_plane) {
		.components = 3,
		.component_mapping = { 0, 1, 2 },
		/* render into the swapchain's framebuffer color texture.
		 * GL swapchains report flipped=true (GL presents bottom-up);
		 * pl_frame_from_swapchain propagates this — ignoring it renders
		 * upside down (pl_frame_from_swapchain parity). */
		.texture = sw ? sw->fbo : NULL,
		.flipped = sw ? sw->flipped : false,
	};
	if (sw) {
		/* adopt the swapchain's own color representation (EGL surface
		 * colorspace BT.2020+PQ when surface_hdr) */
		target->repr = sw->color_repr;
	} else {
		target->repr = (struct pl_color_repr) {
			.sys   = PL_COLOR_SYSTEM_RGB,
			.levels = PL_COLOR_LEVELS_FULL,
		};
	}
	target->crop = (struct pl_rect2df) {
		.x1 = (float) p->swap_w,
		.y1 = (float) p->swap_h,
	};
	target->color = (struct pl_color_space) {
		.primaries = PL_COLOR_PRIM_BT_709,
		.transfer  = PL_COLOR_TRC_SRGB,
	};

	if (p->surface_hdr) {
		/* HDR10 output: the DV reshaper emits BT.2020+PQ which we forward,
		 * including dynamic metadata derived from the RPU. */
		target->color.primaries = PL_COLOR_PRIM_BT_2020;
		target->color.transfer  = PL_COLOR_TRC_PQ;
		if (image)
			target->color.hdr = image->color.hdr;
		/* user preference wins: explicit nits, else display-reported max
		 * (resolved on the Java side), else source max, else PQ ceiling */
		if (user_nits > 0.f)
			target->color.hdr.max_luma = user_nits;
		else if (!target->color.hdr.max_luma)
			target->color.hdr.max_luma = 10000.0f;
	}
}

int dovi_gl_render(void *ctx, struct AVFrame *bl, struct AVFrame *el)
{
	dovi_gl_priv *p = ctx;
	struct pl_swapchain_frame sw_frame;
	struct pl_frame image, el_image, target;
	int width, height;
	int has_el = 0;

	if (!p || !bl)
		return 1;


	/* the EGL context is kept unbound between calls (see dovi_gl_open):
	 * bind it for this render and release it again on the way out */
	if (!eglMakeCurrent(p->display, p->surface, p->surface, p->context)) {
		EGLint err = eglGetError();
		serprintf("%s: eglMakeCurrent failed in render (eglErr 0x%x)\n", TAG, err);
		return 1;
	}

	width  = ANativeWindow_getWidth(p->window);
	height = ANativeWindow_getHeight(p->window);
	if (width <= 0 || height <= 0) {
		width  = bl->width;
		height = bl->height;
	}
	if (width != p->swap_w || height != p->swap_h) {
		p->swap_w = width;
		p->swap_h = height;
		if (!pl_swapchain_resize(p->swap, &p->swap_w, &p->swap_h)) {
			serprintf("%s: pl_swapchain_resize failed\n", TAG);
			eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			return 1;
		}
	}

	if (!pl_swapchain_start_frame(p->swap, &sw_frame)) {
		serprintf("%s: pl_swapchain_start_frame failed\n", TAG);
		eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		return 1;
	}

	int slot = p->tex_slot++ % 3;

	if (!pl_map_avframe_ex(p->gl->gpu, &image, pl_avframe_params(
		.frame    = bl,
		.map_dovi = true,
		.tex      = p->tex[slot],
	))) {
		serprintf("%s: pl_map_avframe_ex failed for base layer\n", TAG);
		/* the swapchain already has a frame in progress: submit an empty
		 * frame (no image) so the state machine does not wedge */
		dovi_gl_target_frame(p, &target, NULL, &sw_frame);
		struct pl_render_params rp = pl_render_default_params;
		pl_render_image(p->rr, NULL, &target, &rp);
		if (!pl_swapchain_submit_frame(p->swap))
			serprintf("%s: emergency submit failed\n", TAG);
		pl_swapchain_swap_buffers(p->swap);
		eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		return 1;	/* frame simply not submitted; next start_frame re-acquires */
	}

	if (el) {
		if (pl_map_avframe_ex(p->gl->gpu, &el_image, pl_avframe_params(
			.frame    = el,
			.map_dovi = false,
			.tex      = p->el_tex[slot],
		))) {
			image.enhancement_layer = &el_image;
			has_el = 1;
		} else {
			serprintf("%s: enhancement layer mapping failed, rendering BL only\n", TAG);
		}
	}

	dovi_gl_target_frame(p, &target, &image, &sw_frame);



	struct pl_render_params params = pl_render_default_params;
	/* User-selectable plane scaler (SAMPLER_PLANE stage: EL residual +
	 * chroma upscaling - the libplacebo equivalent of mpv --cscale).
	 * Default 0 = inherit from the main scaler (lanczos = mpv default).
	 * Applied every render so the setting takes effect without restart. */
	{
		int ps = libavos_get_dolby_vision_plane_scaler();
		if (ps == 1) {
			params.plane_upscaler = &pl_filter_bilinear;
			params.plane_downscaler = &pl_filter_bilinear;
		} else if (ps == 2) {
			params.plane_upscaler = &pl_filter_bicubic;
			params.plane_downscaler = &pl_filter_bicubic;
		} else if (ps == 3) {
			params.plane_upscaler = &pl_filter_ewa_lanczossharp;
			params.plane_downscaler = &pl_filter_ewa_lanczossharp;
		}
	}
	bool ok = pl_render_image(p->rr, &image, &target, &params);

	pl_unmap_avframe(p->gl->gpu, &image);
	if (has_el)
		pl_unmap_avframe(p->gl->gpu, &el_image);

	if (!ok) {
		serprintf("%s: pl_render_image failed\n", TAG);
		eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		return 1;	/* frame not submitted; next start_frame re-acquires */
	}

	if (!pl_swapchain_submit_frame(p->swap)) {
		serprintf("%s: pl_swapchain_submit_frame failed\n", TAG);
		eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		return 1;
	}
	/* mpv gpu-next split: draw_frame does map+render+submit (all GPU
	 * work enqueued early, glFlush only), flip_page only swaps at the
	 * target vsync. swap_buffers (eglSwapBuffers + the swapchain-depth
	 * fence wait) is the ONLY blocking step, deferred to the frame's
	 * blit_time via dovi_gl_present. Renders into the EGL back buffer
	 * already queued; up to max_swapchain_depth frames in flight. */

	eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	return 0;
}

/* Present the frame rendered by dovi_gl_render: swap at the frame's
 * deadline. Blocks (fence wait inside pl_swapchain_swap_buffers) only
 * when 3 frames are already queued - natural backpressure, identical to
 * mpv's flip_page + libplacebo max_swapchain_depth pacing. */
void dovi_gl_present(void *ctx)
{
	dovi_gl_priv *p = ctx;
	if (!p)
		return;
	if (!eglMakeCurrent(p->display, p->surface, p->surface, p->context))
		return;
	pl_swapchain_swap_buffers(p->swap);
}

/* Hardware path: import the MediaCodec AHardwareBuffer as an OES texture.
 * The GPU sampler has already converted the base layer to BT.2020/PQ RGB
 * (buffer dataspace), so the frame is mapped as RGB and the per-frame RPU
 * supplies the dynamic HDR metadata. Pixel-level DV reshaping/NLQ needs
 * YUV-domain pixels and stays on the software path. */
int dovi_gl_render_hw(void *ctx, dovi_hw_frame *hw)
{
	dovi_gl_priv *p = ctx;
	struct pl_swapchain_frame sw_frame;
	struct pl_frame image, target;
	struct pl_render_params params;
	EGLClientBuffer client_buffer = NULL;
	EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;
	GLuint tex = 0;
	pl_tex wrapped = NULL;
	int width, height;
	int ret = 1;

	typedef EGLClientBuffer (*PFN_getNativeClientBuffer)(const AHardwareBuffer *);
	typedef EGLImageKHR (*PFN_createImage)(EGLDisplay, EGLContext, EGLenum,
	                                       EGLClientBuffer, const EGLint *);
	typedef EGLBoolean (*PFN_destroyImage)(EGLDisplay, EGLImageKHR);
	typedef void (*PFN_targetTexture2D)(GLenum, GLeglImageOES);
	PFN_getNativeClientBuffer pfGetClientBuffer;
	PFN_createImage pfCreateImage;
	PFN_destroyImage pfDestroyImage;
	PFN_targetTexture2D pfTargetTexture;

	if (!p || !hw || !hw->ahb)
		return 1;

	if (!eglMakeCurrent(p->display, p->surface, p->surface, p->context)) {
		serprintf("%s: eglMakeCurrent failed in render_hw\n", TAG);
		return 1;
	}

	/* all error paths below jump to done, which releases the binding */

	pfGetClientBuffer = (PFN_getNativeClientBuffer)
		eglGetProcAddress("eglGetNativeClientBufferANDROID");
	pfCreateImage = (PFN_createImage) eglGetProcAddress("eglCreateImageKHR");
	pfDestroyImage = (PFN_destroyImage) eglGetProcAddress("eglDestroyImageKHR");
	pfTargetTexture = (PFN_targetTexture2D)
		eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (!pfGetClientBuffer || !pfCreateImage || !pfDestroyImage || !pfTargetTexture) {
		serprintf("%s: EGL image import entry points missing\n", TAG);
		goto done;
	}

	client_buffer = pfGetClientBuffer((const AHardwareBuffer *) hw->ahb);
	if (!client_buffer) {
		serprintf("%s: eglGetNativeClientBufferANDROID failed\n", TAG);
		goto done;
	}
	egl_image = pfCreateImage(p->display, EGL_NO_CONTEXT,
	                          EGL_NATIVE_BUFFER_ANDROID, client_buffer, NULL);
	if (egl_image == EGL_NO_IMAGE_KHR) {
		serprintf("%s: eglCreateImageKHR failed 0x%x\n", TAG, eglGetError());
		goto done;
	}

	width  = ANativeWindow_getWidth(p->window);
	height = ANativeWindow_getHeight(p->window);
	if (width <= 0 || height <= 0) {
		width  = hw->width;
		height = hw->height;
	}
	if (width != p->swap_w || height != p->swap_h) {
		p->swap_w = width;
		p->swap_h = height;
		if (!pl_swapchain_resize(p->swap, &p->swap_w, &p->swap_h)) {
			serprintf("%s: pl_swapchain_resize failed\n", TAG);
			goto done;
		}
	}

	if (!pl_swapchain_start_frame(p->swap, &sw_frame)) {
		serprintf("%s: pl_swapchain_start_frame failed\n", TAG);
		goto done;
	}

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	pfTargetTexture(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES) egl_image);

	wrapped = pl_opengl_wrap(p->gl->gpu, pl_opengl_wrap_params(
		.texture = tex,
		.target  = GL_TEXTURE_EXTERNAL_OES,
		.iformat = GL_RGBA8,
		.width   = hw->width,
		.height  = hw->height,
	));
	if (!wrapped) {
		serprintf("%s: pl_opengl_wrap failed for OES texture\n", TAG);
		goto done;
	}

	memset(&image, 0, sizeof(image));
	image.num_planes = 1;
	image.planes[0].texture = wrapped;
	image.planes[0].components = 3;
	image.planes[0].component_mapping[0] = 0;
	image.planes[0].component_mapping[1] = 1;
	image.planes[0].component_mapping[2] = 2;
	image.repr.sys    = PL_COLOR_SYSTEM_RGB;
	image.repr.levels = PL_COLOR_LEVELS_FULL;
	image.color.primaries = PL_COLOR_PRIM_BT_2020;
	image.color.transfer  = PL_COLOR_TRC_PQ;
	/* per-frame dynamic (L1 trim) metadata from the extracted RPU NAL */
	if (hw->rpu && hw->rpu_size > 0)
		pl_hdr_metadata_from_dovi_rpu(&image.color.hdr, hw->rpu,
		                              (size_t) hw->rpu_size);

	dovi_gl_target_frame(p, &target, &image, &sw_frame);

	params = pl_render_default_params;
	if (!pl_render_image(p->rr, &image, &target, &params)) {
		serprintf("%s: pl_render_image failed (hw)\n", TAG);
		goto done;
	}

	/* tear down the import before presenting; glFinish below guarantees the
	 * GPU is done sampling before the buffer returns to the MediaCodec pool */
	pl_tex_destroy(p->gl->gpu, &wrapped);
	glDeleteTextures(1, &tex);
	tex = 0;
	pfDestroyImage(p->display, egl_image);
	egl_image = EGL_NO_IMAGE_KHR;

	if (!pl_swapchain_submit_frame(p->swap)) {
		serprintf("%s: pl_swapchain_submit_frame failed (hw)\n", TAG);
		goto done;
	}
	/* NO swap here: the sink's deferred dovi_gl_present swaps at the
	 * frame's blit_time (mpv flip_page split, same as the SW path).
	 * Swapping here AND in the present double-swaps per frame - the
	 * second eglSwapBuffers re-posts a stale back buffer and the screen
	 * alternates fresh/stale = brightness flicker (measured on the OES
	 * path). glFinish still blocks until the GPU is done sampling the
	 * OES import, so hw->release() (called by the sink right after
	 * this returns) can safely give the buffer back to MediaCodec. */
	glFinish();
	ret = 0;

done:
	if (wrapped)
		pl_tex_destroy(p->gl->gpu, &wrapped);
	if (tex)
		glDeleteTextures(1, &tex);
	if (egl_image != EGL_NO_IMAGE_KHR)
		pfDestroyImage(p->display, egl_image);
	eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	return ret;
}
void dovi_gl_close(void *ctx)
{
	dovi_gl_priv *p = ctx;
	if (!p)
		return;

	/* the context is kept unbound between renders (pipelined flip_page):
	 * libplacebo destroy paths issue GL calls, so bind it for the teardown
	 * or pl_renderer_destroy hits gl_poll_callbacks' !unreachable assert
	 * (SIGABRT measured when the sink closes outside a render cycle, e.g.
	 * leaving the player settings screen while a video is loaded) */
	if (p->display != EGL_NO_DISPLAY && p->context != EGL_NO_CONTEXT)
		eglMakeCurrent(p->display, p->surface, p->surface, p->context);

	if (p->rr)
		pl_renderer_destroy(&p->rr);
	if (p->swap)
		pl_swapchain_destroy(&p->swap);
	if (p->gl)
		pl_opengl_destroy(&p->gl);
	if (p->log)
		pl_log_destroy(&p->log);

	if (p->display != EGL_NO_DISPLAY) {
		eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (p->surface != EGL_NO_SURFACE)
			eglDestroySurface(p->display, p->surface);
		if (p->context != EGL_NO_CONTEXT)
			eglDestroyContext(p->display, p->context);
		eglTerminate(p->display);
	}
	if (p->window)
		ANativeWindow_release(p->window);

	free(p);
}

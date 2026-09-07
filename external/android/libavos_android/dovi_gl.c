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

#include <dlfcn.h>

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

/* --- EGL_ANDROID_presentation_time / EGL_ANDROID_get_frame_timestamps ---
 * PFN types come from eglext.h (r26d): PFNEGLPRESENTATIONTIMEANDROIDPROC,
 * PFNEGLGETFRAMETIMESTAMPSANDROIDPROC, PFNEGLGETNEXTFRAMEIDANDROIDPROC.
 * dlsym'd at runtime like dvgl_fps_api (the module targets API 21).
 * EGL_TIMESTAMP_PENDING_ANDROID (-2) vs EGL_TIMESTAMP_INVALID (-1)
 * distinguish "not latched yet" from "no data". Declared BEFORE
 * dovi_gl_priv: the struct carries the resolved pointers. */
typedef EGLBoolean (*PFN_eglPresentationTimeANDROID)(EGLDisplay, EGLSurface, khronos_stime_nanoseconds_t);
typedef EGLBoolean (*PFN_eglGetFrameTimestampsANDROID)(EGLDisplay, EGLSurface, EGLuint64KHR, EGLint, const EGLint *, EGLnsecsANDROID *);
typedef EGLBoolean (*PFN_eglGetNextFrameIdANDROID)(EGLDisplay, EGLSurface, EGLuint64KHR *);

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

	/* --- EGL presentation feedback (the VideoClock hooks, JRiver/mpv
	 * parity on Android): eglPresentationTimeANDROID schedules each
	 * buffer's latch at the frame's content deadline (SF then paces
	 * content-rate frames on whatever mode the panel picked - 48@60Hz
	 * gets SF's own vsync allocation instead of our sleep loop racing
	 * the compositor), and eglGetFrameTimestampsANDROID returns the
	 * ACTUAL display-present time, which the dovi sink feeds back into
	 * its presentation clock so vtime advances at physical presentation
	 * rate (closing the loop: no open-loop drift, no audio-clock-only
	 * anchoring). Both are EGL_ANDROID extensions (26/28+), dlsym'd
	 * exactly like dvgl_fps_api above; has_* flags gate their use per
	 * open, last_feedback_frame_id tracks the newest queued frame so
	 * the sink polls exactly one new timestamp per present. */
	int has_present_at;		/* eglPresentationTimeANDROID */
	int has_frame_ts;		/* eglGetFrameTimestampsANDROID + getNextFrameId */
	EGLuint64KHR next_frame_id;	/* id the NEXT swap will queue (spec:
					 * eglGetNextFrameIdANDROID is a BEFORE-swap
					 * query); captured by dovi_gl_present */
	EGLuint64KHR prev2_frame_id;	/* id TWO presents back: with scheduled
					   presents the swap runs up to one period EARLY, so the
					   one-behind target is still ~dur in the future at poll
					   time (always PENDING, measured fb=0) - the two-behind
					   frame's target passed ~one period ago: its timestamp
					   has landed and is the freshest guaranteed sample. */
	EGLuint64KHR prev3_frame_id;	/* id THREE presents back: on-grid free-run
					   swaps latch at a fixed vsync phase, so a compositor
					   hiccup can leave prev AND prev2 PENDING while the
					   landed sample sits two slots back (measured diag6
					   13:59: gate_to 10-12 with both queries PENDING, the
					   samples lost forever -> depth-2 gate starved -> banked
					   frames -> the 1-per-5-30s judder). Extending the
					   fallback ring one more slot keeps a landed sample
					   reachable (~3 frame ids on a depth-6 swapchain). */
	EGLuint64KHR prev_frame_id;	/* id of the frame queued by the
					 * PREVIOUS present - queried by
					 * dovi_gl_present_feedback. One-behind:
					 * on a depth-3 swapchain at 60Hz the
					 * just-queued frame latches 2-3 vsyncs
					 * later; polling IT blocks the venc thread
					 * ~28ms/present (measured 30/s ceiling).
					 * The PREVIOUS frame latched a frame ago -
					 * its query returns immediately. */
	PFN_eglPresentationTimeANDROID eglPresentationTime;
	PFN_eglGetFrameTimestampsANDROID eglGetFrameTimestamps;
	PFN_eglGetNextFrameIdANDROID eglGetNextFrameId;
} dovi_gl_priv;

static pl_voidfunc_t dovi_gl_get_proc_addr(const char *name)
{
	return (pl_voidfunc_t) eglGetProcAddress(name);
}

/* --- runtime loader for the two EGL_ANDROID entry points --- */
static struct {
	void	*lib;
	PFN_eglPresentationTimeANDROID	eglPresentationTime;
	PFN_eglGetFrameTimestampsANDROID	eglGetFrameTimestamps;
	PFN_eglGetNextFrameIdANDROID	eglGetNextFrameId;
	int	 loaded;
} dvgl_present_api;

static int dvgl_present_api_load(void)
{
	if (dvgl_present_api.loaded)
		return 0;
	dvgl_present_api.lib = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
	if (!dvgl_present_api.lib)
		return 1;
	dvgl_present_api.eglPresentationTime = (PFN_eglPresentationTimeANDROID)
		dlsym(dvgl_present_api.lib, "eglPresentationTimeANDROID");
	dvgl_present_api.eglGetFrameTimestamps = (PFN_eglGetFrameTimestampsANDROID)
		dlsym(dvgl_present_api.lib, "eglGetFrameTimestampsANDROID");
	dvgl_present_api.eglGetNextFrameId = (PFN_eglGetNextFrameIdANDROID)
		dlsym(dvgl_present_api.lib, "eglGetNextFrameIdANDROID");
	/* partial is fine: present_at and feedback gate independently */
	dvgl_present_api.loaded = 1;
	return 0;
}

static void dovi_gl_swap_buffers(void *priv_data)
{
	dovi_gl_priv *p = priv_data;
	eglSwapBuffers(p->display, p->surface);
	/* NOTE: the frame-id capture lives in dovi_gl_present (before the
	 * swap), NOT here: eglGetNextFrameIdANDROID is a BEFORE-swap query
	 * ('identifier for the next frame to be swapped', per the
	 * EGL_ANDROID_get_frame_timestamps spec) - calling it after the
	 * swap yields the id of the frame AFTER the one just queued, and
	 * every feedback poll for that id stays PENDING forever (measured:
	 * fb=0 steady-state, the probe disabled itself on the first try). */
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

/* ANativeWindow_setFrameRate entry points (API 30+), dlsym'd at runtime.
 * The module builds at APP_PLATFORM android-21, so the NDK headers do not
 * declare these and a compile-time #if __ANDROID_API__ >= 30 guard compiles
 * the whole hint out - measured: the dovi sink believed it had latched 48Hz
 * (framerate_set=1 swallowed the -1), while dumpsys showed mActiveModeId=1
 * (120Hz) and presents pinned at ~40/s = 3 vsyncs on 48fps FEL content; the
 * whole pipeline then shed the ~8/s surplus as park/decdrops. dlsym keeps
 * libavos.so loading on <API 30 devices exactly like dvhw_api's AImage_*
 * pattern (codec_mediacodec_dovi.c), and actually reaches the API on 30+. */
#ifndef ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE
/* AOSP ANativeWindow.h values: DEFAULT=0 (any rate ok - SF picks a
 * convenient mode, measured: 48fps content landed on 60Hz and presents
 * stayed ~40/s), FIXED_SOURCE=1 (fixed-rate content - video: SF picks a
 * mode where the content rate latches, the 48Hz one-vsync-per-frame
 * cadence). Media players use FIXED_SOURCE for video surfaces. */
#define ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE 1
#endif
/* AOSP ANativeWindow.h: ANativeWindow_setFrameRateWithChangeStrategy
 * (API 31+): strategy 1 = CHANGE_FRAME_RATE_ALWAYS. The default 3-arg call
 * uses ONLY_IF_SEAMLESS and Samsung's 120->48 transition counts as
 * non-seamless, so SF kept the display at 60/96Hz (measured: FIXED_SOURCE
 * hint accepted, window layer vote read back 48Hz ExactOrMultiple, but
 * mActiveModeId stayed 3 (60Hz) and presents pinned ~40/s). ALWAYS is
 * what Surface.setFrameRate(..., CHANGE_FRAME_RATE_ALWAYS) - the Java
 * player's own mode-2 path (Player.java:1064) - uses to force Samsung
 * panels; fall back to the 3-arg call when the 4-arg symbol is absent
 * (API 30). */
#define ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS 1
typedef int32_t (*PFN_ANativeWindow_setFrameRateWithChangeStrategy)(
	ANativeWindow *, float, int8_t, int8_t);
typedef int32_t (*PFN_ANativeWindow_setFrameRate)(ANativeWindow *, float, int8_t);

static struct {
	void	*lib;
	PFN_ANativeWindow_setFrameRateWithChangeStrategy setFrameRate2;
	PFN_ANativeWindow_setFrameRate setFrameRate;
	int	 loaded;
} dvgl_fps_api;

static int dvgl_fps_api_load(void)
{
	if (dvgl_fps_api.loaded)
		return 0;
	dvgl_fps_api.lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
	if (!dvgl_fps_api.lib) {
		serprintf(TAG ": cannot dlopen libandroid.so for setFrameRate\n");
		return 1;
	}
	dvgl_fps_api.setFrameRate2 = (PFN_ANativeWindow_setFrameRateWithChangeStrategy)
		dlsym(dvgl_fps_api.lib, "ANativeWindow_setFrameRateWithChangeStrategy");
	dvgl_fps_api.setFrameRate = (PFN_ANativeWindow_setFrameRate)
		dlsym(dvgl_fps_api.lib, "ANativeWindow_setFrameRate");
	if (!dvgl_fps_api.setFrameRate2 && !dvgl_fps_api.setFrameRate) {
		serprintf(TAG ": ANativeWindow_setFrameRate not present (<API 30)\n");
		dlclose(dvgl_fps_api.lib);
		dvgl_fps_api.lib = NULL;
		return 1;
	}
	dvgl_fps_api.loaded = 1;
	return 0;
}

/* Declare content fps on the window (see dovi_gl.h). Runtime-resolved so the
 * android-21 build still reaches the API 30+ entry point; pre-30 devices
 * return -1 and the sink simply paces on the default mode. */
int dovi_gl_set_framerate(void *ctx, float fps)
{
	dovi_gl_priv *p = (dovi_gl_priv *) ctx;
	if (!p || !p->window || fps <= 0.0f || fps > 240.0f)
		return -1;
	if (dvgl_fps_api_load())
		return -1;
	int32_t rc;
	if (dvgl_fps_api.setFrameRate2)
		rc = dvgl_fps_api.setFrameRate2(p->window, fps,
			ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
			ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS);
	else
		rc = dvgl_fps_api.setFrameRate(p->window, fps,
			ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
	if (rc == 0)
		serprintf(TAG ": surface frame rate set to %.3f Hz (always)\n", fps);
	else
		serprintf(TAG ": setFrameRate rc=%d (ignored)\n", (int) rc);
	return rc == 0 ? 0 : -1;
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
	/* presentation feedback hooks: resolve the EGL_ANDROID entry points
	 * (present-at scheduling + actual-latch timestamps) and gate them on
	 * BOTH the extension string and the symbol resolving. The sink keeps
	 * its deadline-wait as the fallback on any miss - behavior identical
	 * to today's build. */
	if (!dvgl_present_api_load()) {
		p->eglPresentationTime = dvgl_present_api.eglPresentationTime;
		p->eglGetFrameTimestamps = dvgl_present_api.eglGetFrameTimestamps;
		p->eglGetNextFrameId = dvgl_present_api.eglGetNextFrameId;
		p->has_present_at = (p->eglPresentationTime != NULL) &&
			exts && dovi_gl_has_ext(exts, "EGL_ANDROID_presentation_time");
		p->has_frame_ts = (p->eglGetFrameTimestamps != NULL) &&
			(p->eglGetNextFrameId != NULL) &&
			exts && dovi_gl_has_ext(exts, "EGL_ANDROID_get_frame_timestamps");
	} else {
		p->has_present_at = p->has_frame_ts = 0;
	}
	/* extension probe (symbols + extension string) - the per-surface
	 * eglSurfaceAttrib enable lives at the FINAL-surface point below
	 * (after the BT.2020-PQ recreation), not here. */
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
	 * no-op on EGL < 1.5 without EXT_gl_colorspace_bt2020_pq. NOTE:
	 * destroy the plain surface FIRST - an ANativeWindow allows only ONE
	 * EGL surface per api-connect; creating the replacement while the
	 * original is alive fails with EGL_BAD_ALLOC 'already connected to
	 * another API' (measured: HDR10 mode NEVER engaged, every run
	 * logged 'SDR fallback', and tone-mapping then runs the SDR path -
	 * visible color/quality loss on every DV file so far). */
	if (exts && dovi_gl_has_ext(exts, "EGL_KHR_gl_colorspace") &&
	    dovi_gl_has_ext(exts, "EGL_EXT_gl_colorspace_bt2020_pq")) {
		eglDestroySurface(p->display, p->surface);
		p->surface = EGL_NO_SURFACE;
		EGLSurface cs_surface = eglCreateWindowSurface(
			p->display, config, p->window,
			(const EGLint[]) { EGL_GL_COLORSPACE,
					   EGL_GL_COLORSPACE_BT2020_PQ_EXT,
					   EGL_NONE });
		if (cs_surface != EGL_NO_SURFACE) {
			p->surface = cs_surface;
			p->surface_hdr = 1;
		}
	}
	serprintf(TAG ": EGL surface created (%s)\n",
	          p->surface_hdr ? "HDR10 BT.2020+PQ" : "SDR fallback");

	/* enable per-frame timestamp collection NOW: the surface in
	 * p->surface is final at this point (the BT.2020-PQ recreation above
	 * replaced the initial plain surface - an attrib set before this
	 * point targeted a dead/EGL_NO_SURFACE handle and silently failed,
	 * leaving every eglGetFrameTimestampsANDROID query EGL_BAD_SURFACE
	 * and the sink's feedback probe permanently disabled: measured
	 * fb=0 for every run while the extension was present and the ids
	 * were correct). Spec: EGL_ANDROID_get_frame_timestamps §eglSurfaceAttrib
	 * - 'initial value is false', queries on an un-enabled surface
	 * generate EGL_BAD_SURFACE. */
	if (p->has_frame_ts) {
		if (!eglSurfaceAttrib(p->display, p->surface,
		                      EGL_TIMESTAMPS_ANDROID, EGL_TRUE))
			serprintf(TAG ": eglSurfaceAttrib(TIMESTAMPS) failed eglErr 0x%x\n",
			          eglGetError());
		else
			serprintf(TAG ": frame timestamp collection enabled\n");
		}

	if (!eglMakeCurrent(p->display, p->surface, p->surface, p->context))
		goto fail;

	/* Present pacing belongs to the venc thread's deadline wait (mpv
	 * flip_page model) with the DEFAULT swap interval: SF latches on its
	 * own vsync. An interval-0 experiment (07:13+ builds) let eglSwapBuffers
	 * return immediately but coincided with pipeline-wide degradation
	 * (decode loops 1800→434/s, put rate 43→25/s) - unthrottled 120Hz
	 * compositing of the 4K HDR layer loads the shared video bus and the
	 * codec slows. Default interval keeps the 06:22-build behavior
	 * (pres 41-43/s sustained).
	 * NOTE: the context STAYS current here - pl_opengl_create below
	 * probes GL extensions on the current context. */
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
		/* 6 with scheduled presents + cadence-locked waits: depth 2 measured
		 * a 30/s present ceiling with 33ms swaps on 60Hz - the fence wait
		 * inside swap_buffers serializes on the oldest in-flight frame
		 * retiring (2 vsyncs), and renders throttle to match, queue grows,
		 * the drain skips (13-20/s). With present_at active SF HOLDS each
		 * buffer to its target vsync instead of compositing immediately,
		 * so the old depth-3 hazard (unthrottled 120Hz coalescing, ~19.5ms
		 * SF holds) cannot recur. Depth is also the RENDER run-ahead budget:
		 * pl_swapchain_start_frame's buffer acquire only releases when the
		 * OLDEST buffer latches (SF holds it to its target), so the render
		 * can only run ahead by depth-1 presents. At 24fps with depth 3/4
		 * the acquire blocked 14-43ms serialized in front of the deadline
		 * wait (measured rend avg 23-43ms, pres 17.6-18.5/s on GoT 4K24,
		 * all drop counters zero): 4 slots = render + 2-3 held at targets
		 * leaves <1 period of acquire headroom; a jitter spike anywhere
		 * in the ring stalls the render a full extra period. Depth 6 gives
		 * ~2 periods of headroom (mpv's Android sizing for high-fps 4K;
		 * JRiver's fork: BufferCount = depth + slack + 1, up to 16). */
		.max_swapchain_depth = 6,
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
 * mpv's flip_page + libplacebo max_swapchain_depth pacing.
 *
 * Frame-id capture (BEFORE the swap): eglGetNextFrameIdANDROID 'returns
 * an identifier for the next frame to be swapped' (spec wording) - the
 * value read here IS the id the swap immediately below queues. Staged
 * into last_feedback_frame_id so dovi_gl_present_feedback polls the
 * frame THIS swap queued (querying the after-swap value instead targets
 * an unqueued frame and stays PENDING forever - measured fb=0). */
void dovi_gl_present(void *ctx)
{
	dovi_gl_priv *p = ctx;
	if (!p)
		return;
	if (!eglMakeCurrent(p->display, p->surface, p->surface, p->context))
		return;
	/* id rotation (BEFORE the swap, per the spec): next_frame_id is
	 * captured for the frame this swap queues; the id of the frame the
	 * PREVIOUS present queued becomes prev_frame_id - the feedback query
	 * target. One-behind model keeps the query non-blocking (see the
	 * struct comment). */
	if (p->has_frame_ts) {
		EGLuint64KHR fid = 0;
		if (p->eglGetNextFrameId(p->display, p->surface, &fid)) {
			p->prev3_frame_id = p->prev2_frame_id;
			p->prev2_frame_id = p->prev_frame_id;
			p->prev_frame_id = p->next_frame_id;
			p->next_frame_id = fid;
		}
	}
	pl_swapchain_swap_buffers(p->swap);
}

/* Scheduled present: tell SurfaceFlinger the CLOCK_MONOTONIC ns time at
 * which this buffer should latch, THEN swap. SF holds the buffer until
 * the vsync nearest that target (the swap itself returns immediately;
 * the latch happens in the compositor). The sink still caps its own
 * deadline-wait per mpv flip_page semantics, but with an accepted
 * target the compositor does the fine pacing: on a 60Hz panel 48fps
 * frames allocate to 5:4 vsyncs by SF, and no frame latches half a
 * frame early. JRiver VideoClock parity on Windows uses the identical
 * per-present desiredPresentTime concept via flip-model Present().
 * Returns 0 if the hint was set, -1 if unsupported (caller presents
 * unscheduled). */
int dovi_gl_present_at(void *ctx, int64_t target_monotonic_ns)
{
	dovi_gl_priv *p = ctx;
	if (!p)
		return -1;
	if (!p->has_present_at || !p->eglPresentationTime)
		return -1;
	if (!p->eglPresentationTime(p->display, p->surface,
		                           (khronos_stime_nanoseconds_t) target_monotonic_ns))
		return -1;
	return 0;
}

/* Actual-latch feedback for the ONE-BEHIND frame: the frame queued by
 * the PREVIOUS present. On a depth-3 swapchain the just-queued frame
 * latches 2-3 vsyncs out - polling it blocked the venc thread ~28ms/
 * present (measured 30/s ceiling, 18-23/s skips). The previous frame
 * latched ~a frame ago: the query returns immediately with the ACTUAL
 * display-present time (EGL_DISPLAY_PRESENT_TIME_ANDROID, CLOCK_
 * MONOTONIC ns). Returns 0 with *actual_monotonic_ns set, 1 while
 * still pending (short - only when the previous frame is still in
 * flight), 2 when no one-behind frame is queued YET (first present -
 * the caller's capability probe must NOT treat this as unsupported),
 * -1 unsupported/invalid. */
int dovi_gl_present_feedback(void *ctx, int64_t *actual_monotonic_ns)
{
	const EGLint names[1] = { EGL_DISPLAY_PRESENT_TIME_ANDROID };
	EGLnsecsANDROID values[1] = { 0 };
	dovi_gl_priv *p = ctx;
	if (!p || !actual_monotonic_ns)
		return -1;
	if (!p->has_frame_ts || !p->eglGetFrameTimestamps)
		return -1;
	if (!p->prev_frame_id)
		return 2;	/* no one-behind frame yet (first present) */
	if (!p->eglGetFrameTimestamps(p->display, p->surface,
		                          p->prev_frame_id, 1, names, values))
		return -1;
	if (values[0] == EGL_TIMESTAMP_PENDING_ANDROID) {
		/* scheduled presents swap up to one period EARLY: the one-behind
		 * target is still in the future at poll time. Fall back to the
		 * TWO-behind frame (target passed ~one period ago - its sample
		 * has landed). This is the freshest GUARANTEED sample; skipping
		 * it (returning PENDING) loses it forever once ids rotate
		 * (measured: fb=0 for entire runs on GoT 4K24). On-grid free-run
		 * presents latch at a fixed vsync phase, so a compositor hiccup
		 * can leave BOTH pending while the landed sample sits one more
		 * slot back (diag6: gate starvation, banked frames, judder) -
		 * try THREE-behind before giving up. */
		if (p->prev2_frame_id &&
		    p->eglGetFrameTimestamps(p->display, p->surface,
		                              p->prev2_frame_id, 1, names, values) &&
		    values[0] != EGL_TIMESTAMP_PENDING_ANDROID)
			goto have_sample;
		if (p->prev3_frame_id &&
		    p->eglGetFrameTimestamps(p->display, p->surface,
		                              p->prev3_frame_id, 1, names, values) &&
		    values[0] != EGL_TIMESTAMP_PENDING_ANDROID)
			goto have_sample;
		return 1;
	}
have_sample:
	if (values[0] == EGL_TIMESTAMP_INVALID_ANDROID)
		return -1;
	if (values[0] <= 0)
		return -1;
	*actual_monotonic_ns = (int64_t) values[0];
	return 0;
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

// Copyright 2026 Courville Software
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/system_properties.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <media/NdkImageReader.h>

#include "sfdec_dovi_gl.h"
#include "video_dovi_reshape_glsl.h"

#undef LOG
#define LOG(fmt, ...) do { \
    printf("sfdec_dovi_gl: " fmt "\n", ##__VA_ARGS__); \
    fflush(stdout); \
} while (0)

#ifndef EGL_GL_COLORSPACE_BT2020_PQ_EXT
#define EGL_GL_COLORSPACE_BT2020_PQ_EXT 0x3340
#endif
#define SDR_WHITE_NITS 203.0f
// Frames are queued to the window this long before their presentation time.
#define PRESENT_LEAD_NS 30000000LL
// Frames are released up to this long before their presentation time and held
// acquired until due: about 6 frames at 24 fps, 13 at 60 fps. The reader also
// counts the frame on screen and the one being drawn. 10-bit 4K frames take
// about 25 MB each, so the count follows the frame rate.
#define RELEASE_LEAD_NS 200000000LL
#define HELD_FRAMES_MIN 4
#define HELD_FRAMES_MAX 14

#ifndef EGL_COLOR_COMPONENT_TYPE_EXT
#define EGL_COLOR_COMPONENT_TYPE_EXT 0x3339
#define EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT 0x333B
#endif
#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif

// API 24/26 entry points, resolved at runtime: libsfdec targets android-21.
struct dovi_syms {
    media_status_t (*reader_new)(int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader **);
    media_status_t (*reader_window)(AImageReader *, ANativeWindow **);
    media_status_t (*reader_listener)(AImageReader *, AImageReader_ImageListener *);
    media_status_t (*reader_acquire)(AImageReader *, AImage **);
    void (*reader_delete)(AImageReader *);
    media_status_t (*image_buffer)(const AImage *, AHardwareBuffer **);
    media_status_t (*image_timestamp)(const AImage *, int64_t *);
    media_status_t (*image_crop)(const AImage *, AImageCropRect *);
    void (*image_delete)(AImage *);
    void (*buffer_describe)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
    EGLClientBuffer (*client_buffer)(const AHardwareBuffer *);
    EGLImageKHR (*create_image)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    EGLBoolean (*destroy_image)(EGLDisplay, EGLImageKHR);
    void (*image_target)(GLenum, GLeglImageOES);
    EGLBoolean (*presentation_time)(EGLDisplay, EGLSurface, EGLnsecsANDROID);
};

enum { PARAM_SLOTS = 32, QUEUE_SLOTS = 16 };

struct param_slot {
    int64_t pts_us;
    bool valid;
    VIDEO_DOVI_RESHAPE params;
};

struct queue_slot {
    int64_t image_ts_ns, present_ns;
    VIDEO_DOVI_RESHAPE params;
};

struct held_frame {
    AImage *image;
    int64_t ts, present_ns;
    int flush_gen;
    bool have; // params valid
    VIDEO_DOVI_RESHAPE params;
};

struct image_entry {
    AHardwareBuffer *buffer;
    EGLImageKHR image;
    GLuint tex;
};

struct sfdec_dovi_gl {
    dovi_syms s;
    ANativeWindow *window;
    int width, height;
    bool sdr; // tone map to SDR BT.709 (display without HDR)
    AImageReader *reader;
    ANativeWindow *reader_window;

    pthread_t thread;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    bool stop;
    int flush_gen; // bumped by sfdec_dovi_gl_flush to cut a presentation wait
    int pending;      // images signalled by the reader, not yet acquired
    int init_state;   // 0 running init, 1 ready, -1 failed

    param_slot params[PARAM_SLOTS];
    int param_next;
    VIDEO_DOVI_RESHAPE last;
    bool have_last;
    queue_slot queue[QUEUE_SLOTS];
    int queue_len;

    // GL thread only.
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surf;
    GLuint prog;
    GLint u_code_scale, u_sample_bias, u_pivot_lo, u_pivot_hi, u_pivots, u_coeffs, u_mmr;
    GLint u_ycc_to_rgb, u_ycc_offset, u_lms_to_rgb, u_tex, u_crop, u_rgb_to_ycc, u_sdr_scale, u_sdr_peak;
    bool rgb_sampling; // no GL_EXT_YUV_target: undo the driver's YUV to RGB
    bool smpte2086, cta861_3; // surface HDR metadata extensions (PQ output)
    float meta_max_nits, meta_min_nits; // last metadata set on the surface
    int meta_cll, meta_fall;
    image_entry shown_import; // EGLImage of the shown frame, released with it
    AImage *shown;    // kept until the next frame is presented
    held_frame held[HELD_FRAMES_MAX]; // acquired frames waiting for their time, FIFO
    int held_max; // frames held at most, from the frame rate
    int overflowed; // frames presented early because all were held
    int held_head, held_len;
    int frames, misses, dropped;
    int stale; // output frames without parameters for their pts (previous reused)
    // Timing over the current log period: frames queued after their
    // presentation time, minimum lead and worst draw + swap duration.
    int late;
    int64_t min_lead_ns, max_lead_ns, max_swap_ns;
};

static int api_level(void)
{
    char value[PROP_VALUE_MAX] = {0};
    return __system_property_get("ro.build.version.sdk", value) > 0 ? atoi(value) : 0;
}

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static bool has_ext(const char *list, const char *ext)
{
    size_t n = strlen(ext);
    for (const char *p = list; p && (p = strstr(p, ext)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0'))
            return true;
    return false;
}

template <typename T> static bool sym(void *lib, const char *name, T *out)
{
    *out = lib ? (T)dlsym(lib, name) : NULL;
    return *out != NULL;
}

static bool load_syms(dovi_syms *s)
{
    void *media = dlopen("libmediandk.so", RTLD_NOW);
    void *android = dlopen("libandroid.so", RTLD_NOW);
    return sym(media, "AImageReader_newWithUsage", &s->reader_new) &&
        sym(media, "AImageReader_getWindow", &s->reader_window) &&
        sym(media, "AImageReader_setImageListener", &s->reader_listener) &&
        sym(media, "AImageReader_acquireNextImage", &s->reader_acquire) &&
        sym(media, "AImageReader_delete", &s->reader_delete) &&
        sym(media, "AImage_getHardwareBuffer", &s->image_buffer) &&
        sym(media, "AImage_getTimestamp", &s->image_timestamp) &&
        sym(media, "AImage_getCropRect", &s->image_crop) &&
        sym(media, "AImage_delete", &s->image_delete) &&
        sym(android, "AHardwareBuffer_describe", &s->buffer_describe);
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        LOG("shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// Maps the quad to the visible crop of the decoder buffer; image top at the
// top of the window.
static const char vertex_shader[] =
    "#version 300 es\n"
    "in vec2 pos;\n"
    "uniform vec4 u_crop;\n" // left, top, width, height in buffer uv
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = u_crop.xy + vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5) * u_crop.zw;\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static bool egl_init(sfdec_dovi_gl *gl)
{
    dovi_syms *s = &gl->s;
    gl->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (gl->dpy == EGL_NO_DISPLAY || !eglInitialize(gl->dpy, NULL, NULL))
        return false;
    const char *egl_ext = eglQueryString(gl->dpy, EGL_EXTENSIONS);
    // Without a BT.2020 PQ window surface an HDR display still gets correct
    // colors, tone mapped to SDR (direct rendering would show them wrong).
    if (!gl->sdr && (!has_ext(egl_ext, "EGL_KHR_gl_colorspace") ||
                     !has_ext(egl_ext, "EGL_EXT_gl_colorspace_bt2020_pq"))) {
        LOG("no BT.2020 PQ color space, tone mapping to SDR");
        gl->sdr = true;
    }
    if (!has_ext(egl_ext, "EGL_ANDROID_get_native_client_buffer") ||
        !has_ext(egl_ext, "EGL_ANDROID_image_native_buffer") || !has_ext(egl_ext, "EGL_ANDROID_presentation_time")) {
        LOG("missing EGL extensions");
        return false;
    }
    s->client_buffer = (EGLClientBuffer (*)(const AHardwareBuffer *))eglGetProcAddress("eglGetNativeClientBufferANDROID");
    s->create_image = (EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *))
        eglGetProcAddress("eglCreateImageKHR");
    s->destroy_image = (EGLBoolean (*)(EGLDisplay, EGLImageKHR))eglGetProcAddress("eglDestroyImageKHR");
    s->image_target = (void (*)(GLenum, GLeglImageOES))eglGetProcAddress("glEGLImageTargetTexture2DOES");
    s->presentation_time = (EGLBoolean (*)(EGLDisplay, EGLSurface, EGLnsecsANDROID))
        eglGetProcAddress("eglPresentationTimeANDROID");
    if (!s->client_buffer || !s->create_image || !s->destroy_image || !s->image_target || !s->presentation_time)
        return false;

    // BT.2020 PQ window surface: RGBA 10:10:10:2, else FP16 (NVIDIA Tegra has
    // no 10-bit config), else 8-bit (visible banding, still correct colors).
    // SDR output: default (sRGB) color space, FP16 first. On NVIDIA Tegra
    // with an SDR display an FP16 layer tagged BT.2020 PQ or scRGB linear
    // shows black, and the RGBA8888 surface showed nothing either.
    // eglChooseConfig sizes are minimums, so check the exact red/alpha sizes.
    struct { EGLint red, alpha; bool fp; const char *name; } formats[] = {
        { 10, 2, false, "RGBA1010102" }, { 16, 16, true, "RGBA16F" }, { 8, 8, false, "RGBA8888" } };
    const bool float_ext = has_ext(egl_ext, "EGL_EXT_pixel_format_float");
    const char *format = NULL;
    static const int pq_order[] = { 0, 1, 2 }, sdr_order[] = { 1, 0, 2 };
    // PQ formats first, then the SDR ones when no PQ surface could be created.
    for (size_t i = 0; i < 6 && !format; i++) {
        if (i == 3) {
            if (gl->sdr)
                break;
            LOG("no BT.2020 PQ window surface, tone mapping to SDR");
            gl->sdr = true;
        }
        size_t f = gl->sdr ? sdr_order[i % 3] : pq_order[i % 3];
        if (formats[f].fp && !float_ext)
            continue;
        EGLint attribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, formats[f].red, EGL_GREEN_SIZE, formats[f].red, EGL_BLUE_SIZE, formats[f].red,
            EGL_ALPHA_SIZE, formats[f].alpha,
            // Last so it can be cut when the extension (and attribute) is unknown.
            EGL_COLOR_COMPONENT_TYPE_EXT, formats[f].fp ? EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT : EGL_DONT_CARE,
            EGL_NONE };
        if (!float_ext)
            attribs[12] = EGL_NONE;
        EGLConfig configs[32], config = NULL;
        EGLint n = 0;
        eglChooseConfig(gl->dpy, attribs, configs, 32, &n);
        for (int i = 0; i < n && !config; i++) {
            EGLint r = 0, a = 0;
            eglGetConfigAttrib(gl->dpy, configs[i], EGL_RED_SIZE, &r);
            eglGetConfigAttrib(gl->dpy, configs[i], EGL_ALPHA_SIZE, &a);
            if (r == formats[f].red && a == formats[f].alpha)
                config = configs[i];
        }
        if (!config)
            continue;
        // Scale the video-sized buffers to the view like MediaCodec output.
        ANativeWindow_setBuffersGeometry(gl->window, gl->width, gl->height, 0);
        const EGLint surf_attribs[] = { EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_BT2020_PQ_EXT, EGL_NONE };
        gl->surf = eglCreateWindowSurface(gl->dpy, config, gl->window, gl->sdr ? NULL : surf_attribs);
        if (gl->surf == EGL_NO_SURFACE) {
            LOG("%s window surface failed 0x%x", formats[f].name, eglGetError());
            continue;
        }
        const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        gl->ctx = eglCreateContext(gl->dpy, config, EGL_NO_CONTEXT, ctx_attribs);
        if (gl->ctx == EGL_NO_CONTEXT || !eglMakeCurrent(gl->dpy, gl->surf, gl->surf, gl->ctx))
            return false;
        format = formats[f].name;
    }
    if (!format) {
        LOG("no %s window surface", gl->sdr ? "SDR" : "BT.2020 PQ");
        return false;
    }
    gl->rgb_sampling = !has_ext((const char *)glGetString(GL_EXTENSIONS), "GL_EXT_YUV_target");
    gl->smpte2086 = !gl->sdr && has_ext(egl_ext, "EGL_EXT_surface_SMPTE2086_metadata");
    gl->cta861_3 = !gl->sdr && has_ext(egl_ext, "EGL_EXT_surface_CTA861_3_metadata");

    GLuint vs = compile(GL_VERTEX_SHADER, vertex_shader);
    GLuint fs = compile(GL_FRAGMENT_SHADER, gl->rgb_sampling ? video_dovi_reshape_rgb_fs : video_dovi_reshape_fs);
    if (!vs || !fs)
        return false;
    gl->prog = glCreateProgram();
    glAttachShader(gl->prog, vs);
    glAttachShader(gl->prog, fs);
    glBindAttribLocation(gl->prog, 0, "pos");
    glLinkProgram(gl->prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(gl->prog, GL_LINK_STATUS, &ok);
    if (!ok)
        return false;
    glUseProgram(gl->prog);
#define LOC(name) gl->name = glGetUniformLocation(gl->prog, #name)
    LOC(u_code_scale); LOC(u_sample_bias); LOC(u_pivot_lo); LOC(u_pivot_hi); LOC(u_pivots);
    LOC(u_coeffs); LOC(u_mmr); LOC(u_ycc_to_rgb); LOC(u_ycc_offset); LOC(u_lms_to_rgb);
    LOC(u_tex); LOC(u_crop); LOC(u_rgb_to_ycc); LOC(u_sdr_scale); LOC(u_sdr_peak);
#undef LOC
    glUniform1i(gl->u_tex, 0);
    static const GLfloat bias[3] = { 0, 0, 0 };
    glUniform3fv(gl->u_sample_bias, 1, bias);
    // Profile 5 is full range without matrix coefficients; drivers then
    // convert with BT.601 full range (measured exact on NVIDIA Tegra X1).
    static const GLfloat bt601_full_rgb_to_ycc[9] = {
        0.299f, 0.587f, 0.114f,
        -0.168736f, -0.331264f, 0.5f,
        0.5f, -0.418688f, -0.081312f };
    if (gl->rgb_sampling)
        glUniformMatrix3fv(gl->u_rgb_to_ycc, 1, GL_TRUE, bt601_full_rgb_to_ycc);
    // SDR: 203 nits reference white (ITU-R BT.2408) maps to 1.0.
    glUniform1f(gl->u_sdr_scale, gl->sdr ? 10000.0f / SDR_WHITE_NITS : 0.0f);
    LOG("ready on %s: %s %s output, %s sampling, %d held frames", (const char *)glGetString(GL_RENDERER),
        format, gl->sdr ? "SDR" : "PQ", gl->rgb_sampling ? "RGB (BT.601 full range inverted)" : "raw YUV",
        gl->held_max);
    return true;
}

static void release_import(sfdec_dovi_gl *gl, image_entry *e);

static void egl_release(sfdec_dovi_gl *gl)
{
    if (gl->dpy == EGL_NO_DISPLAY)
        return;
    release_import(gl, &gl->shown_import);
    if (gl->prog)
        glDeleteProgram(gl->prog);
    eglMakeCurrent(gl->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    // Destroying the surface disconnects the window for the next producer.
    if (gl->surf != EGL_NO_SURFACE)
        eglDestroySurface(gl->dpy, gl->surf);
    if (gl->ctx != EGL_NO_CONTEXT)
        eglDestroyContext(gl->dpy, gl->ctx);
    eglReleaseThread();
}

// Import the frame's buffer for each draw rather than caching EGLImages per
// buffer: on NVIDIA Tegra a cached import stayed black from the start of the
// file until a seek, and the codec's buffer count is not bounded.
static bool import_frame(sfdec_dovi_gl *gl, AHardwareBuffer *buffer, image_entry *out)
{
    const EGLint attribs[] = { EGL_NONE };
    EGLImageKHR image = gl->s.create_image(gl->dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
        gl->s.client_buffer(buffer), attribs);
    if (image == EGL_NO_IMAGE_KHR)
        return false;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->s.image_target(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)image);
    out->buffer = buffer;
    out->image = image;
    out->tex = tex;
    return true;
}

static void release_import(sfdec_dovi_gl *gl, image_entry *e)
{
    if (!e->tex)
        return;
    glDeleteTextures(1, &e->tex);
    gl->s.destroy_image(gl->dpy, e->image);
    e->tex = 0;
}

static void upload(sfdec_dovi_gl *gl, const VIDEO_DOVI_RESHAPE *p)
{
    // P010 through GL_EXT_YUV_target: 16-bit container normalized to 1.0.
    // Driver RGB conversion normalizes 10-bit codes by 1024.
    glUniform1f(gl->u_code_scale, (gl->rgb_sampling ? 1024.0f : 65535.0f / 64.0f) / (float)(1 << p->bl_bit_depth));
    glUniform3fv(gl->u_pivot_lo, 1, p->pivot_lo);
    glUniform3fv(gl->u_pivot_hi, 1, p->pivot_hi);
    glUniform1fv(gl->u_pivots, 3 * (VIDEO_DOVI_RESHAPE_PIECES - 1), &p->pivots[0][0]);
    glUniform4fv(gl->u_coeffs, 3 * VIDEO_DOVI_RESHAPE_PIECES, &p->coeffs[0][0][0]);
    glUniform4fv(gl->u_mmr, 3 * VIDEO_DOVI_RESHAPE_MMR_ROWS, &p->mmr[0][0][0]);
    glUniformMatrix3fv(gl->u_ycc_to_rgb, 1, GL_TRUE, p->ycc_to_rgb);
    glUniform3fv(gl->u_ycc_offset, 1, p->ycc_offset);
    glUniformMatrix3fv(gl->u_lms_to_rgb, 1, GL_TRUE, p->lms_to_rgb);
    if (gl->sdr) {
        // Tone map up to the source peak from the RPU (1000 nits if absent).
        float e = powf(p->source_max_pq > 0.0f ? p->source_max_pq : 0.7518f, 1.0f / 78.84375f);
        float nits = 10000.0f * powf(fmaxf(e - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * e), 1.0f / 0.1593017578125f);
        glUniform1f(gl->u_sdr_peak, fmaxf(nits / SDR_WHITE_NITS, 1.0f));
    }
}

// Take a frame from the reader right away, with its parameters, so that the
// codec never waits on the reader: avos releases frames ~200 ms before their
// presentation time and drops frames when the codec stalls.
static void acquire_one(sfdec_dovi_gl *gl)
{
    dovi_syms *s = &gl->s;
    AImage *image = NULL;
    if (s->reader_acquire(gl->reader, &image) != AMEDIA_OK || !image)
        return;
    held_frame *f = &gl->held[(gl->held_head + gl->held_len) % gl->held_max];
    f->image = image;
    f->ts = 0;
    f->present_ns = 0;
    f->have = false;
    f->flush_gen = __atomic_load_n(&gl->flush_gen, __ATOMIC_RELAXED);
    s->image_timestamp(image, &f->ts);

    pthread_mutex_lock(&gl->mtx);
    for (int i = 0; i < gl->queue_len; i++) {
        if (gl->queue[i].image_ts_ns != f->ts)
            continue;
        f->params = gl->queue[i].params;
        f->present_ns = gl->queue[i].present_ns;
        f->have = true;
        // Older entries belong to frames the codec never delivered.
        memmove(gl->queue, gl->queue + i + 1, (gl->queue_len - i - 1) * sizeof(gl->queue[0]));
        gl->queue_len -= i + 1;
        break;
    }
    if (!f->have && gl->have_last) {
        f->params = gl->last;
        f->have = true;
        gl->misses++;
    }
    pthread_mutex_unlock(&gl->mtx);
    gl->held_len++;
}

// Draw the oldest held frame and queue it to the window with its
// presentation time. Called shortly before that time: queueing ~200 ms early
// fills the window's few buffers and blocks eglSwapBuffers.
// Static HDR metadata for the display's own tone mapping of the PQ output,
// set before the swap of the first frame carrying new values.
static void set_hdr_metadata(sfdec_dovi_gl *gl, const VIDEO_DOVI_RESHAPE *p)
{
    if (p->mastering_max_nits == gl->meta_max_nits && p->mastering_min_nits == gl->meta_min_nits &&
        p->max_cll == gl->meta_cll && p->max_fall == gl->meta_fall)
        return;
    gl->meta_max_nits = p->mastering_max_nits;
    gl->meta_min_nits = p->mastering_min_nits;
    gl->meta_cll = p->max_cll;
    gl->meta_fall = p->max_fall;
    bool ok = true;
    if (gl->smpte2086) {
        // Dolby's default mastering primaries without an L9 block: P3-D65.
        static const struct { EGLint attr; float value; } prim[] = {
            { EGL_SMPTE2086_DISPLAY_PRIMARY_RX_EXT, 0.680f }, { EGL_SMPTE2086_DISPLAY_PRIMARY_RY_EXT, 0.320f },
            { EGL_SMPTE2086_DISPLAY_PRIMARY_GX_EXT, 0.265f }, { EGL_SMPTE2086_DISPLAY_PRIMARY_GY_EXT, 0.690f },
            { EGL_SMPTE2086_DISPLAY_PRIMARY_BX_EXT, 0.150f }, { EGL_SMPTE2086_DISPLAY_PRIMARY_BY_EXT, 0.060f },
            { EGL_SMPTE2086_WHITE_POINT_X_EXT, 0.3127f }, { EGL_SMPTE2086_WHITE_POINT_Y_EXT, 0.3290f },
        };
        for (size_t i = 0; i < sizeof(prim) / sizeof(prim[0]); i++)
            ok &= eglSurfaceAttrib(gl->dpy, gl->surf, prim[i].attr,
                                   (EGLint)lroundf(prim[i].value * EGL_METADATA_SCALING_EXT)) == EGL_TRUE;
        ok &= eglSurfaceAttrib(gl->dpy, gl->surf, EGL_SMPTE2086_MAX_LUMINANCE_EXT,
                               (EGLint)lroundf(p->mastering_max_nits * EGL_METADATA_SCALING_EXT)) == EGL_TRUE;
        ok &= eglSurfaceAttrib(gl->dpy, gl->surf, EGL_SMPTE2086_MIN_LUMINANCE_EXT,
                               (EGLint)lroundf(p->mastering_min_nits * EGL_METADATA_SCALING_EXT)) == EGL_TRUE;
    }
    if (gl->cta861_3 && p->max_cll > 0) {
        ok &= eglSurfaceAttrib(gl->dpy, gl->surf, EGL_CTA861_3_MAX_CONTENT_LIGHT_LEVEL_EXT,
                               p->max_cll * EGL_METADATA_SCALING_EXT) == EGL_TRUE;
        ok &= eglSurfaceAttrib(gl->dpy, gl->surf, EGL_CTA861_3_MAX_FRAME_AVERAGE_LEVEL_EXT,
                               p->max_fall * EGL_METADATA_SCALING_EXT) == EGL_TRUE;
    }
    LOG("HDR metadata: mastering %.4f..%.0f nits, MaxCLL %d, MaxFALL %d%s", p->mastering_min_nits,
        p->mastering_max_nits, p->max_cll, p->max_fall, ok ? "" : " (rejected)");
}

static void draw_held(sfdec_dovi_gl *gl)
{
    dovi_syms *s = &gl->s;
    held_frame *f = &gl->held[gl->held_head];
    gl->held_head = (gl->held_head + 1) % gl->held_max;
    gl->held_len--;
    AImage *image = f->image;
    int64_t present_ns = f->present_ns;
    if (f->flush_gen != __atomic_load_n(&gl->flush_gen, __ATOMIC_RELAXED)) {
        s->image_delete(image);
        return;
    }

    AHardwareBuffer *buffer = NULL;
    image_entry import = { NULL, EGL_NO_IMAGE_KHR, 0 };
    if (f->have && s->image_buffer(image, &buffer) == AMEDIA_OK && buffer)
        import_frame(gl, buffer, &import);
    GLuint tex = import.tex;
    if (!tex) {
        if (gl->dropped++ == 0)
            LOG("dropping frame ts=%lld: %s", (long long)f->ts, f->have ? "texture import failed" : "no RPU parameters yet");
        s->image_delete(image);
        return;
    }
    AHardwareBuffer_Desc desc;
    s->buffer_describe(buffer, &desc);
    const AImageCropRect full = { 0, 0, (int32_t)desc.width, (int32_t)desc.height };
    AImageCropRect crop = full;
    if (s->image_crop(image, &crop) != AMEDIA_OK || crop.right <= crop.left || crop.bottom <= crop.top)
        crop = full;
    if (gl->frames == 0)
        LOG("first frame: buffer %ux%u format=0x%x crop=%d,%d-%d,%d", desc.width, desc.height, desc.format,
            crop.left, crop.top, crop.right, crop.bottom);

    EGLint sw = 0, sh = 0;
    eglQuerySurface(gl->dpy, gl->surf, EGL_WIDTH, &sw);
    eglQuerySurface(gl->dpy, gl->surf, EGL_HEIGHT, &sh);
    glViewport(0, 0, sw, sh);
    glUseProgram(gl->prog);
    upload(gl, &f->params);
    if (gl->smpte2086 || gl->cta861_3)
        set_hdr_metadata(gl, &f->params);
    glUniform4f(gl->u_crop, (float)crop.left / desc.width, (float)crop.top / desc.height,
        (float)(crop.right - crop.left) / desc.width, (float)(crop.bottom - crop.top) / desc.height);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    int64_t start_ns = mono_ns();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (present_ns > 0) {
        s->presentation_time(gl->dpy, gl->surf, present_ns);
        int64_t lead = present_ns - start_ns;
        if (lead < 0)
            gl->late++;
        if (gl->frames % 240 == 0 || lead < gl->min_lead_ns)
            gl->min_lead_ns = lead;
        if (gl->frames % 240 == 0 || lead > gl->max_lead_ns)
            gl->max_lead_ns = lead;
    }
    if (!eglSwapBuffers(gl->dpy, gl->surf))
        LOG("eglSwapBuffers failed 0x%x", eglGetError());
    int64_t swap_ns = mono_ns() - start_ns;
    if (gl->frames % 240 == 0 || swap_ns > gl->max_swap_ns)
        gl->max_swap_ns = swap_ns;
    // The previous image is no longer read once this frame is queued.
    release_import(gl, &gl->shown_import);
    if (gl->shown)
        s->image_delete(gl->shown);
    gl->shown = image;
    gl->shown_import = import;
    if (++gl->frames % 240 == 0) {
        LOG("frames=%d stale=%d unmatched=%d dropped=%d late=%d lead=%.1f..%.1f ms max_draw_swap=%.1f ms", gl->frames,
            gl->stale, gl->misses, gl->dropped, gl->late, gl->min_lead_ns / 1e6, gl->max_lead_ns / 1e6, gl->max_swap_ns / 1e6);
        gl->late = 0;
    }
}

static void on_image(void *context, AImageReader *)
{
    sfdec_dovi_gl *gl = (sfdec_dovi_gl *)context;
    pthread_mutex_lock(&gl->mtx);
    gl->pending++;
    pthread_cond_broadcast(&gl->cond);
    pthread_mutex_unlock(&gl->mtx);
}

static void *gl_thread(void *arg)
{
    sfdec_dovi_gl *gl = (sfdec_dovi_gl *)arg;
    bool ok = egl_init(gl);
    pthread_mutex_lock(&gl->mtx);
    gl->init_state = ok ? 1 : -1;
    pthread_cond_broadcast(&gl->cond);
    pthread_mutex_unlock(&gl->mtx);
    while (ok) {
        pthread_mutex_lock(&gl->mtx);
        while (!gl->stop && !gl->pending && !gl->held_len)
            pthread_cond_wait(&gl->cond, &gl->mtx);
        bool stop = gl->stop;
        int pending = gl->pending;
        gl->pending = 0;
        pthread_mutex_unlock(&gl->mtx);
        if (stop)
            break;
        for (int i = 0; i < pending; i++) {
            if (gl->held_len == gl->held_max) {
                // Full: present the oldest now. Frames arrive more often than
                // the declared frame rate, or earlier than expected.
                if (!gl->overflowed++)
                    LOG("%d held frames are not enough, presenting early", gl->held_max);
                draw_held(gl);
            }
            acquire_one(gl);
        }
        int64_t wait_ns = 0;
        while (gl->held_len) {
            const held_frame *f = &gl->held[gl->held_head];
            wait_ns = f->present_ns - PRESENT_LEAD_NS - mono_ns();
            if (f->present_ns > 0 && wait_ns > 0 &&
                f->flush_gen == __atomic_load_n(&gl->flush_gen, __ATOMIC_RELAXED))
                break;
            draw_held(gl);
        }
        // Sleep in short steps so new frames are taken from the reader promptly.
        if (gl->held_len && !__atomic_load_n(&gl->pending, __ATOMIC_RELAXED))
            usleep((useconds_t)(wait_ns > 4000000 ? 4000 : wait_ns / 1000 + 1));
    }
    while (gl->held_len) {
        gl->s.image_delete(gl->held[gl->held_head].image);
        gl->held_head = (gl->held_head + 1) % gl->held_max;
        gl->held_len--;
    }
    release_import(gl, &gl->shown_import);
    if (gl->shown)
        gl->s.image_delete(gl->shown);
    gl->shown = NULL;
    egl_release(gl);
    return NULL;
}

sfdec_dovi_gl_t *sfdec_dovi_gl_create(ANativeWindow *window, int width, int height, bool sdr,
                                      int64_t frame_ns)
{
    if (!window || width <= 0 || height <= 0 || api_level() < 26)
        return NULL;
    sfdec_dovi_gl *gl = (sfdec_dovi_gl *)calloc(1, sizeof(*gl));
    if (!gl)
        return NULL;
    gl->window = window;
    gl->width = width;
    gl->height = height;
    gl->sdr = sdr;
    // One more than the frames within the release lead, unknown rate: maximum.
    gl->held_max = frame_ns > 0 ? (int)((RELEASE_LEAD_NS + frame_ns - 1) / frame_ns) + 1 : HELD_FRAMES_MAX;
    if (gl->held_max < HELD_FRAMES_MIN)
        gl->held_max = HELD_FRAMES_MIN;
    if (gl->held_max > HELD_FRAMES_MAX)
        gl->held_max = HELD_FRAMES_MAX;
    gl->dpy = EGL_NO_DISPLAY;
    gl->ctx = EGL_NO_CONTEXT;
    gl->surf = EGL_NO_SURFACE;
    if (!load_syms(&gl->s)) {
        LOG("AImageReader/AHardwareBuffer API unavailable");
        free(gl);
        return NULL;
    }
    // GPU-sampled private buffers: the decoder picks its native 10-bit layout.
    if (gl->s.reader_new(width, height, AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
                         gl->held_max + 2, &gl->reader) != AMEDIA_OK ||
        gl->s.reader_window(gl->reader, &gl->reader_window) != AMEDIA_OK) {
        LOG("AImageReader creation failed");
        if (gl->reader)
            gl->s.reader_delete(gl->reader);
        free(gl);
        return NULL;
    }
    pthread_mutex_init(&gl->mtx, NULL);
    pthread_cond_init(&gl->cond, NULL);
    AImageReader_ImageListener listener = { gl, on_image };
    gl->s.reader_listener(gl->reader, &listener);
    if (pthread_create(&gl->thread, NULL, gl_thread, gl)) {
        gl->s.reader_delete(gl->reader);
        free(gl);
        return NULL;
    }
    pthread_mutex_lock(&gl->mtx);
    while (!gl->init_state)
        pthread_cond_wait(&gl->cond, &gl->mtx);
    int state = gl->init_state;
    pthread_mutex_unlock(&gl->mtx);
    if (state < 0) {
        sfdec_dovi_gl_destroy(gl);
        return NULL;
    }
    return gl;
}

void sfdec_dovi_gl_destroy(sfdec_dovi_gl_t *gl)
{
    if (!gl)
        return;
    pthread_mutex_lock(&gl->mtx);
    gl->stop = true;
    pthread_cond_broadcast(&gl->cond);
    pthread_mutex_unlock(&gl->mtx);
    pthread_join(gl->thread, NULL);
    AImageReader_ImageListener none = { NULL, NULL };
    gl->s.reader_listener(gl->reader, &none);
    gl->s.reader_delete(gl->reader);
    pthread_cond_destroy(&gl->cond);
    pthread_mutex_destroy(&gl->mtx);
    LOG("closed after %d frames (%d without matching RPU, %d dropped, %d presented early)", gl->frames,
        gl->misses, gl->dropped, gl->overflowed);
    free(gl);
}

ANativeWindow *sfdec_dovi_gl_codec_window(sfdec_dovi_gl_t *gl)
{
    return gl->reader_window;
}

void sfdec_dovi_gl_set_params(sfdec_dovi_gl_t *gl, int64_t pts_us, const VIDEO_DOVI_RESHAPE *params)
{
    pthread_mutex_lock(&gl->mtx);
    // The caller retries an access unit while the codec input is full: reuse
    // its slot, or the retries evict the parameters of frames in flight.
    param_slot *slot = NULL;
    for (int i = 0; i < PARAM_SLOTS && !slot; i++)
        if (gl->params[i].valid && gl->params[i].pts_us == pts_us)
            slot = &gl->params[i];
    if (!slot) {
        slot = &gl->params[gl->param_next];
        gl->param_next = (gl->param_next + 1) % PARAM_SLOTS;
    }
    slot->pts_us = pts_us;
    slot->valid = true;
    slot->params = *params;
    pthread_mutex_unlock(&gl->mtx);
}

void sfdec_dovi_gl_queue(sfdec_dovi_gl_t *gl, int64_t pts_us, int64_t image_ts_ns, int64_t present_ns)
{
    pthread_mutex_lock(&gl->mtx);
    const VIDEO_DOVI_RESHAPE *params = NULL;
    for (int i = 0; i < PARAM_SLOTS && !params; i++)
        if (gl->params[i].valid && gl->params[i].pts_us == pts_us)
            params = &gl->params[i].params;
    if (params) {
        gl->last = *params;
        gl->have_last = true;
    } else if (gl->stale++ == 0) {
        int64_t newest = 0;
        for (int i = 0; i < PARAM_SLOTS; i++)
            if (gl->params[i].valid && gl->params[i].pts_us > newest)
                newest = gl->params[i].pts_us;
        LOG("no parameters for output pts %lld (newest input %lld), reusing the previous ones",
            (long long)pts_us, (long long)newest);
    }
    if (gl->have_last) {
        if (gl->queue_len == QUEUE_SLOTS) {
            memmove(gl->queue, gl->queue + 1, (QUEUE_SLOTS - 1) * sizeof(gl->queue[0]));
            gl->queue_len--;
        }
        queue_slot *q = &gl->queue[gl->queue_len++];
        q->image_ts_ns = image_ts_ns;
        q->present_ns = present_ns;
        q->params = gl->last;
    }
    pthread_mutex_unlock(&gl->mtx);
}

void sfdec_dovi_gl_flush(sfdec_dovi_gl_t *gl)
{
    pthread_mutex_lock(&gl->mtx);
    for (int i = 0; i < PARAM_SLOTS; i++)
        gl->params[i].valid = false;
    gl->queue_len = 0;
    // Never show frames of the new position with the old position's colors.
    gl->have_last = false;
    __atomic_add_fetch(&gl->flush_gen, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&gl->mtx);
}

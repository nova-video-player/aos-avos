/*
 * Copyright 2026 Courville Software
 * SPDX-License-Identifier: Apache-2.0
 *
 * Frame-preserving binaural rendering: abuffer(FLT) -> sofalizer(type=time)
 * -> aformat(FLT stereo) -> abuffersink. Resample the HRIR once, never PCM.
 * There is no output FIFO and no change to the speed filter's frame indices.
 * Convolution history survives pause; seek discards it. Like FFmpeg's
 * sofalizer, output is cropped to the input duration (no extra reverb tail).
 */
#include "global.h"
#include "stream_filter_audio.h"
#include "audio_interface.h"
#include "debug.h"
#include "astdlib.h"
#include <pthread.h>
#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include "sofa_tv_fir.h"

struct sofa_ctx {
    pthread_mutex_t lock;
    int mode, rate, channels, layout_mask, requested_mask;
    char path[AUDIO_SOFA_PATH_MAX];
    AVFilterGraph *graph;
    AVFilterContext *source, *sink;
    AVChannelLayout layout;
    AVFrame *in, *out;
    int16_t *pcm;
    unsigned int pcm_capacity;
    int64_t pts;
    int diag_frames, diag_clipped, diag_nonfinite;
    float diag_peak;
    /* Four-path, regularized generic speaker-to-ear inverse. Coefficients
     * are generated from KU100 measurements for 1 m spacing / 2 m distance.
     * This is NOT a measurement of the user's TV or room. */
    float *tv_coeff, *tv_history;
    int tv_taps, tv_pos;
};

static void reset_graph(struct sofa_ctx *c)
{
    avfilter_graph_free(&c->graph);
    c->source = c->sink = NULL;
    av_channel_layout_uninit(&c->layout);
    c->pts = 0;
    c->diag_frames = c->diag_clipped = c->diag_nonfinite = 0;
    c->diag_peak = 0;
    av_freep(&c->tv_coeff);
    av_freep(&c->tv_history);
    c->tv_taps = c->tv_pos = 0;
}

static int configure_tv(struct sofa_ctx *c)
{
    if (c->mode != AUDIO_SOFA_TV) return 0;
    SwrContext *swr = NULL;
    AVChannelLayout quad = AV_CHANNEL_LAYOUT_QUAD;
    int capacity = (SOFA_TV_FIR_TAPS + 64) * c->rate / SOFA_TV_FIR_RATE + 64;
    c->tv_coeff = av_calloc(capacity, 4 * sizeof(float));
    if (!c->tv_coeff) return AVERROR(ENOMEM);
    int ret = swr_alloc_set_opts2(&swr, &quad, AV_SAMPLE_FMT_FLT, c->rate,
                                  &quad, AV_SAMPLE_FMT_FLT, SOFA_TV_FIR_RATE, 0, NULL);
    if (ret < 0 || (ret = swr_init(swr)) < 0) goto done;
    const uint8_t *input[] = { (const uint8_t *)sofa_tv_fir };
    uint8_t *output[] = { (uint8_t *)c->tv_coeff };
    ret = swr_convert(swr, output, capacity, input, SOFA_TV_FIR_TAPS);
    if (ret < 0) goto done;
    c->tv_taps = ret;
    do {
        output[0] = (uint8_t *)(c->tv_coeff + c->tv_taps * 4);
        ret = swr_convert(swr, output, capacity - c->tv_taps, NULL, 0);
        if (ret < 0) goto done;
        c->tv_taps += ret;
    } while (ret > 0 && c->tv_taps < capacity);
    if (!c->tv_taps || c->tv_taps == capacity) { ret = AVERROR(EINVAL); goto done; }
    // Resampling an impulse response changes the number of coefficients.
    // Correct their integral; do not change the programme's rate or ITDs.
    for (int i = 0; i < c->tv_taps * 4; i++)
        c->tv_coeff[i] *= (float)SOFA_TV_FIR_RATE / c->rate;
    c->tv_history = av_calloc(c->tv_taps * 2, sizeof(float));
    if (!c->tv_history) ret = AVERROR(ENOMEM);
done:
    swr_free(&swr);
    return ret < 0 ? ret : 0;
}

static void filter_tv(struct sofa_ctx *c, float *left, float *right)
{
    c->tv_history[2*c->tv_pos] = *left;
    c->tv_history[2*c->tv_pos+1] = *right;
    float l = 0, r = 0;
    int pos = c->tv_pos;
    for (int j = 0; j < c->tv_taps; j++) {
        const float *k = c->tv_coeff + 4*j;
        float x = c->tv_history[2*pos], y = c->tv_history[2*pos+1];
        l += k[0]*x + k[1]*y;
        r += k[2]*x + k[3]*y;
        if (--pos < 0) pos = c->tv_taps - 1;
    }
    if (++c->tv_pos == c->tv_taps) c->tv_pos = 0;
    *left = isfinite(l) ? l : 0;
    *right = isfinite(r) ? r : 0;
}

static int configure(struct sofa_ctx *c, const AUDIO_FRAME *frame)
{
    AVFilterContext *sofa, *format;
    char args[256], layout[128];
    int ret;
    reset_graph(c);
    c->rate = frame->samplesPerSec;
    c->channels = frame->channels;
    c->layout_mask = c->requested_mask;
    if (c->layout_mask)
        av_channel_layout_from_mask(&c->layout, (uint32_t)c->layout_mask);
    if (c->layout.nb_channels != c->channels) {
        av_channel_layout_uninit(&c->layout);
        av_channel_layout_default(&c->layout, c->channels);
    }
    if (av_channel_layout_describe(&c->layout, layout, sizeof(layout)) < 0)
        return -1;
    c->graph = avfilter_graph_alloc();
    if (!c->graph)
        return -1;
    /* Two ears do not justify a per-frame worker barrier on small blocks. */
    c->graph->nb_threads = 1;
    snprintf(args, sizeof(args), "sample_rate=%d:sample_fmt=flt:channel_layout=%s:time_base=1/%d",
             c->rate, layout, c->rate);
    ret = avfilter_graph_create_filter(&c->source, avfilter_get_by_name("abuffer"),
                                      "input", args, NULL, c->graph);
    if (ret < 0)
        goto fail;
    const AVFilter *filter = avfilter_get_by_name("sofalizer");
    if (!filter) {
        ret = AVERROR_FILTER_NOT_FOUND;
        goto fail;
    }
    sofa = avfilter_graph_alloc_filter(c->graph, filter, "sofa");
    if (!sofa) { ret = AVERROR(ENOMEM); goto fail; }
    /* Set the path directly: filenames are not filter-expression syntax. */
    if ((ret = av_opt_set(sofa->priv, "sofa", c->path, 0)) < 0 ||
        (ret = av_opt_set_int(sofa->priv, "sample_rate", c->rate, 0)) < 0 ||
        (ret = av_opt_set(sofa->priv, "type", "time", 0)) < 0 ||
        (ret = av_opt_set_int(sofa->priv, "normalize", 1, 0)) < 0 ||
        (ret = av_opt_set_int(sofa->priv, "interpolate", 1, 0)) < 0 ||
        (ret = avfilter_init_str(sofa, NULL)) < 0)
        goto fail;
    snprintf(args, sizeof(args), "sample_fmts=flt:sample_rates=%d:channel_layouts=stereo", c->rate);
    if ((ret = avfilter_graph_create_filter(&format, avfilter_get_by_name("aformat"),
                                           "format", args, NULL, c->graph)) < 0 ||
        (ret = avfilter_graph_create_filter(&c->sink, avfilter_get_by_name("abuffersink"),
                                           "output", NULL, NULL, c->graph)) < 0 ||
        (ret = avfilter_link(c->source, 0, sofa, 0)) < 0 ||
        (ret = avfilter_link(sofa, 0, format, 0)) < 0 ||
        (ret = avfilter_link(format, 0, c->sink, 0)) < 0 ||
        (ret = avfilter_graph_config(c->graph, NULL)) < 0)
        goto fail;
    if ((ret = configure_tv(c)) < 0) goto fail;
    serprintf("mysofa: mode=%d input=%dch/%dbit/%dHz mask=0x%x layout=%s -> stereo S16, time domain, tv_taps=%d\n",
              c->mode, c->channels, frame->bits, c->rate, c->requested_mask, layout, c->tv_taps);
    return 0;
fail:
    serprintf("mysofa: graph setup failed (%d); requires FFmpeg sofalizer sample_rate extension\n", ret);
    reset_graph(c);
    return -1;
}

static int sofa_open(STREAM_FILTER_AUDIO *f, AUDIO_PROPERTIES *audio)
{
    struct sofa_ctx *c = f->priv;
    c->requested_mask = audio->channelMask;
    return c->mode && c->path[0] ? 0 : -1;
}

static float input_sample(const unsigned char *p, int bits)
{
    if (bits == 8) return ((int)*p - 128) / 128.0f;
    if (bits == 16) { int16_t x; memcpy(&x, p, 2); return x / 32768.0f; }
    if (bits == 24) {
        int32_t x = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
        return x / 2147483648.0f;
    }
    /* AVOS PCM bits=32 denotes float (same contract as Sonic/atempo). */
    float x; memcpy(&x, p, 4);
    return isfinite(x) ? x : 0.0f;
}

static int16_t pcm_sample(float x)
{
    if (!isfinite(x)) return 0;
    if (x >= 1.0f) return INT16_MAX;
    if (x <= -1.0f) return INT16_MIN;
    return (int16_t)lrintf(x * 32768.0f);
}

static int sofa_filter(STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame)
{
    struct sofa_ctx *c = f->priv;
    if (!frame || frame->size == 0) return 0;
    pthread_mutex_lock(&c->lock);
    int ret = -1;
    if (!frame->data || frame->format != WAVE_FORMAT_PCM || frame->error ||
        frame->channels < 1 || frame->channels > 8 ||
        frame->samplesPerSec < 8000 || frame->samplesPerSec > 192000 ||
        (frame->bits != 8 && frame->bits != 16 && frame->bits != 24 && frame->bits != 32))
        goto done;
    int stride = frame->channels * (frame->bits / 8);
    int samples = frame->size / stride;
    if (!samples || frame->size % stride || samples > INT_MAX / 4) goto done;
    if (!c->graph || c->rate != frame->samplesPerSec || c->channels != frame->channels ||
        c->layout_mask != c->requested_mask) {
        if (configure(c, frame) < 0) goto done;
    }
    av_frame_unref(c->in);
    c->in->format = AV_SAMPLE_FMT_FLT;
    c->in->sample_rate = c->rate;
    c->in->nb_samples = samples;
    c->in->pts = c->pts;
    if (av_channel_layout_copy(&c->in->ch_layout, &c->layout) < 0 || av_frame_get_buffer(c->in, 0) < 0)
        goto done;
    float *input = (float *)c->in->data[0];
    for (int i = 0; i < samples * c->channels; i++)
        input[i] = input_sample(frame->data + (size_t)i * (frame->bits / 8), frame->bits);
    av_fast_malloc(&c->pcm, &c->pcm_capacity, (size_t)samples * 4);
    if (!c->pcm || av_buffersrc_add_frame(c->source, c->in) < 0) goto done;
    int produced = 0, pull;
    while ((pull = av_buffersink_get_frame(c->sink, c->out)) >= 0) {
        if (c->out->format != AV_SAMPLE_FMT_FLT || c->out->ch_layout.nb_channels != 2 ||
            c->out->sample_rate != c->rate || c->out->nb_samples > samples - produced) {
            av_frame_unref(c->out);
            goto done;
        }
        const float *src = (const float *)c->out->data[0];
        for (int i = 0; i < c->out->nb_samples; i++) {
            float l = src[2*i], r = src[2*i+1];
            c->diag_nonfinite += !isfinite(l) + !isfinite(r);
            if (c->mode == AUDIO_SOFA_TV) filter_tv(c, &l, &r);
            float peak = fmaxf(fabsf(l), fabsf(r));
            if (isfinite(peak) && peak > c->diag_peak) c->diag_peak = peak;
            c->diag_clipped += (fabsf(l) >= 1.0f) + (fabsf(r) >= 1.0f);
            c->pcm[2*(produced+i)] = pcm_sample(l);
            c->pcm[2*(produced+i)+1] = pcm_sample(r);
        }
        produced += c->out->nb_samples;
        av_frame_unref(c->out);
    }
    if (pull != AVERROR(EAGAIN) || produced != samples) {
        serprintf("mysofa: frame conservation failed: in=%d out=%d error=%d\n", samples, produced, pull);
        goto done;
    }
    c->pts += samples;
    c->diag_frames += samples;
    // One summary per five seconds of PCM, plus the first block. Avoid
    // per-frame logging on TV devices while retaining evidence of clipping.
    if (c->pts == samples || c->diag_frames >= c->rate * 5) {
        serprintf("mysofa_pcm: mode=%d rate=%d frames=%d out_peak=%.5f clipped=%d nonfinite=%d\n",
                  c->mode, c->rate, c->diag_frames, c->diag_peak,
                  c->diag_clipped, c->diag_nonfinite);
        c->diag_frames = c->diag_clipped = c->diag_nonfinite = 0;
        c->diag_peak = 0;
    }
    frame->data = (unsigned char *)c->pcm;
    frame->size = samples * 4;
    frame->fakeSize = 0;
    frame->channels = 2;
    frame->bits = 16;
    ret = 0;
done:
    if (ret < 0) {
        frame->data = NULL;
        frame->size = frame->fakeSize = 0;
        frame->error = 1;
        reset_graph(c);
    }
    pthread_mutex_unlock(&c->lock);
    return ret;
}

static int sofa_flush(STREAM_FILTER_AUDIO *f)
{
    struct sofa_ctx *c = f->priv;
    pthread_mutex_lock(&c->lock);
    reset_graph(c);
    pthread_mutex_unlock(&c->lock);
    return 0;
}

static int sofa_layout(STREAM_FILTER_AUDIO *f, void *mask, void *unused)
{
    struct sofa_ctx *c = f->priv;
    pthread_mutex_lock(&c->lock);
    c->requested_mask = mask ? *(int *)mask : 0;
    pthread_mutex_unlock(&c->lock);
    return 0;
}

static int sofa_delay(STREAM_FILTER_AUDIO *f) { return 0; } /* no queued PCM */
static int sofa_signal_delay_us(STREAM_FILTER_AUDIO *f)
{
    struct sofa_ctx *c = f->priv;
    // Fixed common references for the bundled, hash-pinned profiles. SADIE's
    // front-center energy centroid is ~107 samples at 48 kHz. Preserve the
    // individual ear/direction differences; FIR length/2 is not HRTF delay.
    int samples = c->mode == AUDIO_SOFA_TV
        ? SOFA_TV_FIR_DELAY + SOFA_TV_HRTF_REFERENCE : 107;
    return (int)(((int64_t)samples * 1000000 + 24000) / 48000);
}
static int sofa_drain(STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame, int end)
{
    memset(frame, 0, sizeof(*frame)); /* every input frame was already returned */
    return 0;
}
static int sofa_delete(STREAM_FILTER_AUDIO *f)
{
    if (!f) return 0;
    struct sofa_ctx *c = f->priv;
    if (c) {
        sofa_flush(f);
        av_frame_free(&c->in);
        av_frame_free(&c->out);
        av_free(c->pcm);
        pthread_mutex_destroy(&c->lock);
        afree(c);
    }
    afree(f);
    return 0;
}
STREAM_FILTER_AUDIO *stream_filter_audio_mysofa_new(int mode, const char *path)
{
    if ((mode != AUDIO_SOFA_TV && mode != AUDIO_SOFA_HEADPHONES) ||
        !path || !path[0] || strlen(path) >= AUDIO_SOFA_PATH_MAX)
        return NULL;
    STREAM_FILTER_AUDIO *f = acalloc(1, sizeof(*f));
    struct sofa_ctx *c = acalloc(1, sizeof(*c));
    if (!f || !c) { afree(f); afree(c); return NULL; }
    f->priv = c;
    c->mode = mode;
    snprintf(c->path, sizeof(c->path), "%s", path);
    pthread_mutex_init(&c->lock, NULL);
    c->in = av_frame_alloc();
    c->out = av_frame_alloc();
    if (!c->in || !c->out) { sofa_delete(f); return NULL; }
    f->name = "mysofa";
    f->open = sofa_open;
    f->close = sofa_flush;
    f->delete = sofa_delete;
    f->filter = sofa_filter;
    f->flush = sofa_flush;
    f->set_param = sofa_layout;
    f->delay = sofa_delay;
    f->signal_delay_us = sofa_signal_delay_us;
    f->drain = sofa_drain;
    return f;
}

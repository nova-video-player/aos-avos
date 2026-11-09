#
# Copyright 2026 Courville Software
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

"""Run the production decoder search/recovery with deterministic decoder failures."""
import unittest

from native_replay import extract, run_c, source
from test_dovi_policy import VIDEO, POLICY


HARNESS = VIDEO.replace(
    'int dv_profile,', 'int width, height, subfmt, profile, valid, msPerFrame, '
    'flush_frames, delay_frames, no_extra; int dv_profile,') + POLICY + r'''
#include <pthread.h>
typedef struct STREAM STREAM;
typedef struct DEC DEC;
typedef struct SINK SINK;
typedef struct { int num_frames; } RC;
struct DEC {
    int cpu, is_open, no_extra, async;
    const char *name;
    int (*open)(DEC *, VIDEO_PROPERTIES *, STREAM *, int *, int *);
    int (*get_rc)(DEC *, RC *);
    SINK *(*get_sink)(DEC *);
    int (*cleanup)(DEC *, void *, int);
    int (*close)(DEC *);
    void (*destroy)(DEC *);
    int (*prepare)(DEC *, void *, int);
    void (*set_playback_speed)(DEC *, int, int);
};
struct SINK {
    STREAM *ctx;
    const char *name;
    int is_open;
    int (*open)(SINK *, VIDEO_PROPERTIES *, STREAM *, int, RC *);
    void (*close)(SINK *);
    void (*delete)(SINK *);
    void (*put_time)(void);
};
struct STREAM {
    VIDEO_PROPERTIES *video;
    DEC *video_dec;
    SINK *video_sink;
    RC video_rc;
    pthread_mutex_t video_sink_mutex;
    void *video_mangler, *frames;
    void (*output_frame_fn)(void);
    void (*player)(STREAM *);
    void (*message_cb)(STREAM *, int);
    int cpu_priority, video_decoder_drained, flags, video_error, video_error_qualifier;
    char video_error_desc[128];
    int use_sink_frames, vtime_post_sink, slideshow, put_time_mode, num_frames;
    int video_speed_num, video_speed_den;
};
#define STREAM_CPU_ANY 0
#define STREAM_CPU_DSP 5
#define STREAM_NO_VIDEO_CODEC 1
#define STREAM_THUMB 2
#define STREAM_DECODER_CHANGED 3
#define VE_NO_ERROR 0
#define VEQ_NONE 0
#define VE_ERROR 1
static int stream_force_prio, stream_force_num_frames = -1;
static const char *stream_force_codec;
static int fail_dv, fail_base_hw, fail_sw, no_dv, calls, attempts, destroyed;
static int attempt_format[32], attempt_cpu[32];
static SINK sink;
static int get_cpu_priority(STREAM *s) { return s->cpu_priority; }
static void stream_set_cpu_priority(STREAM *s, int cpu) { s->cpu_priority = cpu; }
static int dec_open(DEC *d, VIDEO_PROPERTIES *v, STREAM *s, int *flush, int *delay) {
    (void)s; (void)flush; (void)delay;
    assert(attempts < 32);
    attempt_format[attempts] = v->format; attempt_cpu[attempts++] = d->cpu;
    if ((v->format == VIDEO_FORMAT_DOLBY_VISION && fail_dv) ||
        (v->format != VIDEO_FORMAT_DOLBY_VISION && d->cpu == 4 && fail_base_hw) ||
        (d->cpu == 1 && fail_sw)) return 1;
    d->is_open = 1;
    return 0;
}
static int dec_close(DEC *d) { d->is_open = 0; return 0; }
static void dec_destroy(DEC *d) { assert(!d->is_open); destroyed++; free(d); }
static DEC *stream_get_new_dec_video(VIDEO_PROPERTIES *v, void **mangler, int cpu,
                                     int forced, const char *name) {
    (void)mangler; (void)forced; (void)name;
    assert(++calls < 40); /* catch repeated DV/base retry loops */
    if (v->format == VIDEO_FORMAT_DOLBY_VISION ? (no_dv || cpu != 4) : (cpu != 4 && cpu != 1))
        return NULL;
    DEC *d = calloc(1, sizeof(*d));
    assert(d);
    d->cpu = cpu; d->name = "test"; d->async = cpu == 4;
    d->open = dec_open; d->close = dec_close; d->destroy = dec_destroy;
    return d;
}
static int sink_open(SINK *out, VIDEO_PROPERTIES *v, STREAM *s, int n, RC *rc) {
    (void)v; (void)s; (void)n; (void)rc; out->is_open = 1; return 0;
}
static void sink_close(SINK *out) { out->is_open = 0; }
static SINK *stream_get_default_video_sink(STREAM *s) { (void)s; return &sink; }
static int stream_sink_video_allocates_frames(SINK *out) { (void)out; return 0; }
static void _output_frame_no_resize(void) {}
static int _allocate_video_buffers(STREAM *s) { (void)s; return 0; }
static void _query_sink_frames(STREAM *s) { (void)s; }
static void stream_show_rc(RC *rc) { (void)rc; }
static int audio_interface_is_audio_speed_enabled(void) { return 0; }
static void _stream_player_async(STREAM *s) { (void)s; }
static void _stream_player_sync(STREAM *s) { (void)s; }
static void stream_close_video_dec(STREAM *s) {
    if (s->video_dec) { s->video_dec->close(s->video_dec); s->video_dec->destroy(s->video_dec); }
    s->video_dec = NULL;
}
static void _stream_close_video_sink(STREAM *s, int destroy) {
    (void)destroy; if (s->video_sink) s->video_sink->close(s->video_sink);
    s->video_sink = NULL;
}
''' + extract(source('stream_video.c'), 'static int _stream_open_video_dec(') + r'''
static int stream_open_video_dec(STREAM *s, int *unsupported) {
    return _stream_open_video_dec(s, unsupported);
}
''' + extract(source('stream_video.c'), 'static int _handle_video_codec_error( STREAM *s )\n{') + r'''
static void reset(STREAM *s, VIDEO_PROPERTIES *v, int format) {
    memset(s, 0, sizeof(*s)); memset(v, 0, sizeof(*v));
    sink = (SINK){ .name = "test", .open = sink_open, .close = sink_close };
    v->format = VIDEO_FORMAT_DOLBY_VISION; v->fourcc = VIDEO_FOURCC_DOLBY_VISION;
    v->codec_id = format == VIDEO_FORMAT_AV1 ? AV_CODEC_ID_AV1 : AV_CODEC_ID_HEVC;
    v->dv_bitstream_profile = format == VIDEO_FORMAT_AV1 ? 10 : 8;
    v->dv_bl_present_flag = 1; v->dv_bl_signal_compatibility_id = 1;
    s->video = v;
    assert(!pthread_mutex_init(&s->video_sink_mutex, NULL));
    fail_dv = fail_base_hw = fail_sw = no_dv = stream_force_prio = 0;
    calls = attempts = destroyed = 0;
}
static void finish(STREAM *s) {
    stream_close_video_dec(s); _stream_close_video_sink(s, 1);
    pthread_mutex_destroy(&s->video_sink_mutex);
}
'''


class DolbyVisionRecoveryTest(unittest.TestCase):
    def test_startup_failure_reaches_base_hardware_then_software(self):
        run_c(HARNESS + r'''
int main(void) {
    STREAM s; VIDEO_PROPERTIES v;
    for (int format = VIDEO_FORMAT_HEVC; format <= VIDEO_FORMAT_AV1; format++) {
        reset(&s, &v, format);
        assert(!stream_open_video_dec(&s, NULL));
        assert(attempts == 1 && v.format == VIDEO_FORMAT_DOLBY_VISION);
        /* Runtime recovery reuses hardware priority for the base codec. */
        assert(!_handle_video_codec_error(&s));
        assert(attempts == 2 && v.format == format && s.video_dec->cpu == 4);
        assert(!video_dovi_colors_unsupported(&v));
        /* A later base failure downgrades once, without returning to DV. */
        assert(!_handle_video_codec_error(&s));
        assert(attempts == 3 && attempt_format[2] == format && s.video_dec->cpu == 1);
        finish(&s);

        reset(&s, &v, format); fail_dv = fail_base_hw = 1;
        assert(!stream_open_video_dec(&s, NULL));
        assert(attempts == 3 && attempt_format[0] == VIDEO_FORMAT_DOLBY_VISION);
        assert(attempt_format[1] == format && attempt_cpu[1] == 4);
        assert(attempt_format[2] == format && attempt_cpu[2] == 1);
        assert(s.video_dec->cpu == 1 && destroyed == 2);
        finish(&s);

        reset(&s, &v, format); fail_dv = fail_base_hw = fail_sw = 1;
        assert(stream_open_video_dec(&s, NULL));
        assert(attempts == 3 && !s.video_dec && destroyed == 3);
        finish(&s);
    }
    return 0;
}
''')

    def test_missing_candidates_forced_priorities_and_incompatible_layers(self):
        run_c(HARNESS + r'''
int main(void) {
    STREAM s; VIDEO_PROPERTIES v;
    reset(&s, &v, VIDEO_FORMAT_AV1); no_dv = 1;
    assert(!stream_open_video_dec(&s, NULL));
    assert(attempts == 1 && attempt_format[0] == VIDEO_FORMAT_AV1 && s.video_dec->cpu == 4);
    finish(&s);
    reset(&s, &v, VIDEO_FORMAT_AV1); stream_force_prio = 1;
    assert(!stream_open_video_dec(&s, NULL));
    assert(attempts == 1 && s.video_dec->cpu == 1 && v.format == VIDEO_FORMAT_AV1);
    finish(&s);
    reset(&s, &v, VIDEO_FORMAT_AV1); stream_force_prio = 4; fail_dv = fail_base_hw = 1;
    assert(stream_open_video_dec(&s, NULL));
    assert(attempts == 2 && attempt_cpu[0] == 4 && attempt_cpu[1] == 4);
    finish(&s);
    reset(&s, &v, VIDEO_FORMAT_AV1); fail_dv = 1; v.dv_bl_signal_compatibility_id = 0;
    assert(!stream_open_video_dec(&s, NULL));
    assert(attempts == 2 && v.format == VIDEO_FORMAT_AV1);
    assert(video_dovi_colors_unsupported(&v));
    finish(&s);
    reset(&s, &v, VIDEO_FORMAT_HEVC); v.dv_bitstream_profile = 5; v.dv_bl_signal_compatibility_id = 0;
    assert(!stream_open_video_dec(&s, NULL));
    assert(!_handle_video_codec_error(&s));
    assert(attempts == 2 && v.format == VIDEO_FORMAT_HEVC);
    assert(video_dovi_colors_unsupported(&v));
    finish(&s);
    /* Pixel case: no DV candidate, restart at hardware HEVC priority. */
    reset(&s, &v, VIDEO_FORMAT_HEVC); no_dv = 1;
    v.dv_bitstream_profile = 5; v.dv_bl_signal_compatibility_id = 0;
    assert(!stream_open_video_dec(&s, NULL));
    assert(attempts == 1 && v.format == VIDEO_FORMAT_HEVC && s.video_dec->cpu == 4);
    assert(video_dovi_colors_unsupported(&v));
    finish(&s);
    reset(&s, &v, VIDEO_FORMAT_HEVC); no_dv = fail_base_hw = 1;
    v.dv_bitstream_profile = 5; v.dv_bl_signal_compatibility_id = 0;
    assert(!stream_open_video_dec(&s, NULL));
    assert(attempts == 2 && s.video_dec->cpu == 1 && video_dovi_colors_unsupported(&v));
    finish(&s);
    /* Non-DV streams retain the existing hardware-to-software sequence. */
    reset(&s, &v, VIDEO_FORMAT_AV1); v.format = VIDEO_FORMAT_AV1; fail_base_hw = 1;
    assert(!stream_open_video_dec(&s, NULL));
    assert(attempts == 2 && s.video_dec->cpu == 1);
    finish(&s);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()

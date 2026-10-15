"""Check the production startup trigger without changing Bluetooth/speed guards."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_video_pcm_startup import block
from native_replay import run_c


class PcmStartupCorrectionTest(unittest.TestCase):
    def test_trusted_startup_phase_check(self):
        source = (Path(__file__).resolve().parents[1] / 'Source/stream_sync.c').read_text()
        decision = block(source, 'if( s->pcm_startup_correction_pending )')
        constants = '\n'.join(re.findall(
            r'^#define STREAM_PCM_STARTUP_(?:CORRECTION_\w+|DIRECT_STREAK)\s+\d+',
            source, re.M))
        code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define CONFIG_AUDIO_AC3
#define DBG if (0)
#define serprintf printf
#define ABS(x) abs(x)
''' + constants + r'''
typedef struct STREAM STREAM;
typedef struct { int (*get_passthrough)(STREAM *); } SINK;
struct STREAM {
    SINK *audio_sink;
    int pcm_startup_correction_pending, pcm_startup_correction_seek_epoch, seek_epoch;
    int pcm_startup_correction_speed_epoch, audio_speed_diag_epoch;
    int pcm_startup_seed_delay_ms, at_speed_epoch_active, video_speed_num, video_speed_den;
    int atempo_commit_count;
};
static int stream_audiotrack_epoch_active(STREAM *s) { return s->at_speed_epoch_active; }
typedef struct { int has_dynamic_evidence, dynamic_evidence_streak, dynamic_evidence_ms; } STATUS;
static int mode, recode, speed_enabled, software_backend;
static int passthrough(STREAM *s) { (void)s; return mode; }
static int libavos_get_ac3_recoding_enabled(void) { return recode; }
static int audio_interface_is_audio_speed_enabled(void) { return speed_enabled; }
static float audio_interface_get_audio_speed(void) { return speed_enabled ? 1.6f : 1.0f; }
static int stream_get_pcm_startup_seed_delay_ms(STREAM *s) { (void)s; return 200; }
static int stream_get_atempo_delay(STREAM *s) { (void)s; return 30; }
static int check(STREAM *s, STATUS delay_status) {
    int pcm_startup_request_correction = 0;
    int pcm_startup_ledger_clock = speed_enabled && software_backend;
''' + decision + r'''
    return pcm_startup_request_correction;
}
int main(void) {
    SINK sink = {passthrough};
    STREAM initial = {.audio_sink = &sink, .pcm_startup_correction_pending = 1,
        .pcm_startup_correction_seek_epoch = 3, .seek_epoch = 3,
        .pcm_startup_correction_speed_epoch = 7, .audio_speed_diag_epoch = 7,
        .pcm_startup_seed_delay_ms = 86, .video_speed_num = 1, .video_speed_den = 1};
    STREAM s = initial;
    STATUS evidence = {1, 10, 76};
    // Sony seek: latency changes only 10ms, but renderer phase is about 105ms.
    // Ask the existing renderer to inspect phase once direct timing is trusted.
    assert(check(&s, evidence));
    assert(!s.pcm_startup_correction_pending);
    assert(!check(&s, evidence)); // exactly once per armed startup
    s = initial; evidence.dynamic_evidence_ms = 86;
    assert(check(&s, evidence)); // equal latency also says nothing about wall phase
    // Retain the Bluetooth warm-up protection; fallback evidence cannot trigger.
    s = initial; evidence.dynamic_evidence_streak = 9;
    assert(!check(&s, evidence) && s.pcm_startup_correction_pending);
    evidence.dynamic_evidence_streak = 10; evidence.has_dynamic_evidence = 0;
    assert(!check(&s, evidence) && s.pcm_startup_correction_pending);
    evidence.has_dynamic_evidence = 1;
    for (mode = 1; mode <= 3; mode++) {
        s = initial;
        assert(!check(&s, evidence) && !s.pcm_startup_correction_pending);
    }
    mode = 0; recode = 1; s = initial;
    assert(!check(&s, evidence) && !s.pcm_startup_correction_pending);
    recode = 0; s = initial; s.seek_epoch++;
    assert(!check(&s, evidence) && !s.pcm_startup_correction_pending);
    s = initial; s.audio_speed_diag_epoch++;
    assert(!check(&s, evidence) && s.pcm_startup_correction_pending);
    assert(s.pcm_startup_seed_delay_ms == 230);
    assert(s.pcm_startup_correction_speed_epoch == s.audio_speed_diag_epoch);
    // The original latency-delta trigger is unchanged for software speed,
    // a retained PlaybackParams checkpoint and an uncommitted return to 1x.
    int deltas[] = {-501, -500, -16, -15, 0, 15, 16, 500, 501};
    for (int policy = 0; policy < 4; policy++) {
        for (unsigned i = 0; i < sizeof(deltas)/sizeof(deltas[0]); i++) {
            s = initial; s.pcm_startup_seed_delay_ms = 1000;
            speed_enabled = policy == 1;
            s.at_speed_epoch_active = policy == 2;
            s.video_speed_num = policy == 3 ? 2 : 1;
            evidence.dynamic_evidence_ms = 1000 + deltas[i];
            int expected = abs(deltas[i]) <= 500 && (policy == 0 || abs(deltas[i]) >= 16);
            assert(check(&s, evidence) == expected);
            assert(!s.pcm_startup_correction_pending);
        }
    }
    // Deferred neutral commit from the Bravia seek: dynamic delay is trusted
    // before presentation has crossed the queued boundary. Keep the one-shot.
    s = initial; speed_enabled = software_backend = 1;
    evidence.dynamic_evidence_ms = 161;
    s.atempo_commit_count = 1;
    assert(!check(&s, evidence) && s.pcm_startup_correction_pending);
    s.atempo_commit_count = 0;
    assert(check(&s, evidence) && s.pcm_startup_correction_pending);
    // Until a qualified sample is dispatched, later writes must retry it.
    assert(check(&s, evidence) && s.pcm_startup_correction_pending);
    s.pcm_startup_correction_pending = 0; // accepted qualified target
    assert(!check(&s, evidence));
    // Software-path deferral does not alter PlaybackParams or plain PCM.
    s = initial; software_backend = 0; s.atempo_commit_count = 1;
    assert(check(&s, evidence) && !s.pcm_startup_correction_pending);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'startup.c').write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', str(root / 'startup.c'), '-o', str(root / 'startup')], check=True)
            subprocess.run([str(root / 'startup')], check=True)

    def test_qualified_target_bypasses_clamped_cache(self):
        root = Path(__file__).resolve().parents[1]
        renderer = (root / 'Source/codec_sfdec2.c').read_text()
        sync = (root / 'Source/stream_sync.c').read_text()
        request = block(renderer, 'static int sfdec2_request_pcm_startup_correction_locked(')
        dispatch = block(sync, 'if( published && pcm_startup_request_correction && pcm_startup_ledger_clock &&')
        private = sorted(set(re.findall(r'p->(\w+)', request)) - {'locked'})
        private_fields = '\n'.join(
            f'{"void *" if name.endswith("handle") else "int64_t "}{name};'
            for name in private)
        stream_fields = sorted(set(re.findall(r's->(\w+)', request + dispatch)) - {'video_sink'})
        code = r'''
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define DBGSI if (0)
#define serprintf(...) ((void)0)
#define AT_PRESENTED_FRAMES_SRC_TIMESTAMP 1
typedef int64_t INT64;
typedef struct { void *priv; const char *name; } SINK;
typedef struct {
    SINK *video_sink;
''' + '\n'.join(f'int {f};' for f in stream_fields) + r'''
} STREAM;
typedef struct {
    struct { pthread_mutex_t mtx; pthread_cond_t cond; } locked;
''' + private_fields + r'''
    int venc_put_time;
} priv_t;
static INT64 now;
static INT64 _get_monotonic_ns(void) { return now; }
''' + request + r'''
static int sfdec2_request_pcm_startup_correction(STREAM *s, int heard_ts, INT64 sample_ns) {
    return sfdec2_request_pcm_startup_correction_locked(s, heard_ts, sample_ns);
}
static void dispatch(STREAM *s, int published, int pcm_source, int pcm_latency_us,
                     int anchor_ts, INT64 pcm_sample_ns) {
    int pcm_startup_request_correction = s->pcm_startup_correction_pending;
    int pcm_startup_ledger_clock = 1;
''' + dispatch + r'''
}
int main(void) {
    priv_t p = {.locked = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER},
        .render_offset_ns = 7644943643228LL, .render_offset_from_audio = 1,
        .venc_put_time = 91585};
    SINK sink = {&p, "sfdec2"};
    STREAM s = {.video_sink = &sink, .pcm_startup_correction_pending = 1,
        .seek_epoch = 13, .pcm_startup_correction_seek_epoch = 13};
    INT64 wall = 7736483422312LL;
    now = wall + 17000000LL; // renderer contention after the sampled pair
    // No publication, fallback, or provisional mixer-head sample can qualify.
    dispatch(&s, 0, 1, -1, 91520, wall);
    dispatch(&s, 1, 0, -1, 91520, wall);
    dispatch(&s, 1, 2, -1, 91520, wall);
    assert(s.pcm_startup_correction_pending && !p.slew_active);
    s.atempo_commit_count = 1;
    dispatch(&s, 1, 1, -1, 91520, wall);
    assert(s.pcm_startup_correction_pending);
    s.atempo_commit_count = 0;
    dispatch(&s, 1, 1, -1, 91520, wall);
    assert(!s.pcm_startup_correction_pending && p.slew_active);
    assert(!p.pending_reanchor && p.pcm_startup_slew && !p.pcm_resume_slew);
    assert(p.target_offset_ns == wall - 91520LL * 1000000);
    assert(p.target_offset_ns != wall - p.venc_put_time * 1000000LL);
    assert(p.render_offset_ns == 7644943643228LL); // no hard reanchor
    // Calibrated head samples can qualify; the provisional bridge cannot.
    s.pcm_startup_correction_pending = 1;
    dispatch(&s, 1, 2, 84000, 91521, wall);
    assert(!s.pcm_startup_correction_pending);
    assert(p.target_offset_ns == wall - 91521LL * 1000000);
    // Noise remains inside the same 8ms deadband used by renderer selection.
    p.render_offset_ns = wall - 91521LL * 1000000 + 7000000;
    s.pcm_startup_correction_pending = 1;
    dispatch(&s, 1, 2, 84000, 91521, wall);
    assert(!p.slew_active && !p.pcm_startup_slew);
    // Expiry or lifecycle changes while acquiring the sink retain the request.
    int *guards[] = {&s.paused, &s.paused_internal, &s.seek_paused,
        &s.audio_start_pending, &s.audio_resume_pending, &s.audio_reconfiguring,
        &s.atempo_commit_count, &s.audio_speed_diag_epoch};
    for (unsigned i = 0; i < sizeof(guards)/sizeof(guards[0]); ++i) {
        *guards[i] = 1;
        s.pcm_startup_correction_pending = 1;
        dispatch(&s, 1, 1, -1, 91520, wall);
        assert(s.pcm_startup_correction_pending);
        *guards[i] = 0;
    }
    int64_t *private_guards[] = {&p.pending_seek_reanchor, &p.pause_armed, &p.pcm_resume_pending};
    for (unsigned i = 0; i < sizeof(private_guards)/sizeof(private_guards[0]); ++i) {
        *private_guards[i] = 1;
        dispatch(&s, 1, 1, -1, 91520, wall);
        assert(s.pcm_startup_correction_pending);
        *private_guards[i] = 0;
    }
    s.seek_epoch++;
    dispatch(&s, 1, 1, -1, 91520, wall);
    assert(s.pcm_startup_correction_pending);
    s.seek_epoch--;
    now = wall + 100000001;
    dispatch(&s, 1, 1, -1, 91520, wall);
    assert(s.pcm_startup_correction_pending);
    now = wall - 1;
    dispatch(&s, 1, 1, -1, 91520, wall);
    assert(s.pcm_startup_correction_pending);
    // Plain PCM/PlaybackParams retain their existing deferred renderer lookup.
    assert(sfdec2_request_pcm_startup_correction(&s, 0, 0));
    assert(p.pending_reanchor && p.pcm_startup_slew);
    pthread_mutex_destroy(&p.locked.mtx);
    pthread_cond_destroy(&p.locked.cond);
}
'''
        run_c(code)


if __name__ == '__main__':
    unittest.main()

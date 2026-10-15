"""Replay the production PCM calibration gate and first clock handoff."""
import os
import re
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_video_pcm_startup import block


class PlayheadHandoffTest(unittest.TestCase):
    def test_publication_keeps_the_pair_serialized(self):
        source = (Path(__file__).resolve().parents[1] / 'Source/stream_sync.c').read_text()
        publish = block(source, 'static int _stream_sync_anchor_publish(')
        reset = block(source, 'void stream_sync_anchor_reset(')
        matches = block(source, 'static int _stream_pcm_clock_ref_matches(')
        fields = set(re.findall(r's->(\w+)', publish + reset + matches)) - {'video_sink'}
        declarations = '\n'.join(
            f'{"int64_t" if name.endswith("wall_ms") else "int"} {name};'
            for name in sorted(fields))
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <limits.h>
#define STREAM_NO_PTS_VALUE INT_MIN
static int calls, stream_dbg_delay, pcm_clock_mutex;
typedef struct { double rst_anchor, ts_anchor, speed, inv_speed; } timeline_state_t;
static timeline_state_t timeline_snapshot(void) { return (timeline_state_t){0, 0, 1, 1}; }
static int64_t atime64(void) { return 3000000041LL; }
static void pthread_mutex_lock(int *m) { assert(!*m); *m = 1; }
static int pthread_mutex_trylock(int *m) { if (*m) return 1; *m = 1; return 0; }
static void pthread_mutex_unlock(int *m) { assert(*m); *m = 0; }
typedef struct SINK { int is_open; int (*put_time)(struct SINK *, int); } SINK;
typedef struct { SINK *video_sink;
''' + declarations + r'''
} STREAM;
static STREAM *active;
static void sfdec2_refresh_sched_anchor_locked(STREAM *s) { (void)s; }
static int put(SINK *sink, int ts) {
    (void)sink;
    assert(active->anchor_mutex && active->video_sink_mutex);
    assert(!pcm_clock_mutex); // Never held across the renderer callback.
    assert(active->pcm_playhead_latency_us == 75000 && ts == 100040);
    calls++;
    return 0;
}
''' + matches + publish + reset + r'''
int main(void) {
    SINK sink = {1, put};
    STREAM stream = {.video_sink = &sink, .put_time_mode = 1,
                     .seek_epoch = 2, .audio_speed_diag_epoch = 3,
                     .audio_lifecycle_generation = 7, .av_delay = 100,
                     .sink_ref_time = -1}, *s = &stream;
    active = s;
    assert(_stream_sync_anchor_publish(s, 100040, 100020, 0, 0, 75000, 100140, 1));
    assert(calls == 1 && s->pcm_playhead_latency_us == -1);
    assert(s->pcm_clock_ref_valid && s->pcm_clock_ref_ts == 100140);
    assert(s->pcm_clock_ref_wall_ms == 3000000041LL);
    assert(s->pcm_clock_ref_seek_epoch == 2 && s->pcm_clock_ref_speed_epoch == 3);
    assert(s->pcm_clock_ref_audio_generation == 7 && s->pcm_clock_ref_av_delay == 100);
    // A dropped publication cannot replace the timestamp or its reference time.
    s->video_sink_mutex = 1;
    assert(!_stream_sync_anchor_publish(s, 100050, 100020, 0, 0, 75000, 100150, 1));
    assert(calls == 1 && s->pcm_clock_ref_ts == 100140 && !s->anchor_mutex);
    s->video_sink_mutex = 0;
    assert(!_stream_sync_anchor_publish(s, 100050, 100020, 1, 0, 75000, 100150, 1));
    assert(calls == 1 && s->pcm_clock_ref_ts == 100140);
    // Non-PCM publications never leave a usable PCM reference behind.
    s->pcm_head_bridge_active = 1;
    assert(_stream_sync_anchor_publish(s, 100040, 100020, 0, 0, 75000, STREAM_NO_PTS_VALUE, 1));
    assert(!s->pcm_clock_ref_valid && !s->pcm_head_bridge_active);
    assert(_stream_sync_anchor_publish(s, 100040, 100020, 0, 0, 75000, 100140, 1));
    s->pcm_head_bridge_active = 1;
    stream_sync_anchor_reset(s);
    assert(!s->pcm_head_bridge_active);
    assert(!s->pcm_clock_ref_valid && s->sink_ref_time == -1 && s->vid_ref_time == -1);
    sink.is_open = 0;
    assert(_stream_sync_anchor_publish(s, 100040, 100020, 0, 0, 75000, 100140, 1));
    assert(!s->pcm_clock_ref_valid);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'publication.c').write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', str(root / 'publication.c'), '-o', str(root / 'publication')], check=True)
            subprocess.run([str(root / 'publication')], check=True)

    def test_timestamp_expiry_bridge_and_invalidation(self):
        from native_replay import run_c
        root = Path(__file__).resolve().parents[1]
        source = (root / 'Source/stream_sync.c').read_text()
        header = (root / 'Include/stream.h').read_text()
        helpers = ''.join(block(source, marker) for marker in (
            'static int _stream_pcm_clock_ref_matches(',
            'static int _stream_pcm_head_bridge(',
            'int stream_atempo_presentation('))
        fields = (set(re.findall(r's->(\w+)', helpers)) | {'atempo_ledger_lat_samples'}) - {'anchor_mutex', 'audio_ctx'}
        declarations = '\n'.join(re.search(
            rf'^\s*(?:unsigned int|\w+)\s+{name}\s*;', header, re.MULTILINE)[0]
            for name in sorted(fields))
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <limits.h>
typedef uint64_t UINT64;
#define STREAM_NO_PTS_VALUE INT_MIN
#define AT_PRESENTED_FRAMES_SRC_TIMESTAMP 1
#define AT_PRESENTED_FRAMES_SRC_PLAYHEAD 2
#define MAX(a,b) ((a)>(b)?(a):(b))
#define DBG if (0)
#define serprintf(...) ((void)0)
static int stream_dbg_delay, pcm_clock_mutex;
static void pthread_mutex_lock(int *m) { assert(!*m); *m=1; }
static void pthread_mutex_unlock(int *m) { assert(*m); *m=0; }
typedef struct { double rst_anchor, ts_anchor, speed, inv_speed; } timeline_state_t;
static timeline_state_t mapping = {10, 20, 1, 1};
static timeline_state_t timeline_snapshot(void) { return mapping; }
typedef struct { int anchor_mutex; void *audio_ctx;
''' + declarations + r'''
} STREAM;
typedef struct { int heard, heard_rst, state; } ATEMPO_LEDGER_LOOKUP;
static int _stream_atempo_presented_frames(STREAM *s, UINT64 *f, int *r, int *src, int *age) {
    (void)s; *f=48000; *r=48000; *src=2; *age=0; return 1;
}
static ATEMPO_LEDGER_LOOKUP _stream_atempo_ledger_lookup(STREAM *s, UINT64 f, int r) {
    (void)s; (void)f; (void)r; return (ATEMPO_LEDGER_LOOKUP){1000,1000,0};
}
''' + helpers + r'''
static int sample(STREAM *s, int raw, UINT64 frames, int64_t now, int *heard) {
    return _stream_pcm_head_bridge(s,raw,frames,48000,2,0,0,1,now,heard);
}
static STREAM seed(void) {
    return (STREAM){.put_time_mode=1, .pcm_clock_ref_valid=1,
        .pcm_clock_ref_ts=522359, .pcm_clock_ref_wall_ms=3000000100LL,
        .pcm_clock_ref_source=1, .pcm_clock_ref_rst_anchor=10,
        .pcm_clock_ref_ts_anchor=20, .pcm_clock_ref_speed=1,
        .video_speed_num=1, .video_speed_den=1};
}
int main(void) {
    STREAM s=seed(); int heard=-1;
    const int64_t now=3000000132LL;
    // Logged timestamp expiry: align the head to the last clock plus 32ms,
    // instead of jumping to raw 522461 or the legacy fallback 522444.
    assert(sample(&s,522461,48000,now,&heard) && heard==522391);
    assert(s.pcm_head_bridge_active && s.pcm_head_bridge_offset_ms==-70);
    assert(!s.atempo_ledger_lat_valid && !s.atempo_ledger_lat_samples);
    // A provisional publication cannot re-seed the offset on the next query.
    s.pcm_clock_ref_ts=heard; s.pcm_clock_ref_wall_ms=now; s.pcm_clock_ref_source=2;
    assert(sample(&s,522493,49536,now+32,&heard) && heard==522423);
    // Counter stalls do not accumulate wall time, even with repeated queries.
    for(int i=1;i<=100;i++)
        assert(sample(&s,522493,49536,now+32+i*10,&heard) && heard==522423);
    // Unconfirmed head evidence cannot cross a speed commit boundary.
    s.audio_ctx=&s; s.atempo_ledger_active=1; s.atempo_ledger_count=1;
    UINT64 ph; int rate,ts,rst,state;
    assert(!stream_atempo_presentation(&s,&ph,&rate,&ts,&rst,&state));
    s.atempo_ledger_lat_valid=1;
    assert(!sample(&s,522493,49536,now+40,&heard) && !s.pcm_head_bridge_active);
    assert(stream_atempo_presentation(&s,&ph,&rate,&ts,&rst,&state));
    // Every lifecycle mismatch invalidates an already active mapping.
#define INVALIDATE(change) do { s=seed(); assert(sample(&s,522461,48000,now,&heard)); \
    change; assert(!sample(&s,522493,49536,now+32,&heard)); \
    assert(!s.pcm_head_bridge_active); } while(0)
    INVALIDATE(s.paused=1);
    INVALIDATE(s.paused_internal=1);
    INVALIDATE(s.seek_paused=1);
    INVALIDATE(s.audio_start_pending=1);
    INVALIDATE(s.audio_resume_pending=1);
    INVALIDATE(s.audio_reconfiguring=1);
    INVALIDATE(s.atempo_commit_count=1);
    INVALIDATE(s.manual_audio_hold_pending_ms=1);
    INVALIDATE(s.seek_epoch++);
    INVALIDATE(s.audio_speed_diag_epoch++);
    INVALIDATE(s.audio_lifecycle_generation++);
    INVALIDATE(s.av_delay++);
    INVALIDATE(s.pcm_clock_ref_valid=0);
    INVALIDATE(s.video_speed_num=2);
    INVALIDATE(stream_dbg_delay++); stream_dbg_delay=0;
    INVALIDATE(mapping.rst_anchor++); mapping.rst_anchor=10;
    INVALIDATE(mapping.ts_anchor++); mapping.ts_anchor=20;
    INVALIDATE(mapping.speed=1.2); mapping.speed=1;
    s=seed(); s.pcm_clock_ref_source=0;
    assert(!sample(&s,522461,48000,now,&heard));
    s=seed(); s.pcm_clock_ref_source=2;
    assert(!sample(&s,522461,48000,now,&heard));
    s=seed(); assert(!sample(&s,522461,48000,now+69,&heard)); // ref >100ms
    assert(!sample(&s,522461,48000,now-33,&heard)); // backwards wall time
    assert(!sample(&s,522000,48000,now,&heard)); // cannot lead the raw head
    assert(!_stream_pcm_head_bridge(&s,522461,48000,48000,2,101,0,1,now,&heard));
    assert(!_stream_pcm_head_bridge(&s,522461,48000,48000,2,0,1,1,now,&heard));
    assert(!_stream_pcm_head_bridge(&s,522461,48000,48000,2,0,0,1.2,now,&heard));
    assert(!_stream_pcm_head_bridge(&s,522461,48000,48000,1,0,0,1,now,&heard));
    assert(!_stream_pcm_head_bridge(&s,522461,48000,0,2,0,0,1,now,&heard));
    // Publication may own the anchor lock; PCM state remains independently readable.
    s.anchor_mutex=1;
    assert(sample(&s,522461,48000,now,&heard) && heard==522391);
    s.anchor_mutex=0;
    assert(sample(&s,522461,48000,now,&heard));
    assert(!sample(&s,522460,47999,now+1,&heard) && !s.pcm_head_bridge_active);
    assert(!sample(&s,522461,48000,now+2,&heard)); // old reference cannot re-arm
    s=seed(); assert(sample(&s,522461,48000,now,&heard));
    assert(!_stream_pcm_head_bridge(&s,522461,48000,44100,2,0,0,1,now,&heard));
    assert(!sample(&s,522461,48000,now+2,&heard));
    return 0;
}
''')

    def test_active_bridge_survives_blocked_publication(self):
        from native_replay import run_c
        root = Path(__file__).resolve().parents[1]
        source = (root / 'Source/stream_sync.c').read_text()
        header = (root / 'Include/stream.h').read_text()
        helpers = ''.join(block(source, marker) for marker in (
            'static int _stream_pcm_clock_ref_matches(',
            'static int _stream_sync_anchor_publish(',
            'static int _stream_pcm_head_bridge(',
            'static int _stream_pcm_handoff_heard_ts(',
            'void stream_sync_anchor_reset('))
        fields = set(re.findall(r's->(\w+)', helpers)) - {
            'anchor_mutex', 'video_sink_mutex', 'video_sink'}
        declarations = '\n'.join(re.search(
            rf'^\s*(?:unsigned int|\w+)\s+{name}\s*;', header, re.MULTILINE)[0]
            for name in sorted(fields))
        pcm_mutex = re.search(r'^static pthread_mutex_t pcm_clock_mutex.*;',
                              source, re.MULTILINE)[0]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <limits.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
typedef uint64_t UINT64;
#define STREAM_NO_PTS_VALUE INT_MIN
#define AT_PRESENTED_FRAMES_SRC_TIMESTAMP 1
#define AT_PRESENTED_FRAMES_SRC_PLAYHEAD 2
#define DBG if (0)
#define serprintf(...) ((void)0)
static int stream_dbg_delay;
typedef struct { double rst_anchor, ts_anchor, speed, inv_speed; } timeline_state_t;
static timeline_state_t timeline_snapshot(void) { return (timeline_state_t){10.5,20.5,1,1}; }
static int64_t wall=3000000100LL;
static int64_t atime64(void) { return wall; }
typedef struct SINK { int is_open; int (*put_time)(struct SINK *, int); } SINK;
typedef struct {
    pthread_mutex_t anchor_mutex, video_sink_mutex;
    SINK *video_sink;
''' + declarations + r'''
} STREAM;
''' + pcm_mutex + r'''
static void sfdec2_refresh_sched_anchor_locked(STREAM *s) { (void)s; }
''' + helpers + r'''
static pthread_mutex_t renderer_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rendezvous=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t entered=PTHREAD_COND_INITIALIZER;
static int block_callback, in_callback;
static int put(SINK *sink, int ts) {
    (void)sink; (void)ts;
    // The production publisher must release the PCM mutex before callbacks.
    assert(!pthread_mutex_trylock(&pcm_clock_mutex));
    pthread_mutex_unlock(&pcm_clock_mutex);
    if (block_callback) {
        pthread_mutex_lock(&rendezvous);
        in_callback=1;
        pthread_cond_signal(&entered);
        pthread_mutex_unlock(&rendezvous);
        pthread_mutex_lock(&renderer_mutex);
        pthread_mutex_unlock(&renderer_mutex);
    }
    return 0;
}
static void *publish(void *arg) {
    STREAM *s=arg;
    assert(_stream_sync_anchor_publish(s,522423,522400,0,0,-1,522423,2));
    return NULL;
}
int main(void) {
    alarm(10); // A lock-order regression fails instead of hanging the test suite.
    SINK sink={1,put};
    STREAM s={.anchor_mutex=PTHREAD_MUTEX_INITIALIZER,
        .video_sink_mutex=PTHREAD_MUTEX_INITIALIZER, .video_sink=&sink,
        .put_time_mode=1, .video_speed_num=1, .video_speed_den=1};
    int heard;
    assert(_stream_sync_anchor_publish(&s,522359,522300,0,0,-1,522359,1));
    assert(_stream_pcm_head_bridge(&s,522461,48000,48000,2,0,0,1,wall+32,&heard));
    assert(heard==522391);
    wall+=64; block_callback=1;
    pthread_mutex_lock(&renderer_mutex);
    pthread_t writer;
    assert(!pthread_create(&writer,NULL,publish,&s));
    pthread_mutex_lock(&rendezvous);
    while(!in_callback) pthread_cond_wait(&entered,&rendezvous);
    pthread_mutex_unlock(&rendezvous);
    assert(pthread_mutex_trylock(&s.anchor_mutex)==EBUSY);
    // Renderer owns its private lock; publisher owns anchor_mutex and is
    // waiting for that private lock. Neither bridge nor calibration may lose
    // the reference or try to reacquire anchor_mutex from this query.
    assert(_stream_pcm_head_bridge(&s,522493,49536,48000,2,0,0,1,wall,&heard));
    assert(heard==522423);
    assert(_stream_pcm_handoff_heard_ts(&s,522476,wall,&heard));
    assert(heard==522423);
    for (int i=1;i<=10;i++) {
        assert(_stream_pcm_head_bridge(&s,522493,49536,48000,2,0,0,1,wall+i*10,&heard));
        assert(heard==522423); // Still no fabricated progress while blocked.
    }
    pthread_mutex_unlock(&renderer_mutex);
    assert(!pthread_join(writer,NULL));
    assert(s.pcm_head_bridge_active);
    // Epoch invalidation still wins, even while the anchor lock is held.
    pthread_mutex_lock(&s.anchor_mutex);
    s.seek_epoch++;
    assert(!_stream_pcm_head_bridge(&s,522525,51072,48000,2,0,0,1,wall+32,&heard));
    assert(!s.pcm_head_bridge_active);
    pthread_mutex_unlock(&s.anchor_mutex);
    stream_sync_anchor_reset(&s);
    assert(!s.pcm_clock_ref_valid);
    pthread_mutex_destroy(&s.video_sink_mutex);
    pthread_mutex_destroy(&s.anchor_mutex);
    alarm(0);
    return 0;
}
''')

    def test_handoff_and_existing_calibration_policy(self):
        source = (Path(__file__).resolve().parents[1] / 'Source/stream_sync.c').read_text()
        calibration = block(source, 'if( !playhead_is_dac &&')
        handoff = block(source, 'static int _stream_pcm_handoff_heard_ts(')
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#define STREAM_NO_PTS_VALUE INT_MIN
#define MAX(a,b) ((a)>(b)?(a):(b))
static int lock_busy, pcm_clock_mutex;
static int pthread_mutex_trylock(int *m) { (void)m; return lock_busy; }
static void pthread_mutex_unlock(int *m) { (void)m; }
#define DBG if (0)
#define serprintf(...) ((void)0)
typedef struct {
    int64_t atempo_ledger_lat_last_ms, atempo_ledger_lat_frames;
    int atempo_ledger_lat_samples, atempo_ledger_lat_valid;
    int anchor_mutex, pcm_clock_ref_valid, pcm_clock_ref_ts;
    int64_t pcm_clock_ref_wall_ms;
    int paused, paused_internal, seek_paused, audio_start_pending, audio_resume_pending;
    int audio_reconfiguring, atempo_commit_count, manual_audio_hold_pending_ms;
    int video_speed_num, video_speed_den;
    int seek_epoch, audio_speed_diag_epoch, av_delay;
    unsigned audio_lifecycle_generation, pcm_clock_ref_audio_generation;
    int pcm_clock_ref_seek_epoch, pcm_clock_ref_speed_epoch, pcm_clock_ref_av_delay;
} STREAM;
''' + handoff + r'''
static void sample(STREAM *s, int64_t wall_now, int bias_ms, int playhead_rate,
                   int playhead_is_dac, int playhead_age, int state,
                   float cur_speed, int delay_valid, int evidence) {
    struct { int heard, state; } raw = {100000 + bias_ms, state};
    struct { int has_dynamic_evidence; } delay_status = {evidence};
    int heard_ts = 100000;
''' + calibration + r'''
}
static void good(STREAM *s, int64_t now, int bias, int rate) {
    sample(s, now, bias, rate, 0, 0, 0, 1.0f, 1, 0);
}
int main(void) {
    STREAM s = {0};
    // A shallow queue's early samples must not leak into the first active clock.
    good(&s, 250, 0, 48000);
    good(&s, 500, 0, 48000);
    good(&s, 750, 151, 48000);
    assert(s.atempo_ledger_lat_samples == 3 && !s.atempo_ledger_lat_valid);
    good(&s, 1000, 150, 48000);
    assert(s.atempo_ledger_lat_valid && s.atempo_ledger_lat_frames == 7200);
    assert(150 - s.atempo_ledger_lat_frames * 1000 / 48000 == 0);
    // Reproduce the logged 83ms warm-up estimate versus a 150ms live bias.
    // The old fourth update would expose a roughly 67ms forward clock jump.
    s = (STREAM){.atempo_ledger_lat_samples = 3, .atempo_ledger_lat_frames = 2923};
    good(&s, 1000, 150, 48000);
    assert(s.atempo_ledger_lat_frames == 7200 && s.atempo_ledger_lat_valid);
    // The second recording held heard time nearly constant for 41ms. Start
    // from the prior publication projected to now, not that held write value.
    STREAM paired = {.atempo_ledger_lat_samples = 3, .atempo_ledger_lat_frames = 4944,
                     .pcm_clock_ref_valid = 1, .pcm_clock_ref_ts = 99999,
                     .pcm_clock_ref_wall_ms = 959,
                     .seek_epoch = 2, .pcm_clock_ref_seek_epoch = 2,
                     .audio_lifecycle_generation = 7, .pcm_clock_ref_audio_generation = 7};
    STREAM check = paired;
    good(&check, 1000, 115, 48000);
    assert(check.atempo_ledger_lat_frames == 3600); // 75ms, not 115ms
    assert(100115 - check.atempo_ledger_lat_frames / 48 == 100040);
    int projected;
    assert(_stream_pcm_handoff_heard_ts(&paired, 100000, 1000, &projected));
    assert(projected == 100040);
    // No field from a previous lifecycle may supply a wall-time projection.
#define FALLBACK(field, value) do { check = paired; check.field = value; \
    assert(_stream_pcm_handoff_heard_ts(&check, 100000, 1000, &projected)); \
    assert(projected == 100000); } while (0)
    FALLBACK(pcm_clock_ref_valid, 0);
    FALLBACK(pcm_clock_ref_wall_ms, 899);
    FALLBACK(pcm_clock_ref_wall_ms, 1001);
    FALLBACK(pcm_clock_ref_seek_epoch, 1);
    FALLBACK(pcm_clock_ref_speed_epoch, 1);
    FALLBACK(pcm_clock_ref_audio_generation, 6);
    FALLBACK(pcm_clock_ref_av_delay, -100);
    FALLBACK(paused, 1);
    FALLBACK(paused_internal, 1);
    FALLBACK(seek_paused, 1);
    FALLBACK(audio_start_pending, 1);
    FALLBACK(audio_resume_pending, 1);
    FALLBACK(audio_reconfiguring, 1);
    FALLBACK(atempo_commit_count, 1);
    FALLBACK(manual_audio_hold_pending_ms, 100);
    FALLBACK(pcm_clock_ref_ts, INT_MAX);
    check = paired; check.pcm_clock_ref_wall_ms = 3000000000LL;
    assert(_stream_pcm_handoff_heard_ts(&check, 100000, 3000000041LL, &projected));
    assert(projected == 100040); // long uptime must not wrap
    lock_busy = 1;
    check = paired;
    good(&check, 1000, 115, 48000);
    assert(!check.atempo_ledger_lat_valid); // no blocking/inverted lock order
    lock_busy = 0;
    good(&check, 1250, 115, 48000);
    assert(check.atempo_ledger_lat_valid); // readiness retried, no lost request
    // Once active, preserve quarter-step smoothing; do not repeatedly reseed.
    good(&s, 1250, 130, 48000);
    assert(s.atempo_ledger_lat_frames == 6960);
    good(&s, 5000, 110, 48000); // retained calibration after an ordinary pause
    assert(s.atempo_ledger_lat_frames == 6540);
    // Requested 1x is not yet audible while the down-ramp commits are pending.
    s.atempo_commit_count = 1;
    good(&s, 5250, 180, 48000);
    assert(s.atempo_ledger_lat_frames == 6540 && s.atempo_ledger_lat_last_ms == 5000);
    s.atempo_commit_count = 0;
    s.video_speed_num = 120; s.video_speed_den = 100;
    good(&s, 5500, 180, 48000);
    assert(s.atempo_ledger_lat_frames == 6540);
    s.video_speed_num = 100;
    good(&s, 5750, 180, 48000);
    assert(s.atempo_ledger_lat_frames == 7065);
    // A flushing seek clears the calibration and requires four new samples.
    s = (STREAM){0};
    good(&s, 250, 250, 44100);
    good(&s, 500, 250, 44100);
    good(&s, 750, 200, 44100);
    assert(!s.atempo_ledger_lat_valid);
    good(&s, 1000, 117, 44100);
    assert(s.atempo_ledger_lat_valid);
    assert(llabs(117 - s.atempo_ledger_lat_frames * 1000 / 44100) <= 1);
    // The handoff can approach from either side; negative latency stays clamped.
    s = (STREAM){.atempo_ledger_lat_samples = 3, .atempo_ledger_lat_frames = 4800};
    good(&s, 1000, -10, 48000);
    assert(s.atempo_ledger_lat_valid && s.atempo_ledger_lat_frames == 0);
    // Existing freshness, spacing, speed and evidence exclusions remain intact.
    s = (STREAM){.atempo_ledger_lat_samples = 3, .atempo_ledger_lat_frames = 2923,
                 .atempo_ledger_lat_last_ms = 1000};
#define UNCHANGED() assert(s.atempo_ledger_lat_samples == 3 && \
    s.atempo_ledger_lat_frames == 2923 && !s.atempo_ledger_lat_valid)
    good(&s, 1249, 150, 48000); UNCHANGED();
    sample(&s, 1500, 150, 48000, 1, 0, 0, 1, 1, 0); UNCHANGED();
    sample(&s, 1500, 150, 48000, 0, 101, 0, 1, 1, 0); UNCHANGED();
    sample(&s, 1500, 150, 48000, 0, 0, -1, 1, 1, 0); UNCHANGED();
    sample(&s, 1500, 150, 48000, 0, 0, 1, 1, 1, 0); UNCHANGED();
    sample(&s, 1500, 150, 48000, 0, 0, 0, 1.6f, 1, 0); UNCHANGED();
    sample(&s, 1500, 150, 48000, 0, 0, 0, 1, 0, 0); UNCHANGED();
    // Trusted dynamic evidence alongside last-good still enables calibration.
    sample(&s, 1500, 150, 48000, 0, 0, 0, 1, 0, 1);
    assert(s.atempo_ledger_lat_valid && s.atempo_ledger_lat_frames == 7200);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'handoff.c').write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', str(root / 'handoff.c'), '-o', str(root / 'handoff')], check=True)
            subprocess.run([str(root / 'handoff')], check=True)


if __name__ == '__main__':
    unittest.main()

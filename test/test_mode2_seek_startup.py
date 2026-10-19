"""Exercise production Mode 2 seek guards without Android or a decoder."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from native_replay import extract, run_c, source


def block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class Mode2SeekStartupTest(unittest.TestCase):
    def test_frontier_reason_survives_reset_until_consumed(self):
        sync = source('stream_sync.c')
        functions = '\n'.join(extract(sync, marker) for marker in (
            'static void _stream_sync_mode2_heard_reset_locked(',
            'void stream_sync_mode2_heard_reset(',
            'void stream_sync_mode2_heard_frontier_arm(',
            'int stream_sync_mode2_heard_frontier_pending('))
        start = sync.index('int frontier_restart =')
        end = sync.index('s->mode2_heard_frontier_seed_pending = STREAM_MODE2_FRONTIER_NONE;', start)
        end = sync.index(';', end) + 1
        consume = sync[start:end]
        qualification = '\n'.join(re.findall(r'^#define STREAM_MODE2_SEEK_\w+\s+\d+',
                                             sync, re.MULTILINE))
        run_c(r'''
#define CONFIG_SFDEC
#include "global.h"
#include "stream.h"
#include "stream_sync.h"
#include <assert.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#define DBG if(0)
#define serprintf(...) printf(__VA_ARGS__)
static int stream_dbg_delay;
_Static_assert(STREAM_MODE2_FRONTIER_NONE == 0, "pending truth value");
_Static_assert(STREAM_MODE2_FRONTIER_RECREATE == 1, "recreation reason");
_Static_assert(STREAM_MODE2_FRONTIER_SEEK == 2, "seek reason");
''' + qualification + '\n' + functions + '\n' + extract(sync, 'static int _stream_mode2_seek_seed_locked(') + r'''
// Restart wrapper calls the shared reset; unrelated reset side effects are mocked.
static int _stream_sync_restart_locked(STREAM *s, int reset_ledger, int preserve) {
    assert(reset_ledger && !preserve);
    _stream_sync_mode2_heard_reset_locked(s, 0, reset_ledger);
    return 0;
}
''' + extract(sync, 'int stream_sync_restart_with_mode2_frontier(') + r'''
int stream_sync_anchor_get_sink(STREAM *s) { (void)s; return 1000; }
void stream_sync_anchor_reset(STREAM *s) { (void)s; }
void sfdec2_refresh_sched_anchor(STREAM *s) { (void)s; }
int stream_sync_restart(STREAM *s) { return _stream_sync_restart_locked(s,1,0); }
static int flushes;
static int flush_output(STREAM *s) { (void)s; flushes++; return 0; }
''' + extract(source('stream_audio.c'), 'static void _stream_abort_incomplete_compressed_unit(') + r'''
static int consume(STREAM *s) {
    int first_start=1, delay_change=0, raw_heard_ts=1000, reset_interp=0;
    int heard_delay=200, fixed_latency=40, seed=0;
    int64_t wall_now=10000;
    const char *seed_cause="";
''' + consume + '\n' + extract(sync, 'if( frontier_restart )') + r'''
    assert(reset_interp);
    if(frontier_restart) {
        assert(seed==1160);
        assert(!strcmp(seed_cause,seek_restart?"seek_frontier":"restart_frontier"));
    }
    return seek_restart;
}
int main(void) {
    STREAM s={0};
    pthread_mutex_init(&s.mode2_heard_mutex,NULL);
    pthread_mutex_init(&s.anchor_mutex,NULL);
    assert(!stream_sync_mode2_heard_frontier_pending(NULL));
    stream_sync_mode2_heard_frontier_arm(NULL,STREAM_MODE2_FRONTIER_SEEK);
    assert(!stream_sync_mode2_heard_frontier_pending(&s));
    const STREAM_MODE2_FRONTIER_REASON reasons[]={STREAM_MODE2_FRONTIER_RECREATE,
        STREAM_MODE2_FRONTIER_SEEK};
    for(unsigned i=0;i<2;i++) {
        stream_sync_mode2_heard_frontier_arm(&s,reasons[i]);
        for(int reset=0;reset<3;reset++) {
            stream_sync_mode2_heard_reset(&s,0);
            assert(stream_sync_mode2_heard_frontier_pending(&s)==(int)reasons[i]);
        }
        assert(consume(&s)==(reasons[i]==STREAM_MODE2_FRONTIER_SEEK));
        assert(s.mode2_seek_refill_until_ms==(i?10950:0));
        assert(!stream_sync_mode2_heard_frontier_pending(&s));
        assert(!consume(&s)); // Consumed once; no new seek refill ownership.
        stream_sync_mode2_heard_frontier_arm(&s,reasons[i]);
        stream_sync_mode2_heard_reset(&s,1);
        assert(!stream_sync_mode2_heard_frontier_pending(&s));
    }
    assert(!stream_sync_restart_with_mode2_frontier(&s));
    assert(stream_sync_mode2_heard_frontier_pending(&s)==STREAM_MODE2_FRONTIER_RECREATE);
    // Real ordering: _seek_pause marks ownership before stopping the writer.
    // Aborting its partial AU restarts sync, then the normal seek clears/arms it.
    s.audio_time=2000;
    STREAM_SINK_AUDIO sink={.flush=flush_output};
    s.audio_sink=&sink;
    s.mode2_seek_calibration.valid=1;
    s.mode2_seek_calibration.correction_ms=200;
    s.mode2_seek_calibration.delay_ms=609;
    s.mode2_seek_calibration.fixed_ms=199;
    s.seek_paused=1;
    _stream_abort_incomplete_compressed_unit(&s,1024,2560,2,0);
    assert(flushes==1);
    stream_sync_mode2_heard_reset(&s,1);
    stream_sync_mode2_heard_frontier_arm(&s,STREAM_MODE2_FRONTIER_SEEK);
    assert(s.mode2_seek_calibration.valid);
    s.audio=&s.av.audio[0]; s.audio->valid=1;
    assert(_stream_mode2_seek_seed_locked(&s,10000,609,199,2000)==9800);
    // The same abort during ordinary playback must invalidate the calibration.
    s.mode2_seek_calibration.valid=1;
    s.seek_paused=0;
    _stream_abort_incomplete_compressed_unit(&s,1024,2560,2,0);
    assert(!s.mode2_seek_calibration.valid);
    pthread_mutex_destroy(&s.anchor_mutex);
    pthread_mutex_destroy(&s.mode2_heard_mutex);
    return 0;
}
''')

        seek_pause = extract(source('stream_video.c'), 'static int _seek_pause(')
        self.assertLess(seek_pause.index('s->seek_paused     = 1;'),
                        seek_pause.index('thread_state_set( &s->audio_tstate'))

    def test_seek_calibration_is_relative_and_output_scoped(self):
        sync = source('stream_sync.c')
        constants = '\n'.join(re.findall(r'^#define STREAM_MODE2_SEEK_\w+\s+\d+',
                                         sync, re.MULTILINE))
        helpers = '\n'.join(extract(sync, marker) for marker in (
            'static int _stream_mode2_seek_seed_locked(',
            'static void _stream_mode2_seek_calibrate_locked(',
            'static void _stream_sync_mode2_heard_reset_locked('))
        run_c(r'''
#define CONFIG_SFDEC
#include "global.h"
#include "stream.h"
#include <assert.h>
#include <limits.h>
#include <string.h>
#define DBG if(0)
#define serprintf(...) ((void)0)
static int stream_dbg_delay;
''' + constants + '\n' + helpers + r'''
static void init(STREAM *s) {
    memset(s,0,sizeof(*s));
    s->audio=&s->av.audio[0];
    s->audio->valid=1;
    s->audio->format=0x4747;
    s->audio->samplesPerSec=48000;
    s->audio_lifecycle_generation=7;
}
static int seed(STREAM *s,int64_t wall) {
    s->mode2_seek_refill_until_ms=wall+1359;
    s->mode2_heard_interp_delay_ms=609;
    return _stream_mode2_seek_seed_locked(s,10000,609,199,wall);
}
static void qualify(STREAM *s,int applied,int correction,int64_t wall) {
    _stream_mode2_seek_calibrate_locked(s,wall+965,10965-applied,10965-correction);
}
int main(void) {
    STREAM s;
    init(&s);
    assert(seed(&s,1000)==10000); // No previous qualified seek.
    qualify(&s,0,188,1000); // First Bravia capture, 188ms below the seed.
    assert(s.mode2_seek_calibration.valid);
    assert(s.mode2_seek_calibration.correction_ms==188);
    // Seek resets discard absolute timestamps/observation, keep relative evidence.
    s.seek_paused=1;
    _stream_sync_mode2_heard_reset_locked(&s,1,1);
    assert(!s.mode2_seek_calibration.pending && s.mode2_seek_calibration.valid);
    assert(seed(&s,201000)==9812);
    qualify(&s,188,230,201000); // Next seek differs by 42ms, not another 230ms.
    assert(s.mode2_seek_calibration.correction_ms==230);
    for(int i=0;i<10;i++) {
        int64_t now=203000+i*2000;
        assert(seed(&s,now)==9770);
        qualify(&s,230,230,now);
        assert(s.mode2_seek_calibration.correction_ms==230);
    }
    STREAM good=s;
    // All reuse keys must match; no calibration leaks to a different output.
    for(int fault=0;fault<8;fault++) {
        s=good;
        if(fault==0) s.audio_lifecycle_generation++;
        if(fault==1) s.audio->format++;
        if(fault==2) s.audio->samplesPerSec=44100;
        if(fault==3) s.av_delay=20;
        if(fault==4) stream_dbg_delay=20;
        int delay=fault==5?610:609, fixed=fault==6?200:199;
        int64_t now=fault==7?600000:230000;
        assert(_stream_mode2_seek_seed_locked(&s,10000,delay,fixed,now)==10000);
        s.audio->format=0x4747; s.audio->samplesPerSec=48000;
        stream_dbg_delay=0;
    }
    // Do not learn from paused, clamped, stale, underrun or implausible samples.
    for(int fault=0;fault<11;fault++) {
        init(&s); seed(&s,1000);
        if(fault==0) s.paused=1;
        if(fault==1) s.paused_internal=1;
        if(fault==2) s.av_delay=20;
        if(fault==3) s.audio_lifecycle_generation++;
        if(fault==4) s.presentation_observation.underrun_count=1;
        if(fault==5) s.audio->samplesPerSec=44100;
        if(fault==6) s.audio->format++;
        if(fault==10) s.mode2_heard_interp_delay_ms=700;
        int64_t now=fault==7?2360:1965;
        int heard=fault==8?10900:10965; // Starvation/clamping broke wall pacing.
        int measured=fault==9?10465:10765; // 500ms is outside the safety cap.
        _stream_mode2_seek_calibrate_locked(&s,now,heard,measured);
        assert(!s.mode2_seek_calibration.valid && !s.mode2_seek_calibration.pending);
    }
    init(&s); seed(&s,1000); qualify(&s,0,-100,1000);
    assert(s.mode2_seek_calibration.valid);
    assert(seed(&s,3000)==10100); // Earlier presentation is handled symmetrically.
    s.seek_paused=0;
    _stream_sync_mode2_heard_reset_locked(&s,1,1);
    assert(!s.mode2_seek_calibration.valid); // Non-seek flush invalidates reuse.
    return 0;
}
''')

    def test_preview_publication_and_refill(self):
        root = Path(__file__).resolve().parents[1]
        codec = (root / 'Source/codec_sfdec2.c').read_text()
        sync = (root / 'Source/stream_sync.c').read_text()
        preview = block(codec, 'if (passthrough == 2 && !libavos_get_ac3_recoding_enabled()')
        publish = block(codec, 'if( s && time >= 0 && !s->paused && !s->seek_paused &&')
        catchup = block(sync, 'if( raw_heard_ts > s->mode2_heard_interp_ts &&')
        fields = sorted(set(re.findall(r'p->(\w+)', publish)))
        code = r'''
#include <stdint.h>
#include <assert.h>
typedef int64_t INT64;
#define STREAM_NO_PTS_VALUE INT32_MIN
#define DBGSI if (0)
#define serprintf_record(...) ((void)0)
typedef struct {
    int paused, seek_paused, play_n_video_frames, seek_video_drop;
    int seek_video_target_ts, seek_video_target_pending, seek_video_ready_ts, seek_epoch;
    int mode2_heard_interp_ts;
    int64_t mode2_seek_refill_until_ms;
} Stream;
typedef struct { int time, epoch; } Frame;
typedef struct { int venc_q; } Queue;
typedef struct { Queue locked;
''' + ''.join('int64_t ' + name + ';\n' for name in fields) + r'''
} Priv;
static Frame frame;
static int recoding;
static int libavos_get_ac3_recoding_enabled(void) { return recoding; }
static Frame *frame_q_get(int *q) { ++*q; return &frame; }
static int discard(Stream *s, Priv *p, int passthrough, int time) {
    Frame *f = &frame;
    f->time = time; f->epoch = s->seek_epoch;
    int consumed = 0, stale_epoch_drop = 0;
    do {
''' + preview + r'''
    } while (0);
    assert(consumed == stale_epoch_drop);
    return consumed;
}
static void anchor(Stream *s, Priv *p, int passthrough_mode, int time) {
    int64_t published_clock_ns = 10000000000LL;
''' + publish + r'''
}
static void refill(Stream *s, int raw_heard_ts, int64_t wall_now) {
''' + catchup + r'''
}
int main(void) {
    Stream s = {.seek_video_drop=1, .seek_video_target_ts=135550,
        .seek_video_ready_ts=135552, .seek_epoch=2};
    Priv p = {.pending_seek_reanchor=1, .render_offset_ns=-1};
    assert(discard(&s, &p, 2, 133091));
    assert(p.pending_seek_reanchor && p.render_offset_ns == -1);
    assert(!discard(&s, &p, 2, 135552));
    assert(!discard(&s, &p, 0, 133091));
    assert(!discard(&s, &p, 1, 133091));
    recoding=1;
    assert(!discard(&s, &p, 2, 133091));
    anchor(&s, &p, 2, 135353);
    assert(p.render_offset_ns == -1);
    recoding=0;
    s.seek_paused=1;
    assert(!discard(&s, &p, 2, 133091));
    anchor(&s, &p, 2, 135353);
    assert(p.render_offset_ns == -1);
    s.seek_paused=0; s.play_n_video_frames=1;
    assert(!discard(&s, &p, 2, 133091));
    anchor(&s, &p, 2, 135353);
    assert(p.render_offset_ns == -1);
    s.play_n_video_frames=0; s.seek_video_target_pending=1;
    anchor(&s, &p, 2, 135353);
    assert(p.render_offset_ns == -1);
    s.seek_video_target_pending=0;
    s.seek_video_ready_ts=STREAM_NO_PTS_VALUE;
    anchor(&s, &p, 2, 135353);
    assert(p.render_offset_ns == -1);
    s.seek_video_ready_ts=135552;
    anchor(&s, &p, 2, -1);
    assert(p.render_offset_ns == -1);
    anchor(&s, &p, 0, 135353);
    assert(p.render_offset_ns == -1);
    anchor(&s, &p, 2, 135353);
    assert(p.render_offset_ns == 10000000000LL - 135353000000LL);
    assert(!p.pending_seek_reanchor && p.render_offset_from_audio);
    int64_t offset = p.render_offset_ns;
    anchor(&s, &p, 2, 135600); // refill must not publish a second anchor
    assert(p.render_offset_ns == offset);
    s.mode2_heard_interp_ts=1000; s.mode2_seek_refill_until_ms=2359;
    refill(&s, 1209, 1050); // accepted burst is not a presentation jump
    assert(s.mode2_heard_interp_ts == 1000);
    refill(&s, 1400, 1610); // static 609ms expires before presentation qualifies
    assert(s.mode2_heard_interp_ts == 1000);
    refill(&s, 1400, 1984); // Bravia direct evidence enters at ~984ms
    assert(s.mode2_heard_interp_ts == 1000);
    refill(&s, 1400, 2317); // measured frontier catches up at ~1317ms
    assert(s.mode2_heard_interp_ts == 1000);
    assert(s.mode2_seek_refill_until_ms == 2359); // writes cannot renew the bound
    refill(&s, 1400, 2359); // absent evidence still gets bounded fallback
    assert(s.mode2_heard_interp_ts == 1400);
    s.mode2_seek_refill_until_ms=0;
    refill(&s, 1800, 1611); // existing track-recreation policy
    assert(s.mode2_heard_interp_ts == 1800);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'seek.c'
            binary = Path(directory) / 'seek'
            source.write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()

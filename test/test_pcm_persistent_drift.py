"""Qualify one clock step without confusing write bursts or lifecycle transitions."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_video_pcm_startup import block


class PcmDriftTest(unittest.TestCase):
    def test_persistent_step_and_exclusions(self):
        source = (Path(__file__).resolve().parents[1] / 'Source/codec_sfdec2.c').read_text()
        helper = block(source, 'static int pcm_drift_observe(')
        end = source.index('} pcm_drift_t;') + len('} pcm_drift_t;')
        state = source[source.rfind('typedef struct {', 0, end):end]
        start = source.index('int pcm_drift_eligible =')
        gate = source[start:source.index(';', start) + 1]
        fields = sorted(set(re.findall(r'p->(\w+)', gate)) - {'speed_cadence'})
        private = '\n'.join('INT64 ' + name + ';' for name in fields)
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
typedef int64_t INT64;
''' + state + '\n' + helper + r'''
typedef struct { int valid; } AUDIO;
typedef struct {
    AUDIO *audio;
    int put_time_mode, audio_time, paused, seek_paused, audio_start_pending;
    int audio_resume_pending, play_n_video_frames, av_delay, manual_audio_delay_applied_ms;
} STREAM;
typedef struct {
    struct { INT64 correction_ns; } speed_cadence;
''' + private + r'''
} priv_t;
static int eligible(priv_t *p, STREAM *s, int passthrough_mode, int transition,
    float requested_speed, float current_speed) {
    int time=1000, manual_hold_ms=0, speed_changed=transition, epoch_changed=0;
    int resume_started=0, pcm_resume=0, in_grace=0, allow_reanchor=0;
''' + gate + r'''
    return pcm_drift_eligible;
}
static int observe(pcm_drift_t *d,int allowed,int step,int wall,int phase,INT64 *target) {
    return pcm_drift_observe(d,allowed,step,(INT64)(10000+wall)*1000000,
        10000+wall-phase,0,160,350,target);
}
int main(void) {
    pcm_drift_t d={0}; INT64 target=0;
    // A single step followed by perfectly normal increments still recovers.
    for(int sign=-1;sign<=1;sign+=2) {
        memset(&d,0,sizeof(d));
        assert(!observe(&d,1,-sign*250,0,sign*250,&target));
        for(int t=50;t<500;t+=50)
            assert(!observe(&d,1,0,t,sign*250,&target));
        assert(observe(&d,1,0,500,sign*250,&target));
        assert(target==sign*250000000LL && !d.samples);
    }
    // Reversed burst, scheduler gap, or explicit lifecycle reset invalidates it.
    for(int kind=0;kind<3;kind++) {
        memset(&d,0,sizeof(d));
        assert(!observe(&d,1,-250,0,250,&target));
        assert(!observe(&d,kind!=2,0,kind==1?300:100,kind==0?0:250,&target));
        assert(!d.samples);
        assert(!observe(&d,1,0,600,250,&target));
    }
    // Fast duplicate observations cannot satisfy elapsed-time qualification.
    assert(!observe(&d,1,-250,0,250,&target));
    for(int i=0;i<10000;i++) assert(!observe(&d,1,0,1,250,&target));
    assert(d.samples==1);
    // Persistent bias without a new step and hard discontinuities are untouched.
    memset(&d,0,sizeof(d));
    assert(!observe(&d,1,0,0,250,&target) && !d.samples);
    assert(!observe(&d,1,-400,0,400,&target) && !d.samples);
    assert(!observe(&d,1,250,0,250,&target) && !d.samples);
    AUDIO audio={1}; STREAM s={.audio=&audio,.put_time_mode=1,.audio_time=1000};
    priv_t p={.render_offset_from_audio=1,.render_offset_ns=1000};
    assert(eligible(&p,&s,0,0,1.2f,1.2f));
    for(int mode=1;mode<=3;mode++) assert(!eligible(&p,&s,mode,0,1,1));
    assert(!eligible(&p,&s,0,1,1,1));
    assert(!eligible(&p,&s,0,0,1.6f,1.2f));
    s.paused=1; assert(!eligible(&p,&s,0,0,1,1)); s.paused=0;
    s.seek_paused=1; assert(!eligible(&p,&s,0,0,1,1)); s.seek_paused=0;
    s.av_delay=-100; assert(!eligible(&p,&s,0,0,1,1)); s.av_delay=0;
    p.slew_active=1; assert(!eligible(&p,&s,0,0,1,1)); p.slew_active=0;
    p.pcm_resume_pending=1; assert(!eligible(&p,&s,0,0,1,1)); p.pcm_resume_pending=0;
    p.pending_seek_reanchor=1; assert(!eligible(&p,&s,0,0,1,1)); p.pending_seek_reanchor=0;
    p.speed_cadence.correction_ns=1; assert(!eligible(&p,&s,0,0,1,1));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'drift.c'
            path.write_text(code)
            binary = Path(directory) / 'drift'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

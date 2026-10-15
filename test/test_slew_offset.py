"""Check extracted renderer arithmetic against its frozen predecessor and lifecycle rules."""
import re
import unittest

from native_replay import ROOT, extract, run_c, source
from test_video_pcm_startup import slew_helpers


def typedef(text, name):
    end = text.index('} ' + name + ';') + len('} ' + name + ';')
    return text[text.rfind('typedef struct {', 0, end):end]


class SlewOffsetTest(unittest.TestCase):
    def test_steps_guards_deadbands_pause_and_seek(self):
        text = source('codec_sfdec2.c')
        functions = '\n'.join(extract(text, marker) for marker in (
            'static void speed_cadence_reset(',
            'static int mode1_phase_matches_audio_l(',
            'static int pcm_resume_matches_audio_l(',
            'static void sfdec2_reset_sync_state_on_seek_locked(',
            'static void sfdec2_android_sync_on_pause_locked('))
        guards = ''
        for name in ('mode2_new_slew_frame', 'pcm_startup_new_slew_frame'):
            start = text.index('int ' + name + ' =')
            guards += text[start:text.index(';', start) + 1] + '\n'
        step = extract(text, 'if (p->slew_active &&')
        remember = '\n'.join(extract(text, marker) for marker in (
            'if( p->mode2_dynamic_slew && mode2_new_slew_frame )',
            'if( (p->pcm_startup_slew || p->pcm_resume_calibration_active || p->mode1_resume_slew) && pcm_startup_new_slew_frame )'))
        start = text.index('const INT64 mode2_deadband_ns =')
        end = text.index('p->pending_seek_reanchor = 0;', start) + len('p->pending_seek_reanchor = 0;')
        settle = text[start:end]
        fields = set(re.findall(r'p->(\w+)', functions + guards + step + remember + settle)) - {'locked'}
        private_source = text[text.index('typedef struct priv {'):text.index('} priv_t;')]
        declarations = []
        for name in sorted(fields):
            match = re.search(r'^\s*([\w \t*]+\b' + name + r')\s*;', private_source, re.M)
            if not match:
                raise AssertionError('Missing renderer field declaration: ' + name)
            declarations.append(match[1].strip() + ';')
        run_c(r'''
#define CONFIG_SFDEC
#include "global.h"
#include "stream.h"
#include "stream_dec_video.h"
#include "audio_interface.h"
#include "util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define DBGSI if (0)
#define serprintf printf
static int64_t now_ms=10000;
int64_t atime64(void) { return now_ms; }
static timeline_state_t timeline={.speed=1,.inv_speed=1};
timeline_state_t timeline_snapshot(void) { return timeline; }
static int mode, preserves=1;
static int passthrough(STREAM *s) { (void)s; return mode; }
int audio_interface_pause_preserves_output(audio_ctx_t *ctx) { (void)ctx; return preserves; }
''' + typedef(text, 'speed_cadence_t') + '\n' + typedef(text, 'pcm_drift_t') + r'''
typedef struct {
    struct { pthread_mutex_t mtx; } locked;
''' + '\n'.join(declarations) + '\n} priv_t;\n' + slew_helpers(text) + functions + r'''
static void advance(priv_t *p,STREAM *s,VIDEO_FRAME *f,int mode2_dynamic_active) {
''' + guards + step + remember + r'''
}
''' + (ROOT / 'test/fixtures/slew_before_refactor.c').read_text() + r'''
static void qualify_target(priv_t *p,int mode2_dynamic_active) {
''' + settle + r'''
}
static void arm(priv_t *p,int policy,INT64 delta) {
    p->render_offset_ns=1000000000LL; p->target_offset_ns=p->render_offset_ns+delta;
    p->slew_active=1; p->render_offset_from_audio=1;
    p->pcm_startup_slew=policy==1 || policy==2;
    p->pcm_resume_slew=policy==2;
    p->mode1_resume_slew=policy==3;
    p->mode2_dynamic_slew=policy==4 || policy==5;
    p->mode2_dynamic_fast_slew=policy==4;
    p->pcm_startup_slew_frame_time=p->mode2_slew_frame_time=INT_MIN;
}
int main(void) {
    AUDIO_PROPERTIES audio={.valid=1}; VIDEO_PROPERTIES video={.msPerFrame=40};
    STREAM_SINK_AUDIO as={.get_passthrough=passthrough};
    STREAM s={.audio=&audio,.video=&video,.audio_sink=&as,.audio_time=1000,.video_time=1000};
    pthread_mutex_init(&s.audio_lifecycle_mutex,NULL);
    // Compare the complete old and new step paths, including guards and cleanup.
    // Cover overlapping flags too: do not assume mode flags are exclusive.
    const INT64 deltas[]={0,1,199999,200000,300000,400001,999999,1000000,
        1500000,2000001,3999999,4000000,6000000,9999999,150000000};
    const int durations[]={-1,0,1,8,20,40,80};
    for(int flags=0;flags<128;flags++) for(int sign=-1;sign<=1;sign+=2)
    for(unsigned d=0;d<sizeof(deltas)/sizeof(*deltas);d++) {
        priv_t actual={0}, expected;
        arm(&actual,0,sign*deltas[d]);
        actual.pcm_startup_slew=!!(flags&1);
        actual.pcm_resume_slew=!!(flags&2);
        actual.mode1_resume_slew=!!(flags&4);
        actual.mode2_dynamic_slew=!!(flags&8);
        actual.mode2_dynamic_fast_slew=!!(flags&16);
        actual.pcm_resume_calibration_active=!!(flags&32);
        actual.pause_armed=!!(flags&64);
        actual.mode2_dynamic_settle_frames=3;
        memcpy(&expected,&actual,sizeof(actual));
        VIDEO_FRAME f={.time=10,.epoch=1};
        for(unsigned i=0;i<sizeof(durations)/sizeof(*durations);i++) {
            f.duration=durations[i]; f.time++; f.epoch+=i%2;
            s.paused=i%2;
            for(int retry=0;retry<2;retry++) {
                advance(&actual,&s,&f,i%2);
                advance_reference(&expected,&s,&f,i%2);
                assert(memcmp(&actual,&expected,sizeof(actual))==0);
            }
        }
    }
    s.paused=0;
    // Exact step/remainder outcomes in both directions for every bounded policy.
    for(int policy=1;policy<=5;policy++) for(int sign=-1;sign<=1;sign+=2) {
        priv_t p={0}; VIDEO_FRAME f={.time=10,.epoch=1,.duration=40};
        INT64 increment=policy==1?1000000:policy==4?5000000:policy==5?200000:4000000;
        INT64 initial=1000000000LL, delta=sign*(increment+increment/2);
        arm(&p,policy,delta);
        advance(&p,&s,&f,policy>=4);
        assert(p.render_offset_ns==initial+sign*increment && p.slew_active);
        // A retry cannot spend the half-step remainder on the same frame.
        advance(&p,&s,&f,policy>=4);
        assert(p.render_offset_ns==initial+sign*increment);
        f.time++; advance(&p,&s,&f,policy>=4);
        assert(p.render_offset_ns==initial+delta && !p.slew_active);
    }
    // Preserve the source's unflagged fallback semantics, without assuming reachability.
    {
        priv_t p={0}; VIDEO_FRAME f={.duration=40}; arm(&p,0,300000);
        advance(&p,&s,&f,0); assert(p.render_offset_ns==1000300000 && !p.slew_active);
    }
    for(int policy=2;policy<=3;policy++) {
        priv_t p={0}; VIDEO_FRAME f={.time=1,.epoch=1,.duration=40};
        arm(&p,policy,100000000);
        const int durations[]={80,40,20,8,0,-1};
        const INT64 increments[]={4000000,4000000,2000000,800000,1000000,1000000};
        for(int i=0;i<6;i++) {
            INT64 before=p.render_offset_ns; f.duration=durations[i]; f.time++;
            advance(&p,&s,&f,0); assert(p.render_offset_ns-before==increments[i]);
        }
        // Identity includes time, epoch and handle, not just frame address.
        INT64 before=p.render_offset_ns; f.epoch++;
        advance(&p,&s,&f,0); assert(p.render_offset_ns-before==1000000);
        before=p.render_offset_ns; f.android_handle=&s;
        advance(&p,&s,&f,0); assert(p.render_offset_ns-before==1000000);
    }
    // Target deadbands are separate from remainder completion.
    {
        priv_t p={0}; arm(&p,1,8000000); qualify_target(&p,0);
        assert(!p.slew_active && p.target_offset_ns==p.render_offset_ns);
        arm(&p,1,8000001); qualify_target(&p,0); assert(p.slew_active);
        arm(&p,4,8000000); qualify_target(&p,1); assert(!p.slew_active);
        arm(&p,5,24000000); qualify_target(&p,1); assert(!p.slew_active);
        arm(&p,5,24000001); qualify_target(&p,1); assert(p.slew_active);
    }
    for(int policy=1;policy<=5;policy++) {
        priv_t p={0}; pthread_mutex_init(&p.locked.mtx,NULL);
        STREAM_SINK_VIDEO vs={.name="sfdec2",.priv=&p};
        STREAM_DEC_VIDEO vd={.name="sfdec2",.priv=&p,.is_open=1};
        s.video_sink=&vs; s.video_dec=&vd; s.seek_paused=0; s.seek_epoch=1;
        mode=policy==3?1:policy>=4?2:0;
        arm(&p,policy,150000000);
        p.video_timeline=p.pcm_resume_timeline=timeline;
        p.pcm_resume_epoch=s.seek_epoch;
        p.mode1_phase_valid=1; p.venc_ref_time=now_ms-10;
        p.last_mode2_dynamic_active=policy>=4;
        INT64 old_ref=p.venc_ref_time;
        s.paused=1; sfdec2_android_sync_on_pause_locked(&s,1);
        if(policy==1) assert(!p.pcm_startup_slew && !p.slew_active); // Cold start cancels.
        if(policy==2 || policy==3) {
            assert(!p.slew_active);
            VIDEO_FRAME f={.duration=40,.time=1}; advance(&p,&s,&f,0);
            assert(p.render_offset_ns==1000000000LL);
        }
        now_ms+=3000;
        sfdec2_android_sync_on_pause_locked(&s,0); s.paused=0;
        assert(p.target_offset_ns==4150000000LL);
        assert(p.venc_ref_time==old_ref+3000 && !p.pause_armed);
        if(policy==2 || policy==3) {
            // Retained resumes repay at most two frames at the pause boundary.
            assert(p.render_offset_ns==4080000000LL && p.slew_active);
            assert(p.target_offset_ns-p.render_offset_ns==70000000LL);
        } else assert(p.render_offset_ns==4000000000LL);
        if(policy>=4) assert(p.mode2_dynamic_fast_slew);
        s.seek_paused=1; p.pause_armed=1; p.pause_start_ms=now_ms-500;
        sfdec2_reset_sync_state_on_seek_locked(&s);
        assert(p.render_offset_ns==-1 && !p.slew_active && !p.pcm_resume_slew);
        assert(!p.pcm_startup_slew && !p.mode1_resume_slew && p.pending_seek_reanchor);
        assert(p.pause_start_ms==now_ms); // Pre-seek pause time is not reapplied.
        pthread_mutex_destroy(&p.locked.mtx);
    }
    // A target cannot survive replacement of its audio output, even at the same speed.
    for(int policy=2;policy<=3;policy++) {
        priv_t p={0}; pthread_mutex_init(&p.locked.mtx,NULL);
        STREAM_SINK_VIDEO vs={.name="sfdec2",.priv=&p};
        STREAM_DEC_VIDEO vd={.name="sfdec2",.priv=&p,.is_open=1};
        s.video_sink=&vs; s.video_dec=&vd; s.seek_paused=0;
        mode=policy==3?1:0; s.audio_lifecycle_generation=2;
        arm(&p,policy,150000000);
        p.video_timeline=p.pcm_resume_timeline=timeline;
        p.pcm_resume_epoch=s.seek_epoch; p.mode1_phase_valid=1;
        p.mode1_phase_audio_generation=p.pcm_resume_audio_generation=1;
        sfdec2_android_sync_on_pause_locked(&s,1);
        assert(!p.slew_active && !p.pcm_resume_slew && !p.mode1_resume_slew);
        pthread_mutex_destroy(&p.locked.mtx);
    }
    pthread_mutex_destroy(&s.audio_lifecycle_mutex);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()

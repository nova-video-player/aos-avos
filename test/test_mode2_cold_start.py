"""Replay the first direct Mode 2 clock handoff and its lifecycle boundaries."""
import unittest

from native_replay import extract, run_c, source


class Mode2ColdStartTest(unittest.TestCase):
    def test_first_handoff_and_scope(self):
        renderer = source('codec_sfdec2.c')
        run_c(r'''
#define CONFIG_SFDEC
#include "global.h"
#include "stream.h"
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#define NSEC_PER_MSEC 1000000LL
#define DBGSI if(0)
#define serprintf_record(...) ((void)0)
typedef struct {
    int mode2_cold_entry_seen, mode2_cold_entry_epoch;
    unsigned int mode2_cold_entry_audio_generation;
    int64_t mode2_cold_entry_until_ms;
    int pause_armed, last_mode2_dynamic_active, render_offset_from_audio;
    int pending_seek_reanchor, slew_active, pending_reanchor;
    int mode2_dynamic_slew, mode2_dynamic_fast_slew, mode2_dynamic_settle_frames;
    void *mode2_slew_frame_handle;
    int mode2_slew_frame_time, mode2_slew_frame_epoch;
    INT64 render_offset_ns, target_offset_ns;
} priv_t;
static int recode;
static INT64 heard;
static int libavos_get_ac3_recoding_enabled(void) { return recode; }
static INT64 _get_render_heard_ts(priv_t *p, STREAM *s, int allow, int *a, int *b) {
    (void)p; (void)s; (void)a; (void)b; assert(!allow); return heard;
}
''' + extract(renderer, 'static int mode2_cold_entry_l(')
            + extract(renderer, 'static INT64 mode2_cold_entry_limit_ns(')
            + extract(renderer, 'static INT64 mode2_clock_entry_limit_ns(') + r'''
static void transition(priv_t *p, STREAM *s, VIDEO_FRAME *f,
                       int mode2_dynamic_active, INT64 now_ns, int passthrough) {
    int mode2_cold_entry = mode2_cold_entry_l(p, s, passthrough, now_ns);
    int have_audio_time = s->audio_time >= 0;
    int mode2_dynamic_changed = mode2_dynamic_active != p->last_mode2_dynamic_active;
''' + extract(renderer, 'if( mode2_dynamic_changed )') + r'''
}
int main(void) {
    AUDIO_PROPERTIES audio={.valid=1};
    STREAM s={.audio=&audio,.audio_time=2000,.seek_epoch=2,.audio_lifecycle_generation=7};
    VIDEO_FRAME f={.duration=17};
    priv_t p={0};
    s.seek_paused=1;
    assert(!mode2_cold_entry_l(&p,&s,2,1000000000LL));
    assert(!p.mode2_cold_entry_seen); // Startup preview is not playback.
    s.seek_paused=0;
    assert(mode2_cold_entry_l(&p,&s,2,1000000000LL));
    p.render_offset_ns=1000000000LL; p.render_offset_from_audio=1;
    p.mode2_slew_frame_handle=(void*)1;
    heard=1520; // 3s - 1.520s - 1s = the observed +480ms residual.
    transition(&p,&s,&f,1,3000000000LL,2);
    assert(p.render_offset_ns==1000000000LL);
    assert(p.pending_reanchor && p.mode2_dynamic_fast_slew);
    assert(p.mode2_slew_frame_handle==(void*)1); // Keep per-frame retry guard.
    assert(!p.mode2_cold_entry_until_ms);
    transition(&p,&s,&f,0,3010000000LL,2);
    heard=1540;
    transition(&p,&s,&f,1,3020000000LL,2);
    assert(p.render_offset_ns==-1); // Later re-entry retains the old 350ms cap.

    for(int reason=0;reason<9;reason++) {
        p=(priv_t){0}; recode=0; s.seek_epoch=2; s.audio_lifecycle_generation=7;
        s.paused=s.paused_internal=s.av_delay=0; audio.valid=1;
        assert(mode2_cold_entry_l(&p,&s,2,1000000000LL));
        int mode=2;
        if(reason==0) s.seek_epoch++;
        if(reason==1) s.audio_lifecycle_generation++;
        if(reason==2) s.paused=1;
        if(reason==3) s.paused_internal=1;
        if(reason==4) s.av_delay=100;
        if(reason==5) recode=1;
        if(reason==6) mode=1;
        if(reason==7) audio.valid=0;
        if(reason==8) p.pause_armed=1;
        assert(!mode2_cold_entry_l(&p,&s,mode,1100000000LL));
        recode=0; s.paused=s.paused_internal=s.av_delay=0; audio.valid=1; p.pause_armed=0;
        assert(!mode2_cold_entry_l(&p,&s,2,1200000000LL)); // Cancellation is final.
    }
    p=(priv_t){0};
    assert(mode2_cold_entry_l(&p,&s,2,1000000000LL));
    assert(!mode2_cold_entry_l(&p,&s,2,6000000000LL));
    assert(!mode2_cold_entry_l(&p,&s,2,6100000000LL));
    for(int mode=0;mode<3;mode++) {
        p=(priv_t){0}; recode=(mode==2);
        assert(!mode2_cold_entry_l(&p,&s,mode,1000000000LL));
        recode=0;
        assert(!mode2_cold_entry_l(&p,&s,2,1100000000LL));
    }
    p=(priv_t){0}; p.render_offset_ns=1000000000LL;
    p.render_offset_from_audio=1; p.pending_seek_reanchor=1;
    heard=1520;
    transition(&p,&s,&f,1,3000000000LL,2);
    assert(p.render_offset_ns==-1); // Never override the seek anchor policy.

    // nova-23.log: +428.238ms at 29.97fps used to exceed the cold budget.
    // Only first direct startup qualifies; recoding, a seek and re-entry
    // still take the original bounded policy for exactly the same residual.
    f.duration=33; heard=1571;
    for(int reason=0;reason<4;reason++) {
        p=(priv_t){0};
        p.render_offset_ns=1000762000LL; p.render_offset_from_audio=1;
        recode=reason==1;
        p.pending_seek_reanchor=reason==2;
        p.mode2_cold_entry_seen=reason==3;
        transition(&p,&s,&f,1,3000000000LL,2);
        if(!reason) {
            assert(p.render_offset_ns==1000762000LL);
            assert(p.pending_reanchor && p.mode2_dynamic_fast_slew);
        } else {
            assert(p.render_offset_ns==-1);
        }
    }
    return 0;
}
''')

    def test_correction_budget(self):
        renderer = source('codec_sfdec2.c')
        policy = renderer[renderer.index('typedef struct {\n\tint64_t step_ns;'):
                          renderer.index('} offset_slew_policy_t;') + len('} offset_slew_policy_t;')]
        run_c(r'''
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
typedef int64_t INT64;
''' + extract(renderer, 'static INT64 mode2_cold_entry_limit_ns(')
            + policy + extract(renderer, 'static int offset_slew_step(') + r'''
int main(void) {
    assert(mode2_cold_entry_limit_ns(0)==100000000LL);
    assert(mode2_cold_entry_limit_ns(17)>=480058000LL);
    assert(mode2_cold_entry_limit_ns(33)>=428238000LL);
    assert(mode2_cold_entry_limit_ns(34)>=428238000LL);
    assert(mode2_cold_entry_limit_ns(42)<480058000LL); // Too slow to repay in 4s.
    // Repay the observed residual using the actual slew arithmetic. A nominal
    // 33.367ms frame deadline gains at most 5ms, never the previous 428ms snap.
    int64_t observed_offset=0;
    offset_slew_policy_t observed_policy={.step_ns=5000000};
    int observed_frames=0;
    do {
        int64_t old=observed_offset;
        offset_slew_step(&observed_offset,428238000LL,observed_policy,33);
        assert(observed_offset-old<=5000000LL);
        observed_frames++;
    } while(observed_offset!=428238000LL);
    assert(observed_frames==86);
    assert(observed_frames*33367000LL+observed_offset<4000000000LL);
    for(int frame=8;frame<=50;frame++) {
        INT64 limit=mode2_cold_entry_limit_ns(frame);
        assert(limit<=750000000LL);
        for(int sign=-1;sign<=1;sign+=2) {
            int64_t offset=0, target=sign*limit;
            offset_slew_policy_t policy={.step_ns=5000000};
            int frames=0;
            do {
                int64_t old=offset;
                offset_slew_step(&offset,target,policy,frame);
                assert(llabs(offset-old)<=5000000LL);
                assert(frame*1000000LL+offset-old>0); // Deadlines remain ordered.
                frames++;
            } while(offset!=target);
            assert(frames*(frame+5)<=4000);
        }
    }
    return 0;
}
''')

    def test_lifecycle_cancels_without_rearming(self):
        renderer = source('codec_sfdec2.c')
        for name in ('static int videodec_flush(',
                     'static void sfdec2_reset_sync_state_on_seek_locked(',
                     'static void sfdec2_android_sync_on_pause_locked('):
            function = extract(renderer, name)
            self.assertIn('p->mode2_cold_entry_until_ms = 0;', function)
            self.assertNotIn('p->mode2_cold_entry_seen = 0;', function)


if __name__ == '__main__':
    unittest.main()

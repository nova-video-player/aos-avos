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

"""Replay accumulated PCM lag without relaxing lifecycle or burst exclusions."""
import json
import unittest

from native_replay import ROOT, extract, run_c, source, typedef


class PcmDriftHistoryTest(unittest.TestCase):
    def helpers(self):
        text = source('codec_sfdec2.c')
        return r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef int64_t INT64;
static const char *event;
''' + typedef(text, 'pcm_drift_t') + '\n' + typedef(text, 'pcm_drift_history_t') + '\n' + extract(text, 'static int pcm_drift_history_observe(')

    def test_observed_lag_and_exclusions(self):
        run_c(self.helpers() + r'''
static INT64 target;
static int sample(pcm_drift_history_t *h,int wall,int phase) {
    return pcm_drift_history_observe(h,1,0,(INT64)(10000+wall)*1000000,
        10000+wall-phase,0,8,1,160,&target,&event);
}
static void learn(pcm_drift_history_t *h) {
    memset(h,0,sizeof(*h));
    for(int t=0;t<=1200;t+=50) assert(!sample(h,t,-35));
    assert(h->healthy);
}
int main(void) {
    pcm_drift_history_t h={0};
    // Reproduce small increments accumulating beyond the old 350ms ceiling.
    learn(&h);
    for(int t=1250;t<=3950;t+=50)
        assert(!sample(&h,t,-35+(t-1200)/5));
    int recovered=0;
    for(int t=4000;t<=6500;t+=50) {
        if(sample(&h,t,515)) { recovered=1; break; }
    }
    assert(recovered && target>=495000000LL && target<=515000000LL);
    // Pre-existing bias has no preceding healthy evidence, even if it is stable.
    memset(&h,0,sizeof(h));
    for(int t=0;t<5000;t+=50) assert(!sample(&h,t,250));
    // A burst that reverses cannot turn into a correction.
    learn(&h);
    for(int t=1600;t<=2000;t+=50) assert(!sample(&h,t,250));
    assert(h.healthy);
    for(int t=2050;t<4000;t+=50) assert(!sample(&h,t,-35));
    // Continuing drift cannot qualify as a settled clock.
    learn(&h);
    for(int t=1250;t<5000;t+=50) assert(!sample(&h,t,200+(t-1250)/2));
    // Isolate each reset with advancing audio unless regression is intended.
    const char *resets[]={"audio_regression","epoch_reset","output_reset",
        "anchor_reset","wall_regression","context_reset"};
    for(int kind=0;kind<6;kind++) {
        learn(&h);
        int wall=1250,heard=11250,epoch=8;
        unsigned generation=1; INT64 offset=0;
        if(kind==0) heard=11200;
        if(kind==1) epoch=9;
        if(kind==2) generation=2;
        if(kind==3) offset=1000000;
        if(kind==4) wall=1100;
        assert(!pcm_drift_history_observe(&h,kind!=5,0,
            (INT64)(10000+wall)*1000000,heard,offset,epoch,generation,160,&target,&event));
        assert(event && !strcmp(event,resets[kind]));
        assert(!h.healthy);
        for(int t=1700;t<4000;t+=50) assert(!sample(&h,t,250));
    }
    learn(&h);
    for(int t=1250;t<4000;t+=50) assert(!sample(&h,t,t-1200));
    // Negative and excessive residuals stay on the previous recovery paths.
    for(int phase=-250;phase<=800;phase+=1050) {
        learn(&h);
        for(int t=1250;t<4000;t+=50) assert(!sample(&h,t,phase));
    }
    // Duplicates cannot replace one second of independent advancing evidence.
    learn(&h);
    for(int t=1250;t<=2700;t+=50)
        assert(!sample(&h,t,-35+(t-1200)/5));
    assert(!sample(&h,2750,265));
    int samples=h.window.samples;
    assert(samples>0);
    for(int i=0;i<10000;i++) assert(!sample(&h,2750,265));
    assert(h.window.samples==samples);
    return 0;
}
''')


    def test_transient_range_rejection_preserves_witness_only(self):
        run_c(self.helpers() + r'''
static INT64 target;
static int sample(pcm_drift_history_t *h,int wall,int phase) {
    return pcm_drift_history_observe(h,1,0,(INT64)(10000+wall)*1000000,
        10000+wall-phase,0,8,1,160,&target,&event);
}
int main(void) {
    for(int negative=0;negative<2;negative++) {
        pcm_drift_history_t h={0};
        for(int t=0;t<=1200;t+=50) assert(!sample(&h,t,0));
        assert(h.healthy);
        int start=negative?1250:2050, phase=negative?-250:800;
        assert(!sample(&h,start,phase));
        assert(h.healthy && h.phase_rejected && !h.window.samples);
        assert(event && !strcmp(event,negative?"phase_out_of_range":"gap_window_reset"));
        // A held out-of-range phase never qualifies, and does not flood logs.
        for(int t=start+50;t<=start+3000;t+=50) {
            assert(!sample(&h,t,phase));
            assert(h.healthy && h.phase_rejected && !event && !h.window.samples);
        }
        // Let wall time catch up for negative phase, keeping heard time forward.
        int resumed=start+3600;
        assert(!sample(&h,resumed,300));
        assert(h.healthy && !h.phase_rejected && h.window.samples==1);
        for(int t=resumed+50;t<resumed+2000;t+=50) assert(!sample(&h,t,300));
        assert(sample(&h,resumed+2000,300) && target==300000000LL);
        // Lifecycle reset still destroys the retained witness.
        assert(!pcm_drift_history_observe(&h,0,0,0,0,0,8,1,160,&target,&event));
        assert(!h.healthy && !h.phase_rejected);
        for(int t=10000;t<=14000;t+=50) assert(!sample(&h,t,300));
    }
    return 0;
}
''')

    def test_owned_correction_clears_previous_phase_rejection(self):
        run_c(self.helpers() + r'''
int main(void) {
    for(int gap=0;gap<2;gap++) {
        pcm_drift_history_t h={0}; INT64 target=0;
        for(int wall=10000;wall<=11200;wall+=50)
            assert(!pcm_drift_history_observe(&h,1,0,(INT64)wall*1000000,
                wall,0,8,1,160,&target,&event));
        assert(h.healthy);
        assert(!pcm_drift_history_observe(&h,1,0,11250000000LL,
            11500,0,8,1,160,&target,&event));
        assert(h.phase_rejected && !strcmp(event,"phase_out_of_range"));
        // A separate single-step recovery owns the correction. Qualification
        // is suspended, even when the first correcting observation has a gap.
        h.recovery_owned=1; h.recovery_target_ns=-250000000LL;
        int wall=gap?11600:11300;
        assert(!pcm_drift_history_observe(&h,1,1,(INT64)wall*1000000,
            wall+250,0,8,1,160,&target,&event));
        assert(h.healthy && h.recovery_owned && !h.phase_rejected && !h.window.samples);
        if(gap) assert(event && !strcmp(event,"gap_window_reset"));
        else assert(!event);
        // Once qualification resumes, a still-invalid phase is reported anew.
        assert(!pcm_drift_history_observe(&h,1,0,(INT64)(wall+50)*1000000,
            wall+300,0,8,1,160,&target,&event));
        assert(h.phase_rejected && event && !strcmp(event,"phase_out_of_range"));
    }
    return 0;
}
''')

    def test_reset_diagnostic_has_explicit_phase_validity(self):
        text = source('codec_sfdec2.c')
        block = extract(text, 'if (drift_event) DBGSI')
        types = '#include <assert.h>\n#include <stdint.h>\n#include <stddef.h>\ntypedef int64_t INT64;\n'
        types += typedef(text, 'pcm_drift_t') + '\n' + typedef(text, 'pcm_drift_history_t')
        run_c(types + r'''
#define NSEC_PER_MSEC 1000000LL
static int debug_on, calls, valid_seen;
static long long phase_seen;
#define DBGSI if(debug_on)
static void serprintf(const char *fmt,const char *event,long long gap,int valid,
    long long phase,...) { (void)fmt; (void)event; (void)gap;
    calls++; valid_seen=valid; phase_seen=phase; }
typedef struct { INT64 render_offset_ns; int render_offset_from_audio;
    pcm_drift_history_t pcm_drift_history; } priv_t;
typedef struct { int seek_epoch; unsigned audio_lifecycle_generation; } STREAM;
static void diagnostic(priv_t *p,int time) {
    STREAM *s=NULL;
    const char *drift_event="context_reset";
    INT64 drift_gap_ns=250000000,drift_now_ns=9000000000000LL;
    int pcm_history_slew=0,pcm_drift_context_eligible=0;
    float current_speed=1;
''' + block + r'''
}
int main(void) {
    priv_t p={.render_offset_ns=-1,.render_offset_from_audio=1};
    debug_on=1; diagnostic(&p,1000);
    assert(calls==1 && !valid_seen && phase_seen==0);
    p.render_offset_ns=8998750000000LL; diagnostic(&p,1000);
    assert(calls==2 && valid_seen && phase_seen==250);
    diagnostic(&p,-1); assert(!valid_seen && phase_seen==0);
    p.render_offset_from_audio=0; diagnostic(&p,1000); assert(!valid_seen);
    debug_on=0; diagnostic(&p,1000); assert(calls==4);
    return 0;
}
''')

    def test_capture_phase_replay(self):
        # Only relative clock observations, no file/device/account information.
        rows = json.loads((ROOT / 'test/fixtures/pcm_accumulated_lag.json').read_text())
        values = ','.join('{%d,%d}' % tuple(row) for row in rows)
        run_c(self.helpers() + r'''
int main(void) {
    const int rows[][2]={''' + values + r'''};
    pcm_drift_history_t h={0}; INT64 target=0; int recovered=0;
    for(unsigned i=0;i<sizeof(rows)/sizeof(*rows);i++) {
        int wall=10000+rows[i][0];
        if(pcm_drift_history_observe(&h,1,0,(INT64)wall*1000000,
            wall-rows[i][1],0,8,1,160,&target,&event)) {
            assert(rows[i][0]>10000);
            assert(target>350000000LL && target<600000000LL);
            recovered=1; break;
        }
    }
    assert(recovered);
    return 0;
}
''')

    def test_repeated_disturbances_during_owned_slew(self):
        from native_replay import slew_helpers
        text = source('codec_sfdec2.c')
        # Model the capture: a ~278ms correction is followed by two more clock
        # delays before it completes. Replay both observation and frame stepping.
        run_c(self.helpers() + '\n' + extract(text, 'static void pcm_drift_history_follow(') + r'''
#define MIN(a,b) ((a)<(b)?(a):(b))
''' + slew_helpers(text) + r'''
int main(void) {
    for(int preserve=0;preserve<=1;preserve++) {
    pcm_drift_history_t h={0};
    INT64 offset=0,target=0;
    offset_slew_policy_t policy={.step_ns=1000000};
    int active=0, corrections=0, next_frame=0, last_armed=-1;
    for(int wall=0;wall<=45000;wall+=10) {
        while(next_frame<=wall) {
            next_frame+=42;
            if(active) {
                INT64 before=offset;
                active=!offset_slew_step(&offset,target,policy,42);
                pcm_drift_history_follow(&h,before,offset,target);
                assert(offset>=before && offset-before<=1000000);
            }
        }
        if(wall%50 || (wall>=6500 && wall<6850)) continue;
        int phase=30;
        if(wall>=2000) phase=30+MIN(250,(wall-2000)/4);
        if(wall>=6500) phase+=MIN(200,(wall-6500)*2/5);
        if(wall>=16000) phase+=MIN(300,(wall-16000)*3/5);
        int correcting=preserve && active && h.recovery_owned && h.recovery_target_ns==target;
        INT64 proposed=0;
        if(pcm_drift_history_observe(&h,!active || correcting,correcting,
            (INT64)(10000+wall)*1000000,10000+wall-phase,offset,14,1,160,&proposed,&event)) {
            assert(!active && wall-last_armed>=2000);
            target=proposed;
            active=1;
            h.recovery_owned=1;
            h.recovery_target_ns=target;
            h.window.samples=0;
            corrections++;
            last_armed=wall;
        }
    }
    assert(!active && !h.recovery_owned);
    if(preserve) {
        assert(corrections==2);
        // The qualification window allows 40ms of phase variation.
        assert(offset>=740000000LL && offset<=780000000LL);
        assert(h.healthy);
    } else {
        // Previous behavior loses the witness during its first correction and
        // never recovers the remaining ~500ms, even with stable audio afterward.
        assert(corrections==1 && offset<300000000LL && !h.healthy);
    }
    }
    return 0;
}
''')

    def test_ownership_does_not_survive_other_changes(self):
        text = source('codec_sfdec2.c')
        run_c(self.helpers() + '\n' + extract(text, 'static void pcm_drift_history_follow(') + r'''
int main(void) {
    for(int kind=0;kind<9;kind++) {
        pcm_drift_history_t h={.last_ns=10000000000LL,.last_time=9750,
            .epoch=14,.audio_generation=1,.healthy=1,.recovery_owned=1,
            .recovery_target_ns=300000000LL};
        INT64 target=0,offset=1000000,now=10050000000LL;
        int heard=9800,epoch=14,eligible=1;
        unsigned generation=1;
        // Only the exact owned target and previous offset may carry history.
        pcm_drift_history_follow(&h,kind==1?1:0,offset,
            kind==2?400000000:300000000);
        if(kind==3) epoch++;
        if(kind==4) generation++;
        if(kind==5) now+=300000000;
        if(kind==6) heard=9700;
        if(kind==7) eligible=0;
        if(kind==8) now=9900000000LL;
        assert(!pcm_drift_history_observe(&h,eligible,1,now,heard,offset,
            epoch,generation,160,&target,&event));
        if(kind==0 || kind==5) assert(h.healthy && h.recovery_owned && !h.window.samples);
        else assert(!h.healthy && !h.recovery_owned);
    }
    // Completion carries the final offset but ends active-slew ownership.
    pcm_drift_history_t h={.offset_ns=299000000,.healthy=1,
        .recovery_owned=1,.recovery_target_ns=300000000};
    pcm_drift_history_follow(&h,299000000,300000000,300000000);
    assert(h.healthy && !h.recovery_owned && h.offset_ns==300000000);
    return 0;
}
''')

    def test_gap_restarts_qualification_without_forgetting_healthy_playback(self):
        run_c(self.helpers() + r'''
static INT64 target;
static int sample(pcm_drift_history_t *h,int wall,int heard) {
    return pcm_drift_history_observe(h,1,0,(INT64)(10000+wall)*1000000,
        10000+heard,0,8,1,160,&target,&event);
}
int main(void) {
    pcm_drift_history_t h={0};
    for(int t=0;t<=1200;t+=50) assert(!sample(&h,t,t));
    assert(h.healthy);
    // A 380ms gap with 85ms audio progress matches the first failure's shape.
    assert(!sample(&h,1580,1285));
    assert(h.healthy && h.window.samples==1);
    assert(!strcmp(event,"gap_window_reset"));
    for(int t=1630;t<3580;t+=50) assert(!sample(&h,t,t-295));
    assert(sample(&h,3580,3285));
    assert(target==295000000LL);
    // A gap late in qualification cannot count the preceding candidate time.
    for(int t=3630;t<=5430;t+=50) assert(!sample(&h,t,t-295));
    assert(!sample(&h,5810,5515));
    for(int t=5860;t<7810;t+=50) assert(!sample(&h,t,t-295));
    assert(sample(&h,7810,7515));
    // Repeated outages never complete a continuous two-second window.
    for(int t=8200;t<14000;t+=350) assert(!sample(&h,t,t-295));
    // Neither silence nor a fresh session with an existing bias can qualify.
    for(int t=14000;t<17000;t+=50) assert(!sample(&h,t,13700));
    memset(&h,0,sizeof(h));
    for(int t=0;t<5000;t+=50) assert(!sample(&h,t,t-295));
    assert(!h.healthy);
    // Gap preservation must not conceal an epoch, output or clock change.
    for(int kind=0;kind<5;kind++) {
        h=(pcm_drift_history_t){.healthy=1,.last_ns=10000000000LL,
            .last_time=10000,.epoch=8,.audio_generation=1};
        INT64 now=10380000000LL,offset=0;
        int heard=10085,epoch=8; unsigned generation=1;
        if(kind==0) epoch++;
        if(kind==1) generation++;
        if(kind==2) offset=1000000;
        if(kind==3) heard=9999;
        if(kind==4) now=9900000000LL;
        assert(!pcm_drift_history_observe(&h,1,0,now,heard,offset,
            epoch,generation,160,&target,&event));
        assert(!h.healthy && strcmp(event,"gap_window_reset"));
    }
    return 0;
}
''')

    def test_integration_scope(self):
        text = source('codec_sfdec2.c')
        call = text[text.index('int accumulated_drift ='):text.index('\n\tif (pcm_drift_observe', text.index('int accumulated_drift ='))]
        self.assertIn('pcm_drift_context_eligible && (!p->slew_active || pcm_history_slew)', call)
        self.assertIn('fabsf(current_speed - 1.0f) < 0.001f, pcm_history_slew', call)
        self.assertIn('s->audio_lifecycle_generation', call)
        self.assertIn('s->seek_epoch', call)
        self.assertIn('if (drift_event) DBGSI {', text)
        pause = extract(text, 'static void sfdec2_android_sync_on_pause_locked(')
        self.assertIn('memset(&p->pcm_drift_history, 0, sizeof(p->pcm_drift_history))', pause)

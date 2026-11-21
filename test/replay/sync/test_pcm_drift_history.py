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
''' + typedef(text, 'pcm_drift_t') + '\n' + typedef(text, 'pcm_drift_history_t') + '\n' + extract(text, 'static int pcm_drift_history_observe(')

    def test_observed_lag_and_exclusions(self):
        run_c(self.helpers() + r'''
static INT64 target;
static int sample(pcm_drift_history_t *h,int wall,int phase) {
    return pcm_drift_history_observe(h,1,(INT64)(10000+wall)*1000000,
        10000+wall-phase,0,8,1,160,&target);
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
    for(int t=1250;t<=1650;t+=50) assert(!sample(&h,t,250));
    for(int t=1700;t<4000;t+=50) assert(!sample(&h,t,-35));
    // Continuing drift cannot qualify as a settled clock.
    learn(&h);
    for(int t=1250;t<5000;t+=50) assert(!sample(&h,t,200+(t-1250)/2));
    // A stalled clock, backwards clock, missing observations, changed output,
    // changed anchor or seek epoch all invalidate previously healthy evidence.
    for(int kind=0;kind<7;kind++) {
        learn(&h);
        int wall=1250,heard=10000+wall-250,epoch=8;
        unsigned generation=1; INT64 offset=0;
        if(kind==0) wall=1600;
        if(kind==1) heard=1000;
        if(kind==2) epoch=9;
        if(kind==3) generation=2;
        if(kind==4) offset=1000000;
        if(kind==5) wall=1100;
        assert(!pcm_drift_history_observe(&h,kind!=6,
            (INT64)(10000+wall)*1000000,heard,offset,epoch,generation,160,&target));
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
        if(pcm_drift_history_observe(&h,1,(INT64)wall*1000000,
            wall-rows[i][1],0,8,1,160,&target)) {
            assert(rows[i][0]>10000);
            assert(target>350000000LL && target<600000000LL);
            recovered=1; break;
        }
    }
    assert(recovered);
    return 0;
}
''')

    def test_integration_scope(self):
        text = source('codec_sfdec2.c')
        call = text[text.index('int accumulated_drift ='):text.index('\n\tif (pcm_drift_observe', text.index('int accumulated_drift ='))]
        self.assertIn('pcm_drift_eligible && fabsf(current_speed - 1.0f) < 0.001f', call)
        self.assertIn('s->audio_lifecycle_generation', call)
        self.assertIn('s->seek_epoch', call)
        pause = extract(text, 'static void sfdec2_android_sync_on_pause_locked(')
        self.assertIn('memset(&p->pcm_drift_history, 0, sizeof(p->pcm_drift_history))', pause)

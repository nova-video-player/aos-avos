"""Replay the first PlaybackParams renderer handoff and lifecycle exclusions."""
import re
import json
import unittest

from native_replay import ROOT, extract, run_c, source
from test_slew_offset import typedef


class AudioTrackClockHandoffTest(unittest.TestCase):
    def test_recorded_clock_windows(self):
        text = source('codec_sfdec2.c')
        cases = json.loads((ROOT / 'test/fixtures/audiotrack_handoff_samples.json').read_text())
        replay = ''
        for case in cases:
            rows = ','.join('{%d,%d}' % tuple(row) for row in case['observations'])
            replay += r"""
    {
        const int observations[][2]={""" + rows + r"""};
        pcm_drift_t d={0}; INT64 target=0;
        int qualified=0, retries=0;
        const char *reason;
        for(unsigned i=0;i<sizeof(observations)/sizeof(*observations);i++) {
            INT64 now=(INT64)observations[i][0]*1000000;
            int result=audiotrack_handoff_observe(&d,now,observations[i][1],
                """ + str(case['offset_ns']) + r"""LL,13000000000LL,&target,&reason);
            assert(result>=0);
            if(reason) retries++;
            if(result==1) {
                assert(now-d.start_ns>=500000000LL && d.samples>=6);
                qualified=1; break;
            }
        }
        assert(qualified);
"""
            if case['case'] == 'phase_excursion':
                replay += '        assert(retries>0);\n'
            else:
                replay += '        (void)retries;\n'
            replay += '    }\n'
        run_c(r"""
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
typedef int64_t INT64;
""" + typedef(text, 'pcm_drift_t') + '\n' +
              extract(text, 'static int audiotrack_handoff_observe(') +
              '\nint main(void) {\n' + replay + '\nreturn 0; }\n')

    def test_qualification_and_one_time_transition(self):
        text = source('codec_sfdec2.c')
        start = text.index('\tint at_handoff_allowed =')
        end = text.index('\n\t// Lifecycle recovery,', start)
        transition = text[start:end]
        fields = set(re.findall(r'p->(\w+)', transition))
        private = text[text.index('typedef struct priv {'):text.index('} priv_t;')]
        declarations = []
        for name in sorted(fields):
            match = re.search(r'^\s*([\w \t*]+\b' + name + r')\s*;', private, re.M)
            self.assertIsNotNone(match, name)
            declarations.append(match[1].strip() + ';')
        start = text.index('\tint at_clock_active =')
        gate = text[start:text.index(';', start) + 1]
        run_c(r'''
#define CONFIG_SFDEC
#include "global.h"
#include "stream.h"
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define DBGSI if (0)
#define serprintf printf
static INT64 now_ns;
static INT64 _get_monotonic_ns(void) { return now_ns; }
static int enabled=1, software, recoding, epoch;
int audio_interface_is_audio_speed_enabled(void) { return enabled; }
int audio_interface_is_using_atempo(void) { return software; }
static int libavos_get_ac3_recoding_enabled(void) { return recoding; }
static int stream_audiotrack_epoch_active(STREAM *s) { (void)s; return epoch; }
''' + typedef(text, 'pcm_drift_t') + '\n' + extract(text, 'static int audiotrack_handoff_observe(') + r'''
typedef struct {
''' + '\n'.join(declarations) + r'''
} priv_t;
static void publish(priv_t *p, STREAM *s, int time, int speed_changed,
    int preserve_speed_anchor, int lifecycle, int passthrough_mode) {
    int manual_hold_ms=0, epoch_changed=lifecycle, resume_started=0;
    int pcm_resume=0, allow_reanchor=lifecycle;
''' + gate + transition + r'''
}
static void reset(priv_t *p) {
    memset(p,0,sizeof(*p));
    p->render_offset_ns=10000000000LL;
    p->target_offset_ns=p->render_offset_ns+40000000LL;
    p->render_offset_from_audio=1;
    p->slew_active=p->pcm_startup_slew=1;
}
static const char *reason;
static int observe(pcm_drift_t *d,int wall,int heard,INT64 *target) {
    return audiotrack_handoff_observe(d,(INT64)wall*1000000,heard,0,
        4000000000LL,target,&reason);
}
int main(void) {
    INT64 target=0;
    pcm_drift_t d={0};
    for(int sign=-1;sign<=1;sign+=2) {
        memset(&d,0,sizeof(d));
        for(int t=0;t<500;t+=50)
            assert(!observe(&d,1000+t,1000+t-sign*75,&target));
        assert(observe(&d,1500,1500-sign*75,&target)==1);
        assert(target==sign*75000000LL);
    }
    // Dense duplicate samples cannot replace elapsed wall time.
    memset(&d,0,sizeof(d));
    assert(!observe(&d,1000,1075,&target));
    for(int i=0;i<10000;i++) assert(!observe(&d,1001,1076,&target));
    assert(d.samples==1);
    // A stall/gap rejects the window; regression/hard drift ends the handoff.
    assert(!observe(&d,1100,1076,&target));
    assert(!strcmp(reason,"phase-jitter") && d.samples==1);
    memset(&d,0,sizeof(d));
    assert(!observe(&d,1000,1075,&target));
    assert(!observe(&d,1050,1125,&target));
    assert(observe(&d,1060,1120,&target)==-1);
    assert(!strcmp(reason,"clock-regression"));
    memset(&d,0,sizeof(d));
    assert(!observe(&d,1000,1075,&target));
    assert(!observe(&d,1250,1325,&target));
    assert(!strcmp(reason,"observation-gap") && d.samples==1);
    memset(&d,0,sizeof(d));
    assert(observe(&d,1000,1350,&target)==-1);
    assert(!strcmp(reason,"hard-discontinuity"));
    // Reject the first candidate, then require a full new stable window.
    memset(&d,0,sizeof(d));
    assert(!observe(&d,1000,1075,&target));
    assert(!observe(&d,1040,1156,&target));
    assert(!strcmp(reason,"phase-jitter") && d.samples==1);
    for(int t=1090;t<1540;t+=50) assert(!observe(&d,t,t+116,&target));
    assert(observe(&d,1540,1656,&target)==1 && target==-116000000LL);
    // Repeated phase fluctuations cannot qualify or renew the deadline.
    memset(&d,0,sizeof(d));
    for(int t=1000;t<4000;t+=50)
        assert(!observe(&d,t,t+(t%100 ? 75 : 25),&target));
    assert(observe(&d,4000,4075,&target)==-1);
    assert(!strcmp(reason,"timeout"));
    // A genuinely stalled counter never passes, despite repeated retries.
    memset(&d,0,sizeof(d));
    for(int t=1000;t<1300;t+=50) assert(!observe(&d,t,1000,&target));
    assert(observe(&d,1400,1000,&target)==-1);

    // Spaced observations from the failing Pixel capture, normalized by
    // subtracting one constant wall origin. The old startup slew was still
    // heading toward 858.116ms instead of the counter phase near 768ms.
    const int wall[]={3310,3431,3510,3630,3753,3871};
    const int heard[]={2547,2660,2749,2852,2990,3098};
    memset(&d,0,sizeof(d));
    for(int i=0;i<6;i++) {
        int result=audiotrack_handoff_observe(&d,(INT64)wall[i]*1000000,
            heard[i],842485742LL,10000000000LL,&target,&reason);
        assert(result==(i==5));
    }
    assert(target>=768000000LL && target<=769000000LL);

    STREAM stream={0}, *s=&stream;
    AUDIO_PROPERTIES audio={.valid=1}; s->audio=&audio;
    s->put_time_mode=1; s->audio_time=1000;
    priv_t p;
    reset(&p); epoch=1;
    // An audio publication can see the epoch before the speed commit.
    now_ns=11000000000LL;
    publish(&p,s,1075,0,0,0,0);
    assert(!p.at_clock_active && !p.at_clock_handoff_pending);
    publish(&p,s,1075,1,1,0,0);
    assert(p.at_clock_active && p.at_clock_handoff_pending);
    assert(!p.pcm_startup_slew && !p.slew_active);
    assert(p.render_offset_ns==10000000000LL && p.target_offset_ns==p.render_offset_ns);
    assert(!p.at_clock_handoff.samples); // Never qualify a commit-only sample.
    for(int t=0;t<=500;t+=50) {
        now_ns=11000000000LL+(INT64)t*1000000;
        if(t==250) {
            // Another tap does not renew the handoff or count as evidence.
            publish(&p,s,1075+t,1,1,0,0);
            assert(p.at_clock_handoff.start_ns==11000000000LL);
            assert(p.at_clock_handoff_deadline_ns==14000000000LL);
        }
        publish(&p,s,1075+t,0,0,0,0);
    }
    assert(!p.at_clock_handoff_pending && p.pcm_startup_slew && p.slew_active);
    assert(p.target_offset_ns==9925000000LL && p.render_offset_ns==10000000000LL);
    publish(&p,s,1575,1,1,0,0);
    assert(!p.at_clock_handoff_pending && p.target_offset_ns==9925000000LL);

    // Even commit-only traffic expires at the original three-second deadline.
    reset(&p); now_ns=11000000000LL;
    publish(&p,s,1000,1,1,0,0);
    for(int t=100;t<3000;t+=100) {
        now_ns=11000000000LL+(INT64)t*1000000;
        publish(&p,s,1000+t,1,1,0,0);
        assert(p.at_clock_handoff_pending);
        assert(p.at_clock_handoff_deadline_ns==14000000000LL);
    }
    now_ns=14000000000LL; publish(&p,s,4000,1,1,0,0);
    assert(!p.at_clock_handoff_pending && !p.slew_active);
    publish(&p,s,4000,1,1,0,0); assert(!p.at_clock_handoff_pending);

    // Neither software speed, recoding nor passthrough starts this policy.
    for(int mode=1;mode<=3;mode++) {
        reset(&p); publish(&p,s,1000,1,1,0,mode);
        assert(!p.at_clock_handoff_pending && p.pcm_startup_slew);
    }
    software=1; reset(&p); publish(&p,s,1000,1,1,0,0);
    assert(!p.at_clock_handoff_pending); software=0;
    recoding=1; reset(&p); publish(&p,s,1000,1,1,0,0);
    assert(!p.at_clock_handoff_pending); recoding=0;
    epoch=0; reset(&p); publish(&p,s,1000,1,1,0,0);
    assert(!p.at_clock_handoff_pending); epoch=1;

    // Lifecycle recovery and manual delay retain ownership of the anchor.
    int *guards[]={&s->paused,&s->paused_internal,&s->seek_paused,
        &s->audio_start_pending,&s->audio_resume_pending,&s->audio_reconfiguring,
        &s->av_delay,&s->manual_audio_delay_applied_ms};
    for(unsigned i=0;i<sizeof(guards)/sizeof(*guards);i++) {
        reset(&p); publish(&p,s,1000,1,1,0,0);
        assert(p.at_clock_handoff_pending);
        *guards[i]=1; publish(&p,s,1000,0,0,0,0); *guards[i]=0;
        assert(!p.at_clock_handoff_pending);
        publish(&p,s,1000,1,1,0,0);
        assert(!p.at_clock_handoff_pending);
    }
    reset(&p); publish(&p,s,1000,1,1,1,0);
    assert(!p.at_clock_handoff_pending && p.at_clock_active);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()

"""Replay bounded Mode 2 seek qualification and renderer admission."""
import unittest

from native_replay import extract, run_c, source


class Mode2SeekQualificationTest(unittest.TestCase):
    def test_qualification_requires_current_fresh_evidence(self):
        run_c(r'''
#define CONFIG_SFDEC
#include "global.h"
#include "stream.h"
#include "stream_sync.h"
#include <assert.h>
#include <string.h>
#define STREAM_MODE2_DIRECT_FRESH_MS 250
static int stream_dbg_delay, stream_mode2_dynamic_all=1;
''' + extract(source('stream_sync.c'), 'STREAM_MODE2_SEEK_STARTUP stream_sync_mode2_seek_startup(') + r'''
int main(void) {
    STREAM s={0};
    pthread_mutex_init(&s.mode2_heard_mutex,NULL);
    s.audio=&s.av.audio[0]; s.audio->valid=1;
    s.audio->format=WAVE_FORMAT_EAC3; s.audio->samplesPerSec=48000;
    s.seek_epoch=2; s.audio_lifecycle_generation=5;
    s.mode2_heard_frontier_seed_pending=STREAM_MODE2_FRONTIER_SEEK;
    assert(stream_sync_mode2_seek_startup(&s,1000)==STREAM_MODE2_SEEK_WAIT);
    s.mode2_heard_frontier_seed_pending=STREAM_MODE2_FRONTIER_NONE;
    assert(stream_sync_mode2_seek_startup(&s,1000)==STREAM_MODE2_SEEK_FALLBACK);
    s.mode2_seek_calibration.seek_epoch=2;
    s.mode2_seek_calibration.generation=5;
    s.mode2_seek_calibration.format=s.audio->format;
    s.mode2_seek_calibration.rate=48000;
    s.mode2_seek_calibration.pending=1;
    assert(stream_sync_mode2_seek_startup(&s,1000)==STREAM_MODE2_SEEK_WAIT);
    s.mode2_seek_calibration.pending=0; s.mode2_seek_calibration.valid=1;
    s.mode2_dynamic_clock_active=1;
    s.presentation_observation.direct_trusted=1;
    s.presentation_observation.direct_heard_wall_ms=1000;
    s.presentation_observation.observed_wall_ms=1000;
    s.presentation_observation.last_advance_wall_ms=1000;
    assert(stream_sync_mode2_seek_startup(&s,1000)==STREAM_MODE2_SEEK_WAIT);
    s.mode2_dynamic_clock_ready=1;
    assert(stream_sync_mode2_seek_startup(&s,1000)==STREAM_MODE2_SEEK_READY);
    assert(stream_sync_mode2_seek_startup(&s,1251)==STREAM_MODE2_SEEK_WAIT);
    assert(stream_sync_mode2_seek_startup(&s,999)==STREAM_MODE2_SEEK_WAIT);
    s.presentation_observation.last_advance_wall_ms=700;
    assert(stream_sync_mode2_seek_startup(&s,1000)==STREAM_MODE2_SEEK_WAIT);
    s.presentation_observation.last_advance_wall_ms=1000;
    s.presentation_observation.epoch++;
    assert(stream_sync_mode2_seek_startup(&s,1000)==STREAM_MODE2_SEEK_WAIT);
    s.presentation_observation.epoch--;
    s.presentation_observation.underrun_count=1;
    assert(stream_sync_mode2_seek_startup(&s,1000)==STREAM_MODE2_SEEK_WAIT);
    s.presentation_observation.underrun_count=0;
    int *cancel[]={&s.mode2_seek_calibration.reused,&s.paused,&s.paused_internal,
        &s.seek_paused,&s.play_n_video_frames,&s.av_delay,&stream_dbg_delay};
    for(unsigned i=0;i<sizeof(cancel)/sizeof(*cancel);i++) {
        *cancel[i]=1;
        assert(stream_sync_mode2_seek_startup(&s,1000)==STREAM_MODE2_SEEK_FALLBACK);
        *cancel[i]=0;
    }
    s.audio_lifecycle_generation++;
    assert(stream_sync_mode2_seek_startup(&s,1000)==STREAM_MODE2_SEEK_FALLBACK);
    s.audio_lifecycle_generation--;
    s.mode2_seek_calibration.seek_epoch=1;
    assert(stream_sync_mode2_seek_startup(&s,1000)==STREAM_MODE2_SEEK_FALLBACK);
    s.mode2_seek_calibration.seek_epoch=2;
    stream_mode2_dynamic_all=0;
    assert(stream_sync_mode2_seek_startup(&s,1000)==STREAM_MODE2_SEEK_FALLBACK);
    pthread_mutex_destroy(&s.mode2_heard_mutex);
    return 0;
}
''')

    def test_renderer_wait_anchor_preroll_timeout_and_scope(self):
        run_c(r'''
#define CONFIG_SFDEC
#include "global.h"
#include "stream.h"
#include "stream_sync.h"
#include <assert.h>
#include <limits.h>
#include <string.h>
#define NSEC_PER_MSEC 1000000LL
#define DBGSI if(0)
#define serprintf(...) ((void)0)
#define serprintf_record(...) ((void)0)
typedef struct {
    int mode2_seek_start_epoch;
    unsigned int mode2_seek_audio_generation;
    int64_t mode2_seek_wait_start_ms,mode2_seek_wait_until_ms;
    int mode2_seek_preroll,render_offset_from_audio,slew_active;
    int pending_reanchor,pending_seek_reanchor,last_mode2_dynamic_active;
    int mode2_dynamic_slew,mode2_dynamic_fast_slew,mode2_dynamic_settle_frames;
    void *mode2_slew_frame_handle;
    int mode2_slew_frame_time,mode2_slew_frame_epoch,snap_origin_epoch;
    INT64 render_offset_ns,target_offset_ns;
} priv_t;
static STREAM_MODE2_SEEK_STARTUP state;
static int recode,queries;
static INT64 heard;
static int libavos_get_ac3_recoding_enabled(void) { return recode; }
STREAM_MODE2_SEEK_STARTUP stream_sync_mode2_seek_startup(STREAM *s,int64_t now) {
    (void)s; (void)now; queries++; return state;
}
static INT64 _get_render_heard_ts(priv_t *p,STREAM *s,int allow,int *used,int *age) {
    (void)p; (void)s; (void)used; (void)age; assert(!allow); return heard;
}
''' + extract(source('codec_sfdec2.c'), 'static int mode2_seek_startup_l(') + r'''
int main(void) {
    STREAM s={0}; priv_t p={0}; VIDEO_FRAME f={0};
    s.seek_epoch=2; s.audio_lifecycle_generation=7;
    f.time=10000; f.duration=42; state=STREAM_MODE2_SEEK_WAIT;
    s.seek_paused=1;
    assert(!mode2_seek_startup_l(&p,&s,&f,2,1000000000LL));
    assert(!queries && !p.mode2_seek_start_epoch);
    s.seek_paused=0;
    assert(mode2_seek_startup_l(&p,&s,&f,2,1000000000LL)==1);
    assert(mode2_seek_startup_l(&p,&s,&f,2,2200000000LL)==1);
    state=STREAM_MODE2_SEEK_READY; heard=10800;
    assert(mode2_seek_startup_l(&p,&s,&f,2,2200000000LL)==2);
    assert(p.render_offset_ns==2200000000LL-10800*NSEC_PER_MSEC);
    assert(p.last_mode2_dynamic_active && !p.pending_seek_reanchor && !p.slew_active);
    f.time=10800;
    assert(!mode2_seek_startup_l(&p,&s,&f,2,2200000000LL));
    assert(!p.mode2_seek_preroll);
    INT64 anchor=p.render_offset_ns;
    state=STREAM_MODE2_SEEK_WAIT;
    assert(!mode2_seek_startup_l(&p,&s,&f,2,2300000000LL));
    assert(p.render_offset_ns==anchor); // No second wait/reanchor in this seek.
    s.seek_epoch++;
    assert(mode2_seek_startup_l(&p,&s,&f,2,3000000000LL)==1);
    assert(!mode2_seek_startup_l(&p,&s,&f,2,4500000000LL));
    assert(!p.mode2_seek_wait_until_ms && p.render_offset_ns==anchor);
    state=STREAM_MODE2_SEEK_READY;
    assert(!mode2_seek_startup_l(&p,&s,&f,2,4600000000LL));
    assert(p.render_offset_ns==anchor); // Timeout cannot rearm on late evidence.
    s.seek_epoch++; state=STREAM_MODE2_SEEK_FALLBACK;
    assert(!mode2_seek_startup_l(&p,&s,&f,2,5000000000LL));
    assert(p.render_offset_ns==anchor); // Reused calibration keeps the fast path.
    for(int mode=0;mode<3;mode++) {
        s.seek_epoch++; recode=mode==2; queries=0;
        assert(!mode2_seek_startup_l(&p,&s,&f,mode,6000000000LL));
        assert(!queries && p.render_offset_ns==anchor);
    }
    recode=0; s.seek_epoch++; state=STREAM_MODE2_SEEK_WAIT;
    assert(mode2_seek_startup_l(&p,&s,&f,2,7000000000LL)==1);
    s.paused=1;
    assert(!mode2_seek_startup_l(&p,&s,&f,2,7100000000LL));
    s.paused=0;
    assert(!mode2_seek_startup_l(&p,&s,&f,2,7200000000LL));
    s.seek_epoch++;
    assert(mode2_seek_startup_l(&p,&s,&f,2,8000000000LL)==1);
    s.audio_lifecycle_generation++;
    assert(!mode2_seek_startup_l(&p,&s,&f,2,8100000000LL));
    return 0;
}
''')

    def test_wait_releases_lease_and_rechecks_frame(self):
        renderer = source('codec_sfdec2.c')
        wait = extract(renderer, 'if( seek_startup == 1 )')
        self.assertLess(wait.index('stream_audio_read_release(s)'),
                        wait.index('pthread_cond_timedwait'))
        self.assertIn('audio_lease = 0;', wait)
        self.assertIn('goto retry_frame;', wait)
        discard = extract(renderer, 'if( seek_startup == 2 )')
        self.assertIn('stale_epoch_drop = 1;', discard)


if __name__ == '__main__':
    unittest.main()

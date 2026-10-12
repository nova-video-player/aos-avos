"""Exercise production AudioTrack checkpoint arithmetic across repeated ramps."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_video_pcm_startup import block


class AudioTrackEpochTest(unittest.TestCase):
    def test_fractional_checkpoint_continuity(self):
        root = Path(__file__).resolve().parents[1]
        sync = (root / 'Source/stream_sync.c').read_text()
        stream = (root / 'Source/stream.c').read_text()
        header = (root / 'Include/stream.h').read_text()
        helper = sync[sync.index('static pthread_mutex_t audiotrack_epoch_mutex'):
                      sync.index('static int _stream_get_heard_audio_ts_internal')]
        fields = sorted(set(re.findall(r's->(at_speed_epoch_\w+)', helper)))
        declarations = '\n'.join(re.search(
            rf'^\s*(?:unsigned int|\w+)\s+{name}\s*;', header, re.MULTILINE)[0] for name in fields)
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>
typedef uint64_t UINT64;
typedef struct {
''' + declarations + r'''
    void *audio_ctx;
} STREAM;
static UINT64 ph;
static int ph_rate=48000, query_action;
static STREAM *query_stream;
static int audio_interface_get_presented_frames(void *, UINT64 *, int *, int *, int *, int);
''' + helper + r'''
static int audio_interface_get_presented_frames(void *ctx, UINT64 *frames, int *rate,
    int *source, int *age, int fresh) {
    (void)ctx; (void)source; (void)age; assert(fresh==2);
    *frames=ph; *rate=ph_rate;
    if(query_action==1) stream_audiotrack_epoch_reset(query_stream);
    if(query_action==2) {
        unsigned g=stream_audiotrack_epoch_begin(query_stream,48,48000,1);
        stream_audiotrack_epoch_finish(query_stream,g,1);
    }
    return 1;
}
static void arm_epoch(STREAM *s, UINT64 frames, int rate, int fallback, float speed) {
    unsigned g=stream_audiotrack_epoch_begin(s,frames,rate,fallback);
    stream_audiotrack_epoch_finish(s,g,speed);
}
static STREAM racing;
static void *writer(void *arg) {
    (void)arg;
    for(int i=1;i<=20000;i++) arm_epoch(&racing,(UINT64)i*48,48000,i,1);
    return NULL;
}
static void *reader(void *arg) {
    (void)arg;
    for(int i=0;i<50000;i++) {
        double t=stream_get_audiotrack_epoch_ts(&racing,96000000,48000,0);
        assert((t>=0 && t<=20000) || t==2000000);
    }
    return NULL;
}
int main(void) {
    STREAM s = {0};
    UINT64 frames = 0;
    arm_epoch(&s, frames, 48000, 2156, 1.0f);
    long double expected = 2156;
    int legacy = 2156;
    // A zero initial playhead is valid, and repeated clock queries cannot
    // consume or modify the fractional checkpoint.
    double first = stream_get_audiotrack_epoch_ts(&s, 31, 48000, -1);
    assert(first > 2156 && first < 2157);
    assert(first == stream_get_audiotrack_epoch_ts(&s, 31, 48000, -1));
    for (int cycle = 0; cycle < 100; cycle++) {
        for (int step = 0; step < 24; step++) {
            UINT64 delta = 5301 + (cycle * 127 + step * 37) % 1000;
            if (step == 12 || step == 0)
                delta += 384000; // hold before each ramp
            float old_speed = s.at_speed_epoch_speed;
            expected += (long double)delta * 1000 / 48000 / old_speed;
            legacy += (int)((int)(delta * 1000 / 48000) / old_speed);
            frames += delta;
            float speed = step < 12 ? (105 + step * 5) / 100.0f
                                    : (155 - (step - 12) * 5) / 100.0f;
            int rounded = (int)stream_get_audiotrack_epoch_ts(&s, frames, 48000, -1);
            arm_epoch(&s, frames, 48000, rounded, speed);
            assert(fabsl(s.at_speed_epoch_heard_ts - expected) < 0.000001L);
            assert(fabsl((int)s.at_speed_epoch_heard_ts - expected) < 1.0L);
        }
    }
    assert(expected - legacy > 1000); // the old checkpoints drift cumulatively
    // A rejected speed request keeps the confirmed hardware slope, without
    // throwing away the checkpoint fraction.
    arm_epoch(&s, frames, 48000, (int)expected, 1.6f);
    s.at_speed_epoch_speed = 1.0f;
    assert(fabsl(stream_get_audiotrack_epoch_ts(&s, frames + 48, 48000, -1)
                 - (expected + 1)) < 0.000001L);
    // Reset/reconfiguration cannot reuse an incompatible checkpoint.
    assert(stream_get_audiotrack_epoch_ts(&s, frames - 1, 48000, 91) == 91);
    assert(stream_get_audiotrack_epoch_ts(&s, frames, 44100, 92) == 92);
    stream_audiotrack_epoch_reset(&s); // seek/flush/stop
    arm_epoch(&s, 0, 44100, 9000, 1.2f);
    assert(s.at_speed_epoch_heard_ts == 9000);
    // Large cumulative playhead positions retain precise small deltas.
    s.at_speed_epoch_presented_frames = UINT64_C(1) << 40;
    double now = stream_get_audiotrack_epoch_ts(&s,
        s.at_speed_epoch_presented_frames + 53, 44100, -1);
    assert(fabsl(now - (9000 + 53000.0L / 44100 / s.at_speed_epoch_speed)) < 0.000001L);
    pthread_t w,r;
    assert(!pthread_create(&w,NULL,writer,NULL));
    assert(!pthread_create(&r,NULL,reader,NULL));
    assert(!pthread_join(w,NULL)); assert(!pthread_join(r,NULL));
    // Transition readers never consume an unconfirmed speed or advance a cache.
    stream_audiotrack_epoch_reset(&s);
    unsigned g=stream_audiotrack_epoch_begin(&s,48000,48000,1000);
    int heard=-1; ph=96000; query_stream=&s;
    assert(stream_audiotrack_epoch_read(&s,&heard) && heard==1000);
    stream_audiotrack_epoch_finish(&s,g,1); // rejected 1.6x, hardware stayed 1x
    assert(stream_audiotrack_epoch_read(&s,&heard) && heard==2000);
    ph=72000; // older concurrent query must not move the cache backwards
    assert(stream_audiotrack_epoch_read(&s,&heard) && heard==2000);
    ph=144000; ph_rate=44100;
    assert(stream_audiotrack_epoch_read(&s,&heard) && heard==2000);
    ph_rate=48000; query_action=1;
    assert(!stream_audiotrack_epoch_read(&s,&heard));
    stream_audiotrack_epoch_finish(&s,g,2); // reset cannot be undone by setter completion
    assert(!stream_audiotrack_epoch_active(&s));
    arm_epoch(&s,0,48000,0,1); query_action=2;
    assert(stream_audiotrack_epoch_read(&s,&heard) && heard==1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            source, binary = Path(tmp) / 'epoch.c', Path(tmp) / 'epoch'
            source.write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', str(source), '-lm', '-pthread', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()

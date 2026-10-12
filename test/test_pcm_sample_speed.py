"""Exercise the production PCM sample clock across anchored speed changes."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_video_pcm_startup import block


class PcmSampleSpeedTest(unittest.TestCase):
    def test_media_origin_survives_speed_changes(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / 'Source/stream_audio.c').read_text()
        update = block(source, 'else if( !passthrough_active && !ac3_recoding )')
        update = update[update.index('{'):]
        mapping = block((root / 'Include/util.h').read_text(),
                        'static inline double timeline_rst_to_ts(')
        code = r'''
#include <assert.h>
#include <stdint.h>
typedef struct { double rst_anchor, ts_anchor, speed, inv_speed; } timeline_state_t;
''' + mapping + r'''
static timeline_state_t map = {0, 0, 1, 1};
#define RST_TO_TS_TIME(value,type) ((type)timeline_rst_to_ts(map,value))
typedef struct { double audio_ref_time_rst; uint64_t audio_samples; int time; } STREAM;
static void _set_audio_time(STREAM *s, int time) { s->time = time; }
static void update(STREAM *s, int sync_rate)
''' + update + r'''
int main(void) {
    STREAM s = {0, 60200 * 48, 0};
    update(&s,48000); assert(s.time == 60200);
    // Audible media is 60s, submitted media is 60.2s. Remap the queue too.
    map = (timeline_state_t){60000,60000,1.6,1/1.6};
    update(&s,48000); assert(s.time == 60125);
    s.audio_samples += 48000; update(&s,48000); assert(s.time == 60750);
    // Settle at a non-unit ratio, without losing the original media origin.
    map = (timeline_state_t){61000,60625,1.2,1/1.2};
    update(&s,48000); assert(s.time == 60791);
    map = (timeline_state_t){61200,60791.6666667,1,1};
    update(&s,48000); assert(s.time == 60791);
    // A seek/reset seeds a new media origin under the current mapping.
    s.audio_ref_time_rst=900000; s.audio_samples=44100;
    map=(timeline_state_t){900000,70000,1.25,0.8};
    update(&s,44100); assert(s.time == 70800);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            source, binary = Path(tmp) / 'clock.c', Path(tmp) / 'clock'
            source.write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall',
                            '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()

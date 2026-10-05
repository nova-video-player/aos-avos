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
        helper = block(sync, 'double stream_get_audiotrack_epoch_ts(')
        start = stream.index('double epoch_ts = stream_get_audiotrack_epoch_ts(')
        arm = stream[start:stream.index('\n\t\t\t\t\tDBG {', start)]
        fields = sorted(set(re.findall(r's->(\w+)', helper + arm)))
        declarations = '\n'.join(re.search(
            rf'^\s*\w+\s+{name}\s*;', header, re.MULTILINE)[0] for name in fields)
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <math.h>
typedef uint64_t UINT64;
typedef struct {
''' + declarations + r'''
} STREAM;
static int64_t atime64(void) { return 3000000001LL; }
''' + helper + r'''
static void arm_epoch(STREAM *s, UINT64 ep_frames, int ep_rate, int anchor_ts, float av_speed) {
''' + arm + r'''
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
    s.at_speed_epoch_active = 0; // seek/flush/stop
    arm_epoch(&s, 0, 44100, 9000, 1.2f);
    assert(s.at_speed_epoch_heard_ts == 9000);
    // Large cumulative playhead positions retain precise small deltas.
    s.at_speed_epoch_presented_frames = UINT64_C(1) << 40;
    double now = stream_get_audiotrack_epoch_ts(&s,
        s.at_speed_epoch_presented_frames + 53, 44100, -1);
    assert(fabsl(now - (9000 + 53000.0L / 44100 / s.at_speed_epoch_speed)) < 0.000001L);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            source, binary = Path(tmp) / 'epoch.c', Path(tmp) / 'epoch'
            source.write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', str(source), '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()

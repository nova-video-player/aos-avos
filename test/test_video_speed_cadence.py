"""Exercise the production renderer's speed-boundary arithmetic on the host."""
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class SpeedCadenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (Path(__file__).resolve().parents[1] / 'Source/codec_sfdec2.c').read_text()
        start = source.index('// Renderer-local speed-boundary compensation.')
        helper = source[start:source.index('typedef struct priv {', start)]
        cls.directory = tempfile.TemporaryDirectory()
        root = Path(cls.directory.name)
        code = '''
#include <stdint.h>
#include <math.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
typedef int64_t INT64;
typedef struct { double rst_anchor, ts_anchor, speed, inv_speed; } timeline_state_t;
''' + helper + '''
static speed_cadence_t state;
void fresh(void) { memset(&state, 0, sizeof(state)); }
void enable(void) { state.enabled = 1; }
void reset(void) { speed_cadence_reset(&state); }
int64_t correction(void) { return state.correction_ns; }
int64_t frame(int media, int epoch, int valid, double speed, double anchor,
              int64_t raw, int64_t interval, int commit) {
    timeline_state_t map = { anchor, 0, speed, 1.0 / speed };
    unsigned generation = state.generation;
    int64_t correction;
    int64_t deadline = speed_cadence_time(&state, map, epoch, media, valid,
                                         interval, raw, &correction);
    if (commit == 2) speed_cadence_reset(&state); // lifecycle raced release
    if (commit == 3) state.enabled = 1; // speed commit raced release
    if (commit) speed_cadence_commit(&state, generation, map, epoch, media,
                                    valid, deadline, correction);
    return deadline;
}
'''
        (root / 'cadence.c').write_text(code)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                        '-Werror', '-shared', '-fPIC', str(root / 'cadence.c'),
                        '-o', str(root / 'cadence.so'), '-lm'], check=True)
        cls.lib = ctypes.CDLL(str(root / 'cadence.so'))
        cls.lib.frame.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                 ctypes.c_double, ctypes.c_double, ctypes.c_int64,
                                 ctypes.c_int64, ctypes.c_int]
        cls.lib.frame.restype = cls.lib.correction.restype = ctypes.c_int64

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def setUp(self):
        self.lib.fresh()

    def frame(self, media, raw_ms, speed=1, anchor=0, epoch=1, valid=1, commit=1,
              interval_ms=None):
        return self.lib.frame(media, epoch, valid, speed, anchor, round(raw_ms * 1e6),
                              round((interval_ms or 40 / speed) * 1e6), commit) / 1e6

    def test_boundary_and_convergence_without_cumulative_phase(self):
        self.frame(0, 1000)
        self.lib.enable()
        # New map shortens the next deadline by 15ms beyond the speed change.
        self.assertEqual(self.frame(40, 1010, speed=1.6), 1024)
        self.assertEqual(self.lib.correction(), 14000000)
        # Every accepted frame spends at most 1ms; the raw audio schedule wins.
        previous = 1024
        for i in range(2, 30):
            raw = 985 + 25 * i
            deadline = self.frame(40 * i, raw, speed=1.6)
            self.assertGreaterEqual(deadline - previous, 24)
            self.assertLessEqual(deadline - previous, 25)
            previous = deadline
        self.assertEqual(self.lib.correction(), 0)
        self.assertEqual(previous, raw)
        # Return to 1x, including a new anchor, with an excessive positive gap.
        self.assertEqual(self.frame(1200, previous + 60, anchor=7), previous + 41)

    def test_lookahead_retries_and_dropped_frames_do_not_spend_correction(self):
        self.frame(0, 1000)
        self.lib.enable()
        for _ in range(30):
            self.assertEqual(self.frame(40, 1010, speed=1.6, commit=0), 1024)
        self.assertEqual(self.lib.correction(), 0)
        self.assertEqual(self.frame(40, 1010, speed=1.6), 1024)

    def test_passthrough_and_ordinary_playback_are_unchanged(self):
        self.frame(0, 1000)
        # The production passthrough path never enables this correction.
        self.assertEqual(self.frame(40, 1070, speed=1.6), 1070)
        self.lib.enable()
        self.lib.reset()
        self.assertEqual(self.frame(80, 1100), 1100)

    def test_lifecycle_reset_during_release_cannot_restore_old_state(self):
        self.frame(0, 1000)
        self.lib.enable()
        self.frame(40, 1010, speed=1.6, commit=2)
        self.lib.enable()
        self.assertEqual(self.frame(80, 1200), 1200)

    def test_speed_commit_during_release_keeps_last_submitted_frame(self):
        self.frame(0, 1000, commit=3)
        self.assertEqual(self.frame(40, 1010, speed=1.6), 1024)

    def test_source_gaps_missing_pts_and_seek_epochs(self):
        self.frame(0, 1000)
        self.lib.enable()
        # A two-frame source gap must remain two frames at the new speed.
        self.assertEqual(self.frame(80, 1030, speed=1.6), 1049)
        self.assertEqual(self.frame(120, 2000, epoch=2), 2000)
        self.assertEqual(self.lib.correction(), 0)
        self.assertEqual(self.frame(160, 2100, valid=0), 2100)
        self.assertEqual(self.frame(200, 2200), 2200)

    def test_high_frame_rates_use_ten_percent_limit(self):
        self.frame(0, 1000, interval_ms=5)
        self.lib.enable()
        self.assertEqual(self.frame(5, 1010, speed=2, interval_ms=2.5), 1002.75)


if __name__ == '__main__':
    unittest.main()

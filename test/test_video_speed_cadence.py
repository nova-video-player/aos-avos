"""Exercise the production renderer's speed-boundary arithmetic on the host."""
import ctypes
import math
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
void short_lead(void) { state.short_lookahead = 1; }
int64_t lookahead(double speed) { return speed_cadence_lookahead_ns(&state, speed); }
int64_t mode2_entry_limit(int frame_ms) { return mode2_clock_entry_limit_ns(frame_ms); }
int64_t calibrate(int us, unsigned generation, int delay) {
    return speed_cadence_calibration(&state, us, generation, delay);
}
void reset(void) { speed_cadence_reset(&state); }
int64_t correction(void) { return state.correction_ns; }
int64_t frame(int media, int epoch, int valid, double speed, double anchor,
              int64_t raw, int64_t interval, int commit) {
    timeline_state_t map = { anchor, 0, speed, 1.0 / speed };
    unsigned generation = state.generation;
    int64_t calibration_offset = state.calibration_offset_ns;
    int64_t correction;
    int64_t deadline = speed_cadence_time(&state, map, epoch, media, valid,
                                         interval, raw, &correction);
    if (commit == 2) speed_cadence_reset(&state); // lifecycle raced release
    if (commit == 3) state.enabled = 1; // speed commit raced release
    if (commit == 4) speed_cadence_calibration(&state, 145000, 7, 0);
    if (commit) speed_cadence_commit(&state, generation, map, epoch, media,
                                    valid, deadline, correction, calibration_offset);
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
        cls.lib.lookahead.argtypes = [ctypes.c_double]
        cls.lib.lookahead.restype = cls.lib.calibrate.restype = ctypes.c_int64
        cls.lib.mode2_entry_limit.argtypes = [ctypes.c_int]
        cls.lib.mode2_entry_limit.restype = ctypes.c_int64

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
        self.assertEqual(self.frame(40, 1010, speed=1.6), 1022.5)
        self.assertEqual(self.lib.correction(), 12500000)
        # At 1.6x the 25ms interval limits repayment to 2.5ms per frame.
        previous = 1022.5
        for i in range(2, 30):
            raw = 985 + 25 * i
            deadline = self.frame(40 * i, raw, speed=1.6)
            self.assertGreaterEqual(deadline - previous, 22.5)
            self.assertLessEqual(deadline - previous, 25)
            previous = deadline
        self.assertEqual(self.lib.correction(), 0)
        self.assertEqual(previous, raw)
        # Return to 1x, including a new anchor, with an excessive positive gap.
        self.assertEqual(self.frame(1200, previous + 60, anchor=7), previous + 44)

    def test_mode2_handoff_recovery_budget(self):
        limit = self.lib.mode2_entry_limit
        # Bravia's two observed corrections fit without a 175ms deadline jump.
        for correction in (176819000, 174017000):
            self.assertLessEqual(correction, limit(42))
            frames = math.ceil(correction / 5000000)
            self.assertLessEqual(frames * (42 + 5), 2000)
        for interval in (16, 33, 42, 83, 1000, 2147483647):
            self.assertGreaterEqual(limit(interval), 100000000)
            self.assertLessEqual(limit(interval), 350000000)
            if limit(interval) > 100000000:
                self.assertLessEqual(limit(interval) // 5000000 * (interval + 5), 2000)
        self.assertEqual(limit(0), 100000000)
        self.assertEqual(limit(-1), 100000000)
        self.assertGreater(400000000, limit(16))

    def test_lookahead_retries_and_dropped_frames_do_not_spend_correction(self):
        self.frame(0, 1000)
        self.lib.enable()
        for _ in range(30):
            self.assertEqual(self.frame(40, 1010, speed=1.6, commit=0), 1022.5)
        self.assertEqual(self.lib.correction(), 0)
        self.assertEqual(self.frame(40, 1010, speed=1.6), 1022.5)

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
        self.assertEqual(self.frame(40, 1010, speed=1.6), 1022.5)

    def test_source_gaps_missing_pts_and_seek_epochs(self):
        self.frame(0, 1000)
        self.lib.enable()
        # A two-frame source gap must remain two frames at the new speed.
        self.assertEqual(self.frame(80, 1030, speed=1.6), 1047.5)
        self.assertEqual(self.frame(120, 2000, epoch=2), 2000)
        self.assertEqual(self.lib.correction(), 0)
        self.assertEqual(self.frame(160, 2100, valid=0), 2100)
        self.assertEqual(self.frame(200, 2200), 2200)

    def test_high_frame_rates_use_ten_percent_limit(self):
        self.frame(0, 1000, interval_ms=5)
        self.lib.enable()
        self.assertEqual(self.frame(5, 1010, speed=2, interval_ms=2.5), 1002.75)

    def test_calibration_is_smoothed_at_any_final_speed(self):
        for speed in (1.0, 1.2, 1.8):
            with self.subTest(speed=speed):
                self.lib.fresh()
                self.assertEqual(self.lib.calibrate(113000, 7, 0), 0)
                self.frame(0, 1000, speed=speed)
                self.lib.enable()
                self.lib.short_lead()
                self.assertEqual(self.lib.calibrate(145000, 7, 0), 32000000)
                previous = 1000
                interval = 40 / speed
                for i in range(1, 20):
                    raw = 1032 + interval * i
                    # A repeated peek spends nothing until a frame is submitted.
                    first = self.frame(i * 40, raw, speed=speed, commit=0)
                    self.assertEqual(first, self.frame(i * 40, raw, speed=speed, commit=0))
                    deadline = self.frame(i * 40, raw, speed=speed)
                    self.assertGreaterEqual(deadline - previous, interval - .00001)
                    self.assertLessEqual(deadline - previous, interval * 1.1 + .00001)
                    previous = deadline
                self.assertEqual(self.lib.correction(), 0)
                self.assertAlmostEqual(deadline, raw, places=5)
                self.assertEqual(self.lib.calibrate(145000, 7, 0), 0)

    def test_calibration_during_release_is_not_lost(self):
        self.lib.calibrate(113000, 7, 0)
        self.frame(0, 1000)
        self.lib.enable()
        self.lib.short_lead()
        self.assertEqual(self.frame(40, 1040, commit=4), 1040)
        self.assertEqual(self.frame(80, 1112), 1084)
        self.assertEqual(self.lib.correction(), -28000000)

    def test_calibration_does_not_follow_a_different_output_or_manual_delay(self):
        self.frame(0, 1000)
        self.lib.enable()
        self.lib.short_lead()
        self.assertEqual(self.lib.calibrate(113000, 7, 0), 0)
        self.assertEqual(self.lib.calibrate(145000, 8, 0), 0)
        self.assertEqual(self.lib.calibrate(170000, 8, -100), 0)
        self.assertEqual(self.lib.calibrate(-1, 8, -100), 0)
        self.assertEqual(self.lib.calibrate(200000, 8, -100), 0)
        self.lib.reset()
        self.lib.enable()
        self.assertEqual(self.lib.calibrate(250000, 8, -100), 0)

    def test_speed_lookahead_stays_short_at_intermediate_ratios_and_resets(self):
        self.assertEqual(self.lib.lookahead(1), 200000000)
        self.assertEqual(self.lib.lookahead(1.8), 50000000)
        self.lib.short_lead()
        self.assertEqual(self.lib.lookahead(1.2), 50000000)
        self.assertEqual(self.lib.lookahead(1), 50000000)
        self.lib.reset()
        self.assertEqual(self.lib.lookahead(1), 200000000)

    def test_queued_ramp_to_1_2x_settles_without_visiting_1x(self):
        peaks = []
        for legacy_lead in (True, False):
            self.lib.fresh()
            self.lib.enable()
            self.lib.short_lead()
            speed, media_anchor, wall_anchor = 1.8, 0, 0
            index = step = 0
            peak = last_correction = 0
            # Simulate renderer peeks and successful submissions, with twelve
            # playhead-gated commits 120ms apart. Include 8ms media-map anchor
            # adjustments, like those seen in the recorded software-filter ramp.
            # Hold 1.2x for over eight seconds; 1x calibration is never invoked.
            for wall in range(800, 11000):
                if 1500 <= wall <= 2820 and (wall - 1500) % 120 == 0:
                    new_wall = wall - 1000
                    media_anchor += (new_wall - wall_anchor) * speed - 8
                    wall_anchor = new_wall
                    step += 1
                    speed = round(1.8 - step * .05, 2)
                media = round(index * 1001 / 24)
                raw = 1000 + wall_anchor + (media - media_anchor) / speed
                lead = 200 if legacy_lead else self.lib.lookahead(speed) / 1e6
                preview = self.frame(media, raw, speed=speed, anchor=step,
                                     interval_ms=1001 / 24 / speed, commit=0)
                if preview <= wall + lead:
                    deadline = self.frame(media, raw, speed=speed, anchor=step,
                                          interval_ms=1001 / 24 / speed)
                    peak = max(peak, abs(raw - deadline))
                    if abs(raw - deadline) > 1:
                        last_correction = wall
                    index += 1
            self.assertEqual(speed, 1.2)
            self.assertEqual(self.lib.correction(), 0)
            self.assertLess(last_correction, 3200)
            peaks.append(peak)
        self.assertLess(peaks[1], peaks[0] / 2)

    def test_rapid_ramps_repay_phase_without_deadline_jumps(self):
        # 24fps, twelve commits roughly three frames apart. Queued-video
        # remapping shifts the raw schedule another 15ms at each boundary,
        # before the preceding correction is gone. Exercise both directions
        # and repeated ramps, not only an isolated speed change.
        for direction in (-1, 1):
            with self.subTest(direction=direction):
                self.lib.fresh()
                initial_speed = 1.6 if direction < 0 else 1.0
                raw = deadline = 1000.0
                media = index = 0
                self.frame(media, raw, speed=initial_speed, interval_ms=41.708 / initial_speed)
                self.lib.enable()
                for cycle in range(3):
                    for step in range(1, 13):
                        speed = round(initial_speed + direction * step * .05, 2)
                        nominal = 41.708 / speed
                        for frame in range(3):
                            index += 1
                            next_media = round(index * 41.708)
                            interval = (next_media - media) / speed
                            raw += interval - (direction * 15 if frame == 0 else 0)
                            previous = deadline
                            deadline = self.frame(next_media, raw, speed=speed,
                                                  anchor=cycle * 12 + step,
                                                  interval_ms=nominal)
                            self.assertLessEqual(abs(deadline - previous - interval),
                                                 min(4, nominal / 10) + .00001)
                            self.assertLess(abs(self.lib.correction()), 70000000)
                            media = next_media
                    # Returning to the raw clock must take less than one second
                    # even at 24fps, with no accumulated debt on the next ramp.
                    for _ in range(math.floor(1000 / nominal)):
                        index += 1
                        next_media = round(index * 41.708)
                        interval = (next_media - media) / speed
                        raw += interval
                        previous = deadline
                        deadline = self.frame(next_media, raw, speed=speed,
                                              anchor=cycle * 12 + 12,
                                              interval_ms=nominal)
                        self.assertLessEqual(abs(deadline - previous - interval),
                                             min(4, nominal / 10) + .00001)
                        media = next_media
                    self.assertEqual(self.lib.correction(), 0)
                    initial_speed = speed
                    direction = -direction


if __name__ == '__main__':
    unittest.main()

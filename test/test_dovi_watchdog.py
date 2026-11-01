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

"""Replay Dolby Vision no-first-frame recovery timing."""
import unittest

from native_replay import run_c


class DolbyVisionWatchdogTest(unittest.TestCase):
    def test_watchdog_ignores_pause_starvation_and_steady_state(self):
        run_c(r'''
#include <assert.h>
#include "video_startup_watchdog.h"
int main(void) {
    VIDEO_STARTUP_WATCHDOG w;
    video_startup_watchdog_reset(&w, 1);
    /* An empty decoder cannot fail just because time passes. */
    for (int t = 100; t <= 30000; t += 100)
        assert(!video_startup_watchdog_step(&w, t, 0, 1, 0, 1));
    assert(!video_startup_watchdog_step(&w, 30100, 1, 0, 0, 1));
    /* A full input queue after one accepted packet can expose a stall. */
    for (int t = 30200; t < 40200; t += 100)
        assert(!video_startup_watchdog_step(&w, t, 0, 1, 0, 1));
    assert(video_startup_watchdog_step(&w, 40200, 0, 1, 0, 1));
    video_startup_watchdog_suspend(&w);
    for (int t = 50000; t < 70000; t += 100)
        assert(!video_startup_watchdog_step(&w, t, 0, 1, 0, 0));
    assert(!w.active_ms);
    /* Network/input gaps reset the timer; elapsed wall time isn't enough. */
    for (int t = 70000; t < 170000; t += 2000)
        assert(!video_startup_watchdog_step(&w, t, 1, 0, 0, 1));
    /* First-frame output disables startup timing until the next flush. */
    w.output_seen = 1;
    for (int t = 170000; t < 200000; t += 100)
        assert(!video_startup_watchdog_step(&w, t, 0, 1, 1, 1));
    video_startup_watchdog_reset(&w, w.enabled);
    assert(!w.output_seen && !w.inputs && !w.active_ms);
    assert(!video_startup_watchdog_step(&w, 200000, 1, 0, 0, 1));
    /* Short streams must also recover if EOS draining produces no frame. */
    for (int t = 200100; t < 210100; t += 100)
        assert(!video_startup_watchdog_step(&w, t, 0, 0, 1, 1));
    assert(video_startup_watchdog_step(&w, 210100, 0, 0, 1, 1));
    video_startup_watchdog_reset(&w, 0);
    for (int t = 210200; t < 240000; t += 100)
        assert(!video_startup_watchdog_step(&w, t, 1, 1, 1, 1));
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()

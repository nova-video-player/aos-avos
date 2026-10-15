#!/usr/bin/env python3
"""Compatibility runner for the current pause/resume and renderer replays.

The old monolithic replay depended on removed helpers and obsolete preload and
commit-timeout contracts. Its coverage now lives in unittest-discoverable modules.
These host checks do not reproduce Android/JNI timing or measure physical lipsync.
"""
import unittest


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromNames([
        'test_pause_resume_policy', 'test_slew_offset', 'test_audiotrack_speed_epoch',
    ])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)

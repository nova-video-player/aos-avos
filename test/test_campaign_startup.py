"""Launch ordering, preference preservation and startup evidence boundaries."""
import contextlib
import io
import itertools
import json
import os
from pathlib import Path
import shlex
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import campaign_startup as startup
import run_stress_campaign as campaign


def evidence(mode=2):
    return '\n'.join(f'video_sched_diag: frame={i}\nvideo_render_diag: frame={i}\n'
                     f'audiotrack_write: wrote 2560 out of 2560 bytes (passthrough={mode})'
                     for i in (100, 140, 180)) + '\n'


class CampaignStartupTests(unittest.TestCase):
    def test_readiness_accepts_pcm_and_passthrough_but_requires_evidence(self):
        for mode in (0, 1, 2):
            self.assertTrue(startup.evidence_ready(evidence(mode), {}))
        self.assertFalse(startup.evidence_ready('video_sched_diag: frame=100\n' * 10, {}))
        self.assertFalse(startup.evidence_ready(evidence().replace('video_render_diag:', 'missing:'), {}))

    def test_remote_uri_is_one_argument_and_serial_is_propagated(self):
        uri = "https://example.invalid/a b.mkv?x=1&literal=$(date)'"
        env = {'ANDROID_SERIAL': 'device-one'}
        with patch.object(startup.subprocess, 'run', return_value=SimpleNamespace(
                returncode=0, stdout='', stderr='')) as run:
            startup.shell(env, 'am', 'start', '-d', uri)
        self.assertEqual(shlex.split(run.call_args.args[0][2]), ['am', 'start', '-d', uri])
        self.assertEqual(run.call_args.kwargs['env'], env)

    def test_capture_precedes_debug_and_launch_and_cleanup_runs_on_failure(self):
        for failure in (None, RuntimeError('launch denied'), KeyboardInterrupt()):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as tmp:
                output = Path(tmp)
                events = []
                collector = Mock()
                collector.poll.return_value = None
                log = output / 'startup/logcat.log'

                def append(text):
                    with log.open('a') as handle:
                        handle.write(text)

                def popen(*args, **kwargs):
                    events.append('capture')
                    return collector

                def shell(env, *args, **kwargs):
                    events.append(args)
                    if args[0] == 'log':
                        append(args[-1] + '\n')
                    if args[-1] == 'at_pcm_observe 600':
                        append('pcm_present_observer: enabled seconds=600\n')
                    if args[:2] == ('am', 'start'):
                        if failure:
                            raise failure
                        append(evidence())
                    return 'ok'

                with patch.object(startup, 'shell', side_effect=shell), \
                        patch.object(startup.subprocess, 'Popen', side_effect=popen), \
                        patch.object(startup.subprocess, 'run', return_value=SimpleNamespace(
                            returncode=0, stdout='', stderr='')), \
                        patch.object(startup.time, 'monotonic', side_effect=itertools.count()), \
                        patch.object(startup.time, 'sleep', side_effect=lambda _: append(evidence())), \
                        patch.object(startup, 'stop') as stop, contextlib.redirect_stdout(io.StringIO()):
                    args = (output, 'content://videos/1', 'org.courville.nova',
                            {'PCM_PRESENTATION_CAPTURE': '1'})
                    if failure:
                        with self.assertRaises(type(failure)):
                            startup.capture_startup(*args, hold=2)
                    else:
                        self.assertEqual(startup.capture_startup(*args, hold=2)['status'], 'READY')
                self.assertEqual(events[0], 'capture')
                launch = next(i for i, item in enumerate(events) if item[:2] == ('am', 'start'))
                for cmd in ('dbgs 2', 'dbgsink 2', 'dbga 2', 'at_pcm_observe 600'):
                    self.assertTrue(any(item[-1] == cmd for item in events[1:launch]))
                self.assertEqual(events[-1][-1], 'at_pcm_observe 0')
                self.assertFalse(any('SpeedTestReceiver' in str(item) or 'configure' in item
                                     for item in events))
                stop.assert_any_call(collector)
                report = json.loads((output / 'startup/startup-report.json').read_text())
                self.assertEqual(report['status'], 'ERROR' if failure else 'READY')
                self.assertEqual(report['startup_health'], 'not_assessed')

    def test_wrapper_preserves_existing_mode_and_routes_startup_errors(self):
        for video, fail in ((False, False), (True, False), (True, True)):
            with self.subTest(video=video, fail=fail), tempfile.TemporaryDirectory() as tmp:
                output = Path(tmp) / 'session'
                argv = ['run_stress_campaign.py', '--serial', 'test-device', '--output-dir', str(output)]
                if video:
                    argv += ['--video', '/sdcard/clip.mkv']
                with patch('sys.argv', argv), patch.dict(os.environ, {'DRY_RUN': '0', 'SESSION_LOGCAT': '1'}), \
                        patch.object(campaign, 'capture_startup', side_effect=RuntimeError('launch failed') if fail else None) as launch, \
                        patch.object(campaign.subprocess, 'run', return_value=SimpleNamespace(returncode=0)) as run, \
                        patch.object(campaign, 'generate_reports', return_value=0) as reports, \
                        contextlib.redirect_stdout(io.StringIO()):
                    campaign.main()
                self.assertEqual(launch.call_count, int(video))
                self.assertEqual(run.call_count, int(not fail))
                if not fail:
                    self.assertEqual(run.call_args.kwargs['env']['ANDROID_SERIAL'], 'test-device')
                if fail:
                    self.assertIn('launch failed', reports.call_args.kwargs['unavailable_reason'])
                    self.assertEqual(reports.call_args.args[1], 2)

    def test_dry_run_does_not_launch_or_create_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'session'
            with patch('sys.argv', ['run_stress_campaign.py', '--video', '/sdcard/clip.mkv',
                                   '--dry-run', '--output-dir', str(output)]), \
                    patch.object(campaign, 'capture_startup') as launch, \
                    patch.object(campaign.subprocess, 'run', return_value=SimpleNamespace(returncode=0)), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(campaign.main(), 0)
            launch.assert_not_called()
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()

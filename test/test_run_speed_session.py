#!/usr/bin/env python3
"""Host checks for speed-session setup, backend evidence and cleanup."""
import contextlib
import io
import json
from pathlib import Path
import shlex
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import run_speed_session as speed
from run_stress_campaign import generate_reports
from test_analyze_stress import fixture
from analyze_stress import config


class SpeedSessionTests(unittest.TestCase):
    def test_remote_shell_preserves_uri_metacharacters(self):
        uri = "https://example.invalid/a b.mkv?x=1&literal=$(date)'"
        with patch.object(speed.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout='', stderr='')) as run:
            speed.shell('am', 'start', '-d', uri)
        args = run.call_args.args[0]
        self.assertEqual(args[:2], ['adb', 'shell'])
        self.assertEqual(shlex.split(args[2]), ['am', 'start', '-d', uri])

    def test_receiver_acknowledgement_is_required(self):
        with patch.object(speed, 'shell', return_value='Broadcast completed: result=0'):
            with self.assertRaises(RuntimeError):
                speed.configure('org.courville.nova', 'configure', 'test', 'atempo')
        with patch.object(speed, 'shell', return_value='Broadcast completed: result=-1, data="speed-test:configured:test:atempo"'):
            speed.configure('org.courville.nova', 'configure', 'test', 'atempo')
            with self.assertRaises(RuntimeError):
                speed.configure('org.courville.nova', 'configure', 'test', 'sonic')

    def test_ready_checks_native_backend_filter_pcm_and_initial_speed(self):
        base = fixture().replace('out of 7680 bytes', 'out of 7680 bytes (passthrough=0)')
        for backend, value in speed.BACKENDS.items():
            opened = ('stream_open_audio_filter: atempo/sonic filter disabled (using AudioTrack PlaybackParams)'
                      if backend == 'audiotrack' else f'stream_open_audio_filter: opened [{backend}]')
            text = f'stream_set_audio_speed_backend: {value}\n{opened}\n' + base
            self.assertTrue(speed.ready(text, backend))
            self.assertFalse(speed.ready(text.replace(f'stream_set_audio_speed_backend: {value}',
                                                     'stream_set_audio_speed_backend: 99'), backend))
            self.assertFalse(speed.ready(text.replace(opened, ''), backend))
            self.assertFalse(speed.ready(text.replace('speed=1.000', 'speed=1.600'), backend))
            with self.assertRaises(RuntimeError):
                speed.ready(text.replace('passthrough=0', 'passthrough=1'), backend)

    def test_dry_run_and_existing_evidence_never_contact_adb(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'session'
            args = ['run_speed_session.py', '--video', '/sdcard/clip.mkv', '--backend', 'sonic',
                    '--output-dir', str(output)]
            with patch('sys.argv', args + ['--dry-run']), patch.object(speed, 'shell') as shell, \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(speed.main(), 0)
                shell.assert_not_called()
                self.assertFalse(output.exists())
            output.mkdir()
            (output / 'evidence').write_text('keep')
            with patch('sys.argv', args), patch.object(speed, 'shell') as shell, \
                    contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    speed.main()
                shell.assert_not_called()
                self.assertEqual((output / 'evidence').read_text(), 'keep')

    def test_session_restores_settings_on_success_failure_and_interrupt(self):
        for outcome in (0, 1, KeyboardInterrupt()):
            with self.subTest(outcome=outcome), tempfile.TemporaryDirectory() as tmp:
                output = Path(tmp) / 'session'
                collector = Mock()
                collector.poll.return_value = None
                driver = Mock()
                driver.poll.return_value = 0
                if isinstance(outcome, BaseException):
                    driver.wait.side_effect = outcome
                else:
                    driver.wait.return_value = outcome
                calls = []

                def receiver(package, operation, session, backend=None):
                    calls.append((operation, session, backend))
                    return 'acknowledged\n'

                argv = ['run_speed_session.py', '--video', '/sdcard/clip.mkv', '--backend', 'sonic',
                        '--cycles', '2', '--hold', '7', '--output-dir', str(output)]
                with patch('sys.argv', argv), patch.object(speed, 'shell', return_value='Status: ok'), \
                        patch.object(speed, 'configure', side_effect=receiver), \
                        patch.object(speed, 'ready', return_value=True), \
                        patch.object(speed.subprocess, 'Popen', side_effect=[collector, driver]) as popen, \
                        patch.object(speed.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout='', stderr='')), \
                        patch.object(speed, 'stop_process') as stop, \
                        patch.object(speed, 'generate_reports', return_value=0) as reports, \
                        contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    speed.main()
                expected = 130 if isinstance(outcome, BaseException) else outcome
                reports.assert_called_once_with(output.resolve(), expected, 'speed-1/analyzer-config.json')
                self.assertEqual([c[0] for c in calls], ['configure', 'restore'])
                self.assertEqual(calls[0][1], calls[1][1])
                command = popen.call_args_list[1]
                self.assertEqual(command.args[0][-4:], ['2', '7', '0.02', '7'])
                self.assertEqual(command.kwargs['env']['SPEED_STEPS'], '12')
                self.assertEqual(command.kwargs['env']['REQUIRE_FILTER'], 'sonic')
                self.assertEqual(stop.call_count, 2)

    def test_report_uses_original_speed_configuration(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            output = Path(tmp)
            (output / 'speed-1').mkdir()
            (output / 'speed-1/analyzer-config.json').write_text(json.dumps(config({})))
            (output / 'logcat-session.log').write_text(fixture())
            self.assertEqual(generate_reports(output, 0, 'speed-1/analyzer-config.json'), 0)
            self.assertEqual(json.loads((output / 'run-report.json').read_text())['verdict'], 'PASS')


if __name__ == '__main__':
    unittest.main()

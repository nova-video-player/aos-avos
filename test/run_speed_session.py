#!/usr/bin/env python3
"""Configure Nova, open a video and run a separate PCM speed-ramp session."""
import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time
from urllib.parse import urlsplit
import uuid
import xml.etree.ElementTree as ET

from run_stress_campaign import generate_reports


RECEIVER = 'com.archos.mediacenter.video.debug.SpeedTestReceiver'
PLAYER = 'com.archos.mediacenter.video.player.PlayerActivity'
BACKENDS = {'atempo': 0, 'audiotrack': 1, 'sonic': 2}


class SetupRejected(RuntimeError):
    """Receiver rejected setup before saving this session's preferences."""


def pending_restore_hint(package):
    # Debug APKs permit run-as. Read only the test backup, never app-wide prefs.
    try:
        backup = ET.fromstring(shell('run-as', package, 'cat',
                                     'shared_prefs/speed_test_preferences.xml'))
        session = backup.findtext("string[@name='session']")
        if session and re.fullmatch(r'[A-Za-z0-9_-]{1,80}', session):
            command = ['python3', 'test/run_speed_session.py', '--package', package]
            if os.environ.get('ANDROID_SERIAL'):
                command += ['--serial', os.environ['ANDROID_SERIAL']]
            command += ['--restore', session]
            return 'Restore the previous session first: ' + shlex.join(command)
    except (OSError, RuntimeError, subprocess.SubprocessError, ET.ParseError):
        pass
    return ('Restore the previous session using its manifest.json session ID and '
            '--restore, with the same package/device. The new recovery ID does not own that backup.')


def video_uri(value):
    if value.startswith('/'):
        return Path(value).as_uri()  # Absolute path on the Android device.
    if not urlsplit(value).scheme:
        raise argparse.ArgumentTypeError('use a video URI or an absolute path on the device')
    return value


def positive(value):
    value = int(value)
    if value <= 0:
        raise argparse.ArgumentTypeError('must be greater than zero')
    return value


def shell(*args, timeout=40):
    # adb joins its shell arguments; quote once for the remote shell, not the host.
    result = subprocess.run(['adb', 'shell', shlex.join(map(str, args))],
                            capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError('adb shell failed: ' + result.stderr.strip())
    return result.stdout


def configure(package, operation, session, backend=None):
    args = ['am', 'broadcast', '--include-stopped-packages', '-n', f'{package}/{RECEIVER}',
            '--es', 'operation', operation, '--es', 'session', session]
    if backend:
        args += ['--es', 'backend', backend]
    output = shell(*args)
    expected = f'speed-test:{"configured" if operation == "configure" else "restored"}:{session}'
    if backend:
        expected += ':' + backend
    if 'result=-1' not in output or f'data="{expected}"' not in output:
        rejected = re.search(r'data="speed-test:error:([^"\r\n]+)"', output)
        if rejected:
            reason = rejected.group(1)
            message = f'Nova rejected speed-test {operation}: {reason}.'
            if reason == 'invalid-operation-or-pending-restore':
                message += ' ' + pending_restore_hint(package)
            # configure-failed may have persisted a backup before failing to
            # apply preferences. Unknown errors and lost replies also need cleanup.
            if operation == 'configure' and reason in {
                    'invalid-session', 'stop-playback-first', 'invalid-backend',
                    'invalid-operation-or-pending-restore'}:
                raise SetupRejected(message)
            raise RuntimeError(message)
        raise RuntimeError('Nova did not acknowledge speed-test ' + operation +
                           '; check the ADB connection and that the debug APK includes '
                           'SpeedTestReceiver. Response: ' + output.strip())
    return output


def ready(text, backend):
    choices = re.findall(r'stream_set_audio_speed_backend: (\d+)', text)
    speeds = re.findall(r'put_time_calc:.*? speed=([0-9.]+)', text)
    pcm = re.findall(r'audiotrack_write: wrote ([1-9]\d*) out of .*?passthrough=(\d+)', text)
    if pcm and pcm[-1][1] != '0':
        raise RuntimeError('compressed passthrough is active; PCM setup did not take effect')
    opened = ('using AudioTrack PlaybackParams)' if backend == 'audiotrack' else
              f'stream_open_audio_filter: opened [{backend}]')
    return (bool(choices) and int(choices[-1]) == BACKENDS[backend] and opened in text and
            bool(speeds) and abs(float(speeds[-1]) - 1) < .01 and bool(pcm) and
            text.count('video_render_diag:') >= 3)


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        process.wait()
        return
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--video', type=video_uri, help='URI or absolute path on the Android device')
    parser.add_argument('--backend', choices=BACKENDS)
    parser.add_argument('--cycles', type=positive, default=5)
    parser.add_argument('--hold', type=positive, default=8, help='seconds at BOTH 1.60x and 1.00x')
    parser.add_argument('--startup-timeout', type=positive, default=40)
    parser.add_argument('--serial', help='adb device serial; otherwise use ANDROID_SERIAL/default device')
    parser.add_argument('--package', default=os.environ.get('PACKAGE', 'org.courville.nova'))
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--restore', metavar='SESSION_ID', help='restore settings after an interrupted host run')
    parser.add_argument('--dry-run', action='store_true', help='show the plan without contacting the device')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_.]+', args.package):
        parser.error('invalid package')
    if args.restore and not re.fullmatch(r'[A-Za-z0-9_-]{1,80}', args.restore):
        parser.error('invalid restore session ID')
    if not args.restore and (not args.video or not args.backend):
        parser.error('--video and --backend are required')
    if args.restore and (args.video or args.backend):
        parser.error('--restore cannot be combined with --video or --backend')
    if args.serial:
        os.environ['ANDROID_SERIAL'] = args.serial
    if args.dry_run:
        if args.restore:
            print(f'Stop Nova and restore session {args.restore}')
        else:
            print(f'Configure {args.backend}, PCM/FFmpeg, initial 1.00x; enable dbgs/dbgsink/dbga=2')
            print(f'Open {args.video}')
            print(f'{args.cycles} cycles: 12 speed-up keys, hold {args.hold}s, '
                  f'12 speed-down keys, hold {args.hold}s (1.00x -> 1.60x -> 1.00x)')
            print('Save session logs/reports, stop test playback and restore saved preferences')
        return 0
    if args.restore:
        shell('am', 'force-stop', args.package)
        configure(args.package, 'restore', args.restore)
        print('Saved speed-test preferences restored.')
        return 0

    script_dir = Path(__file__).resolve().parent
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    output = (args.output_dir or script_dir.parent / f'speed-session-{stamp}').resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        parser.error('output directory must be absent or empty; previous evidence is preserved')
    output.mkdir(parents=True, exist_ok=True)
    session = uuid.uuid4().hex
    manifest = dict(session=session, backend=args.backend, cycles=args.cycles, hold_seconds=args.hold,
                    package=args.package, initial_speed=1.0, target_speed=1.6,
                    video=args.video, serial=os.environ.get('ANDROID_SERIAL'))
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Speed session: {output}', flush=True)
    print(f'Recovery ID if the host is killed: {session}', flush=True)
    collector = driver = None
    restore_needed = False
    result = 2
    error = None
    cleanup_error = None
    with (output / 'logcat-session.log').open('w') as capture, \
            (output / 'logcat-session-stderr.txt').open('w') as capture_errors, \
            (output / 'session.log').open('w') as session_log:
        try:
            shell('am', 'force-stop', args.package)
            # Preserve buffer configuration failures as evidence instead of hiding them.
            resized = subprocess.run(['adb', 'logcat', '-G', '16M'], capture_output=True, text=True)
            (output / 'logcat-buffer.txt').write_text(
                f'resize_rc={resized.returncode}\n{resized.stdout}{resized.stderr}' +
                subprocess.run(['adb', 'logcat', '-g'], capture_output=True, text=True).stdout)
            collector = subprocess.Popen(['adb', 'logcat', '-T', '1', '-v', 'epoch'],
                                         stdout=capture, stderr=capture_errors, start_new_session=True)
            restore_needed = True  # Includes a lost acknowledgement after settings were saved.
            session_log.write(configure(args.package, 'configure', session, args.backend))
            session_log.flush()
            launch = shell('am', 'start', '-W', '-n', f'{args.package}/{PLAYER}',
                           '-a', 'android.intent.action.VIEW', '-d', args.video,
                           '-t', 'video/*', '--ei', 'resume', '0', '--ei', 'position', '0')
            session_log.write(launch)
            session_log.flush()
            if re.search(r'Error:|Exception', launch):
                raise RuntimeError('video launch failed; see session.log')
            deadline = time.monotonic() + args.startup_timeout
            while True:
                if collector.poll() is not None:
                    raise RuntimeError('session logcat collector exited')
                if ready((output / 'logcat-session.log').read_text(errors='replace'), args.backend):
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError('startup did not confirm the selected backend, PCM, 1x and render diagnostics')
                time.sleep(.5)
            print(f'Confirmed {args.backend} at 1x; running {args.cycles} speed cycles.', flush=True)
            env = dict(os.environ, OUTPUT_DIR=str(output / 'speed-1'), PACKAGE=args.package,
                       LABEL=args.backend, REQUIRE_FILTER=args.backend, SPEED_STEPS='12',
                       HI_SPEED_MIN='1.59', HOLD_MIN_MS=str(args.hold * 600),
                       REQUIRE_RENDER_TIMING='1', REQUIRE_AUDIO_PRESENTATION='1')
            driver = subprocess.Popen(['bash', str(script_dir / 'stress_speed_validate.sh'),
                                       str(args.cycles), str(args.hold), '0.02', str(args.hold)],
                                      env=env, stdout=session_log, stderr=subprocess.STDOUT,
                                      start_new_session=True)
            started = last_update = time.monotonic()
            while driver.poll() is None:
                if collector.poll() is not None:
                    raise RuntimeError('session logcat collector exited during the test')
                if time.monotonic() - last_update >= 15:
                    print(f'Speed test running: {int(time.monotonic() - started)}s elapsed; '
                          f'progress in {output / "session.log"}', flush=True)
                    last_update = time.monotonic()
                time.sleep(.5)
            result = driver.wait()
            if result < 0:
                result = 128 - result
            if collector.poll() is not None:
                raise RuntimeError('session logcat collector exited during the test')
        except KeyboardInterrupt:
            result, error = 130, 'Interrupted'
        except SetupRejected as exc:
            restore_needed = False  # Never restore another session using our new ID.
            result, error = 2, str(exc)
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            result, error = 2, str(exc)
        finally:
            stop_process(driver)
            stop_process(collector)
            if restore_needed:
                try:
                    shell('am', 'force-stop', args.package)
                    session_log.write(configure(args.package, 'restore', session))
                except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
                    cleanup_error = str(exc)
                    if result < 128:
                        result = 2
    (output / 'summary.md').write_text(
        f'# PCM speed session\n\nBackend: {args.backend}\n\nCycles requested: {args.cycles}\n\n'
        f'Ramp: 1.00x to 1.60x and back; hold {args.hold}s at each end.\n\n'
        f'Driver/session exit code: {result}\n\n'
        f'Setup error: {error or "none"}\n\nRestore error: {cleanup_error or "none"}\n\n'
        'Per-cycle evidence: speed-1/results.txt. Full command output: session.log.\n')
    if error:
        print(error, file=sys.stderr)
    if cleanup_error:
        print('Restore failed: ' + cleanup_error, file=sys.stderr)
        print(f'Retry with --restore {session} using the same package/device.', file=sys.stderr)
    if driver is None:
        return generate_reports(output, result, 'speed-1/analyzer-config.json',
                                unavailable_reason='Speed cycles did not start: ' + (error or 'setup incomplete'))
    return generate_reports(output, result, 'speed-1/analyzer-config.json')


if __name__ == '__main__':
    def interrupt(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupt)
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(2)

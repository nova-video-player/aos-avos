"""Capture an optional campaign launch without changing playback preferences."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import time
from urllib.parse import urlsplit
import uuid

PLAYER = 'com.archos.mediacenter.video.player.PlayerActivity'
RECEIVER = 'com.archos.mediacenter.LibAvosReceiver'


def video_uri(value):
    if value.startswith('/'):
        return Path(value).as_uri()
    if not urlsplit(value).scheme:
        raise argparse.ArgumentTypeError('use a video URI or an absolute path on the device')
    return value


def positive(value):
    value = int(value)
    if value <= 0:
        raise argparse.ArgumentTypeError('must be greater than zero')
    return value


def shell(env, *args, timeout=40):
    result = subprocess.run(['adb', 'shell', shlex.join(map(str, args))],
                            env=env, capture_output=True, text=True, timeout=timeout)
    if result.returncode or re.search(r'Error:|Exception|Permission Denial', result.stdout):
        raise RuntimeError('adb command failed: ' + result.stdout + result.stderr)
    return result.stdout


def evidence_ready(text, env):
    frames = set(re.findall(r'video_sched_diag: frame=(-?\d+)', text))
    render = text.count('video_render_diag:')
    written = bool(re.search(r'audiotrack_write: wrote [1-9]\d* out of', text))
    return (len(frames) >= 3 and
            (env.get('REQUIRE_RENDER_TIMING', '1') == '0' or render >= 3) and
            (env.get('REQUIRE_AUDIO_PRESENTATION', '1') == '0' or written))


def stop(process):
    if process is None or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=10)
    except ProcessLookupError:
        process.wait()
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()


def capture_startup(output, video, package, env, timeout=60, hold=5):
    """READY means launch evidence exists, not a startup/lipsync PASS verdict."""
    directory = output / 'startup'
    directory.mkdir()
    log = directory / 'logcat.log'
    marker = 'AVOS_TEST_STARTUP_' + uuid.uuid4().hex
    report = dict(status='ERROR', video=video, package=package,
                  serial=env.get('ANDROID_SERIAL'), startup_timeout_sec=timeout,
                  startup_hold_sec=hold, physical_lipsync='unmeasured',
                  startup_health='not_assessed', source_log=str(log))
    collector = compositor = None
    pcm_armed = False
    with log.open('w') as capture, (directory / 'commands.log').open('w') as commands, \
            (directory / 'logcat-stderr.txt').open('w') as errors, \
            (directory / 'surfaceflinger-collector.log').open('w') as sf_log:
        def command(*args, **kwargs):
            response = shell(env, *args, **kwargs)
            commands.write(response + '\n')
            commands.flush()
            return response

        def debug(cmd):
            return command('am', 'broadcast', '--include-stopped-packages', '-n',
                           f'{package}/{RECEIVER}', '-a', 'com.archos.mediacenter.AVSH',
                           '--es', 'cmd', cmd)

        def text():
            if collector.poll() is not None:
                raise RuntimeError('startup logcat collector exited; see startup/logcat-stderr.txt')
            return log.read_text(errors='replace')

        try:
            subprocess.run(['adb', 'get-state'], env=env, capture_output=True, check=True, timeout=10)
            resized = subprocess.run(['adb', 'logcat', '-G', '16M'], env=env,
                                     capture_output=True, text=True, timeout=10)
            (directory / 'logcat-buffer.txt').write_text(
                f'resize_rc={resized.returncode}\n{resized.stdout}{resized.stderr}')
            collector = subprocess.Popen(['adb', 'logcat', '-T', '1', '-v', 'epoch'],
                                         env=env, stdout=capture, stderr=errors, start_new_session=True)
            command('am', 'force-stop', package)
            command('log', '-p', 'i', '-t', 'avos_test', marker)
            # Barrier proves collection started before any launch/setup action.
            deadline = time.monotonic() + 10
            while marker not in text():
                if time.monotonic() >= deadline:
                    raise RuntimeError('startup capture marker was not delivered')
                time.sleep(.1)
            command('am', 'broadcast', '--include-stopped-packages', '-n',
                    f'{package}/{RECEIVER}', '-a', 'com.archos.mediacenter.DEBUG')
            for cmd in ('dbgs 2', 'dbgsink 2', 'dbga 2'):
                debug(cmd)
            if env.get('PCM_PRESENTATION_CAPTURE', '0') == '1':
                pcm_armed = True
                debug('at_pcm_observe 600')
            if env.get('SURFACEFLINGER_CAPTURE', '0') == '1':
                compositor = subprocess.Popen([
                    'python3', str(Path(__file__).with_name('surfaceflinger_timing.py')), 'collect',
                    '--output', str(directory / 'surfaceflinger-samples.jsonl'), '--package', package,
                    '--interval', env.get('SURFACEFLINGER_INTERVAL_SEC', '2'),
                    '--layer', env.get('SURFACEFLINGER_LAYER', '')], env=env,
                    stdout=sf_log, stderr=subprocess.STDOUT, start_new_session=True)
            launch_marker = marker + '_LAUNCH'
            command('log', '-p', 'i', '-t', 'avos_test', launch_marker)
            command('am', 'start', '-W', '-n', f'{package}/{PLAYER}',
                    '-a', 'android.intent.action.VIEW', '-d', video, '-t', 'video/*',
                    '--ei', 'resume', '0', '--ei', 'position', '0', timeout=timeout)
            deadline = time.monotonic() + timeout
            ready_at = None
            hold_start = 0
            while True:
                captured = text()
                launched = captured.partition(launch_marker)[2]
                pcm_ready = not pcm_armed or bool(re.search(
                    r'pcm_present_observer: enabled seconds=600\b|audio_present_diag:.* source=pcm_observer\b',
                    captured.partition(marker)[2]))
                if ready_at is None and pcm_ready and evidence_ready(launched, env):
                    ready_at = time.monotonic()
                    hold_start = len(launched)
                    print(f'Playback detected; capturing {hold}s of startup baseline.', flush=True)
                if ready_at is not None and time.monotonic() - ready_at >= hold:
                    if not evidence_ready(launched[hold_start:], env):
                        raise RuntimeError('playback diagnostics stopped during startup baseline')
                    report['status'] = 'READY'
                    break
                if ready_at is None and time.monotonic() >= deadline:
                    raise RuntimeError('startup did not confirm live playback/diagnostics; see startup/logcat.log')
                time.sleep(.25)
        except BaseException as error:
            report['error'] = str(error) or type(error).__name__
            raise
        finally:
            if pcm_armed:
                try:
                    debug('at_pcm_observe 0')
                except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                    report['cleanup_error'] = str(error)
            stop(compositor)
            stop(collector)
            samples = directory / 'surfaceflinger-samples.jsonl'
            if samples.exists():
                try:
                    from surfaceflinger_timing import write_report
                    sf = write_report(samples, log)
                    report['surfaceflinger'] = dict(status=sf['status'],
                        report=str(directory / 'surfaceflinger-report.json'))
                except (OSError, ValueError, KeyError) as error:
                    report['surfaceflinger'] = dict(status='UNAVAILABLE', error=str(error))
            (directory / 'startup-report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report

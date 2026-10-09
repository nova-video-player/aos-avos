#!/usr/bin/env python3
"""Compositor correlation must not invent presentation or physical lipsync."""
import contextlib
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

from analyze_stress import config
from run_stress_campaign import generate_reports
from surfaceflinger_timing import PENDING, analyze, parse_latency, select_layer, write_report
from test_analyze_stress import fixture


LAYER = 'SurfaceView - org.courville.nova/Player#0'
REFRESH = 16_683_333


def snapshot(rows, generation=1, refresh=REFRESH):
    return dict(generation=generation, layer=LAYER, listing=dict(duration_ms=10),
                capture=dict(rc=0, duration_ms=40,
                             stdout=str(refresh) + '\n' + '\n'.join('\t'.join(map(str, r)) for r in rows)))


def playback(count=30, start=1_000_000_000, epoch=1, segment_marker=''):
    rows, logs = [], [segment_marker] if segment_marker else []
    for i in range(count):
        desired = start + round(i * REFRESH * 2.5)
        actual = ((desired + REFRESH - 1) // REFRESH) * REFRESH + 500_000
        rows.append((desired, actual, desired - 20_000_000))
        logs.append(f'video_render_diag: frame={i * 42} epoch={epoch} deadline_ns={desired} '
                    f'submit_ns={desired - 200_000_000} interval_ms=41.708 anchor_age_ms=20 '
                    f'phase_ms=-80 render_seq={i+1}')
    return rows, '\n'.join(logs)


class SurfaceFlingerTests(unittest.TestCase):
    def test_invalid_pending_and_zero_are_not_presentations(self):
        refresh, rows, counts = parse_latency(
            f'{REFRESH}\n0 0 0\n10 {PENDING} 5\n20 25 -1\noops\n30 35 10\n')
        self.assertEqual(refresh, REFRESH)
        self.assertEqual(rows, [(4, 30, 35, 10)])
        self.assertEqual(counts, dict(zero_or_invalid=2, pending=1, malformed=1))
        self.assertIsNone(parse_latency('Permission denied')[0])

    def test_only_unique_video_layer_is_selected(self):
        listing = 'org.courville.nova/Player#0\nBackground for -' + LAYER + '\n' + LAYER
        self.assertEqual(select_layer(listing, 'org.courville.nova'), LAYER)
        self.assertIsNone(select_layer(listing + '\n' + LAYER + '(BLAST)', 'org.courville.nova'))
        self.assertEqual(select_layer(listing, 'org.courville.nova', LAYER), LAYER)
        self.assertIsNone(select_layer(listing, 'org.courville.nova', 'missing'))

    def test_blast_child_selected_only_for_an_exact_single_parent_pair(self):
        base = 'SurfaceView[org.courville.nova/com.archos.mediacenter.video.player.PlayerActivity]'
        parent, child = base + '#192', base + '(BLAST)#193'
        self.assertEqual(select_layer(parent + '\n' + child, 'org.courville.nova'), child)
        self.assertEqual(select_layer(child + '\n' + parent, 'org.courville.nova'), child)
        self.assertEqual(select_layer(child, 'org.courville.nova'), child)
        self.assertEqual(select_layer(LAYER, 'org.courville.nova'), LAYER)  # Shield legacy naming.
        for listing in (parent + '\nSurfaceView[org.courville.nova/Other](BLAST)#194',
                        parent + '\n' + child + '\n' + base + '(BLAST)#195',
                        parent + '\n' + child + '\n' + LAYER):
            self.assertIsNone(select_layer(listing, 'org.courville.nova'))

    def test_overlapping_snapshots_deduplicate_and_allow_refresh_cadence(self):
        rows, log = playback()
        result = analyze([snapshot(rows[:20]), snapshot(rows[10:]), snapshot(rows[10:])], log)
        self.assertEqual(result['exact_matches'], 30)
        self.assertEqual(len(result['cadence']), 29)
        self.assertEqual(result['cadence_review_count'], 0)
        self.assertEqual(result['snapshots'][1]['overlap_rows'], 10)
        self.assertEqual(result['snapshots'][2]['new_rows'], 0)
        self.assertTrue(result['snapshots'][2]['unchanged_history'])
        self.assertEqual(result['segments'][0]['scheduled_phase_estimate_ms']['median'], -80)
        self.assertEqual(result['physical_lipsync'], 'unmeasured')

    def test_only_exact_unique_deadlines_match(self):
        rows, log = playback(3)
        altered = [(d + 1, a, r) for d, a, r in rows]
        self.assertEqual(analyze([snapshot(altered)], log)['exact_matches'], 0)
        self.assertEqual(analyze([snapshot(rows)], log + '\n' + log)['exact_matches'], 0)
        conflict = list(rows)
        conflict[1] = (rows[1][0], rows[1][1] + REFRESH, rows[1][2])
        self.assertEqual(analyze([snapshot(rows), snapshot(conflict)], log)['exact_matches'], 2)

    def test_repeated_filter_buffers_preserve_cadence_and_initial_baseline(self):
        rows, log = playback()
        plain = analyze([snapshot(rows)], log)
        filtered = '\n'.join('stream_audio: applying speed filter [atempo]\n' + line
                             for line in log.splitlines())
        result = analyze([snapshot(rows)], filtered)
        self.assertEqual(len(result['segments']), 1)
        self.assertEqual(len(result['cadence']), 29)
        self.assertEqual(result['baseline']['present_delay_median_ms'],
                         plain['baseline']['present_delay_median_ms'])
        changed = filtered.replace('render_seq=15', 'render_seq=15\napplying speed filter [sonic]')
        self.assertLess(len(analyze([snapshot(rows)], changed)['cadence']), 29)
        ramp = '\n'.join(line if i < 15 else line.replace('interval_ms=41.708', 'interval_ms=26.068')
                         for i, line in enumerate(log.splitlines()))
        self.assertEqual(len(analyze([snapshot(rows)], ramp)['cadence']), 28)

    def test_deferred_seek_preview_keeps_presentation_but_excludes_phase(self):
        rows, log = playback()
        log = log.replace('phase_ms=-80', 'phase_ms=-482643', 1)
        log = ('_stream_play_n_frames(n=1)\n'
               'SINK_REF_DEFERRED: seek_epoch=1 frame_time=0\n'
               'VIDEO_SEEK_TARGET_READY: epoch=1\n' + log)
        result = analyze([snapshot(rows)], log)
        self.assertEqual(result['exact_matches'], 30)
        self.assertTrue(result['matches'][0]['paused_or_preview'])
        self.assertEqual(result['segments'][0]['scheduled_phase_estimate_ms']['min'], -80)
        self.assertEqual(len(result['cadence']), 28)
        self.assertIsNone(result['baseline'])

    def test_paused_output_does_not_establish_phase_or_baseline(self):
        rows, log = playback()
        result = analyze([snapshot(rows)], 'android_sync: pause start\n' + log)
        self.assertEqual(result['exact_matches'], 30)
        self.assertIsNone(result['segments'][0]['scheduled_phase_estimate_ms'])
        self.assertFalse(result['cadence'])
        self.assertIsNone(result['baseline'])

    def test_delayed_presentation_is_reviewed_without_rebasing(self):
        rows, log = playback()
        baseline = analyze([snapshot(rows)], log)['baseline']
        late, late_log = playback(start=4_000_000_000, epoch=2,
                                  segment_marker='VIDEO_SEEK_TARGET_READY: epoch=2')
        late = [(d, a + 100_000_000, r) for d, a, r in late]
        result = analyze([snapshot(rows + late)], log + '\n' + late_log)
        self.assertEqual(result['baseline'], baseline)
        self.assertGreater(result['segments'][1]['baseline_delay_change_ms'], 90)
        self.assertEqual(result['segments'][1]['scheduled_phase_estimate_ms']['median'], -80)
        delayed = list(rows)
        d, a, r = delayed[15]
        delayed[15] = (d, a + 50_000_000, r)
        result = analyze([snapshot(delayed)], log)
        self.assertGreater(result['cadence_review_count'], 0)

    def test_do_not_compare_across_missing_frames_transitions_or_refresh_changes(self):
        rows, log = playback(5)
        missing = '\n'.join(line for i, line in enumerate(log.splitlines()) if i != 2)
        result = analyze([snapshot(rows)], missing)
        self.assertEqual(len(result['cadence']), 2)
        # Pending middle record is not bridged as though SF delivered adjacent frames.
        with_pending = rows[:2] + [(rows[2][0], PENDING, rows[2][2])] + rows[3:]
        self.assertEqual(len(analyze([snapshot(with_pending)], log)['cadence']), 2)
        lines = log.splitlines()
        lines.insert(2, 'WALLCLOCK_RESET: by pause resume')
        self.assertEqual(len(analyze([snapshot(rows)], '\n'.join(lines))['cadence']), 3)
        result = analyze([snapshot(rows[:2]), snapshot(rows[2:], generation=2)], log)
        self.assertEqual(len(result['cadence']), 3)
        self.assertIsNone(result['segments'][1]['baseline_delay_change_ms'])
        result = analyze([snapshot(rows[:2]), snapshot(rows[2:], refresh=20_000_000)], log)
        self.assertEqual(len(result['cadence']), 3)

    def test_no_late_baseline_and_no_data_is_not_a_pass(self):
        rows, log = playback(segment_marker='VIDEO_SEEK_TARGET_READY: epoch=1')
        self.assertIsNone(analyze([snapshot(rows)], log)['baseline'])
        result = analyze([snapshot([(0, 0, 0)])], log)
        self.assertEqual(result['status'], 'UNAVAILABLE')
        result = analyze([snapshot(rows)], '')
        self.assertEqual(result['status'], 'UNCORRELATED')
        self.assertEqual(result['verdict_effect'], 'none')

    def test_history_without_overlap_is_visible(self):
        rows, log = playback()
        result = analyze([snapshot(rows[:5]), snapshot(rows[20:])], log)
        self.assertTrue(result['snapshots'][1]['history_overlap_missing'])
        self.assertEqual(len(result['cadence']), 13)

    def test_report_integration_preserves_campaign_verdict(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            phase = root / 'resume-1'
            phase.mkdir()
            (root / 'logcat-session.log').write_text(fixture(sequence=True))
            (phase / 'analyzer-config.json').write_text(json.dumps(config()))
            rows, log = playback()
            samples = phase / 'surfaceflinger-samples.jsonl'
            samples.write_text(json.dumps(snapshot(rows)) + '\n')
            (phase / 'logcat.log').write_text(log)
            write_report(samples, phase / 'logcat.log')
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(generate_reports(root, 0), 0)
            result = json.loads((root / 'run-report.json').read_text())
            self.assertEqual(result['surfaceflinger'][0]['status'], 'CORRELATED')
            self.assertEqual(result['physical_lipsync'], 'unmeasured')
            later = root / 'seek-1'
            later.mkdir()
            shifted = [(d, a + 100_000_000, r) for d, a, r in rows]
            later_samples = later / samples.name
            later_samples.write_text(json.dumps(snapshot(shifted)) + '\n')
            (later / 'logcat.log').write_text(log)
            write_report(later_samples, later / 'logcat.log')
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(generate_reports(root, 0), 0)
            result = json.loads((root / 'run-report.json').read_text())
            comparison = result['surfaceflinger'][1]['initial_session_baseline_comparison']
            self.assertAlmostEqual(comparison[0]['baseline_delay_change_ms'], 100, delta=1)
            # Missing initial baseline is not silently replaced by the next phase.
            initial = phase / 'surfaceflinger-report.json'
            first_report = json.loads(initial.read_text())
            first_report['baseline'] = None
            initial.write_text(json.dumps(first_report))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(generate_reports(root, 0), 0)
            result = json.loads((root / 'run-report.json').read_text())
            self.assertIsNone(result['surfaceflinger_initial_baseline'])
            self.assertIsNone(result['surfaceflinger'][1]['initial_session_baseline_comparison'][0]['baseline_delay_change_ms'])

    def test_pcm_confirmation_rejects_stale_or_unrelated_evidence(self):
        common = Path(__file__).resolve().parent / 'stress_common.sh'
        marker = '1000.000 avos_test: AVOS_TEST_POLL_PCM_ARM_test\n'
        sample = 'avos_player: audio_present_diag: presented=48000 source=pcm_observer generation=1\n'
        ack = 'avos_player: pcm_present_observer: enabled seconds=600\n'
        cases = [
            (marker + '1001.000 ' + sample, True),
            ('1001.000 ' + sample + marker, True),  # Delivery can reorder threads.
            (marker + '999.000 ' + sample, False),
            (marker + '999.000 ' + ack, False),
            ('1001.000 ' + sample, False),  # Lost marker cannot establish freshness.
            (marker + '1001.000 ' + sample.replace('pcm_observer', 'playhead'), False),
            (marker + '1001.000 ' + ack, True),
            (marker + '1001.000 ' + ack.replace('600', '0'), False),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            raw = Path(tmp) / 'logcat.log'
            for log, ready in cases:
                with self.subTest(log=log):
                    raw.write_text(log)
                    run = subprocess.run(['bash', '-c', '. "$1"; RAW_LOG="$2"; '
                        'PCM_OBSERVER_MARKER=AVOS_TEST_POLL_PCM_ARM_test; stress_pcm_observer_ready',
                        'test', str(common), str(raw)], capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0 if ready else 1, run.stderr)

    def test_shared_driver_cleanup_and_optional_failure_do_not_change_exit_code(self):
        # Exercise the real shell hooks and collector, without touching a device.
        # The fake logcat stays alive until cleanup, like adb's continuous stream.
        script_dir = Path(__file__).resolve().parent
        rows, log = playback()
        raw = snapshot(rows)['capture']['stdout']
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            adb = root / 'adb'
            adb.write_text(f'#!{sys.executable}\n' +
                           'import os, shlex, sys, time\nfrom pathlib import Path\n'
                           'args = sys.argv[1:]\n'
                           'if args[:2] == ["logcat", "-G"]: sys.exit(0)\n'
                           'if args and args[0] == "shell":\n'
                           '    remote = shlex.split(args[1]) if len(args) == 2 else args[1:]\n'
                           '    if remote[0] == "log":\n'
                           '        Path(os.environ["FAKE_COMMANDS"] + ".marker").write_text(remote[-1])\n'
                           '        sys.exit(0)\n'
                           '    if remote[0] == "am":\n'
                           '        cmd = remote[remote.index("cmd") + 1]\n'
                           '        with open(os.environ["FAKE_COMMANDS"], "a") as f: f.write(cmd + "\\n")\n'
                           '        sys.exit(0)\n'
                           '    if os.environ.get("FAKE_UNAVAILABLE") == "1": sys.exit(1)\n'
                           f'    print({LAYER!r} if "--list" in remote else {raw!r})\n'
                           'else:\n'
                           f'    print({log!r}, flush=True)\n'
                           '    seen = 0\n'
                           '    marked = False\n'
                           '    while True:\n'
                           '        path = Path(os.environ["FAKE_COMMANDS"])\n'
                           '        marker = Path(str(path) + ".marker")\n'
                           '        if marker.exists() and not marked:\n'
                           '            print("1000.000 avos_test: " + marker.read_text(), flush=True)\n'
                           '            marked = True\n'
                           '        cmds = path.read_text().splitlines() if path.exists() else []\n'
                           '        if len(cmds) > seen:\n'
                           '            if os.environ.get("FAKE_EXIT_ON_COMMAND"): sys.exit(0)\n'
                           '            time.sleep(float(os.environ.get("FAKE_ACK_DELAY", "0")))\n'
                           '        if not os.environ.get("FAKE_NO_ACK"):\n'
                           '            for cmd in cmds[seen:]: print("1001.000 avos_player: pcm_present_observer: enabled seconds=" + cmd.split()[-1], flush=True)\n'
                           '        if len(cmds) > seen and os.environ.get("FAKE_SAMPLE"):\n'
                           '            print("1001.000 avos_player: audio_present_diag: presented=48000 source=pcm_observer generation=1 age_ms=0 sample_ns=1001000000 query_start_ns=1001000000", flush=True)\n'
                           '        seen = len(cmds)\n'
                           '        time.sleep(.02)\n')
            adb.chmod(0o755)
            for enabled, unavailable, pcm, no_ack, delay, died, sample in (
                    (0, 0, 0, 0, 0, 0, 0), (1, 0, 0, 0, 0, 0, 0), (1, 1, 0, 0, 0, 0, 0),
                    (0, 0, 1, 0, 0, 0, 0), (0, 0, 1, 1, 0, 0, 0),
                    (1, 0, 1, 0, 3, 0, 0), (0, 0, 1, 0, 0, 1, 0),
                    (1, 0, 1, 1, 0, 0, 1)):
                output = root / f'{enabled}-{unavailable}-{pcm}-{no_ack}-{delay}-{died}-{sample}'
                output.mkdir()
                env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ['PATH'],
                           SURFACEFLINGER_CAPTURE=str(enabled), FAKE_UNAVAILABLE=str(unavailable),
                           PCM_PRESENTATION_CAPTURE=str(pcm), FAKE_COMMANDS=str(output / 'commands'),
                           FAKE_NO_ACK='1' if no_ack else '', FAKE_ACK_DELAY=str(delay),
                           FAKE_EXIT_ON_COMMAND='1' if died else '', FAKE_SAMPLE='1' if sample else '')
                # Clear a developer's layer override; exercise discovery instead.
                env.pop('SURFACEFLINGER_LAYER', None)
                shell = f'''
set -u
SCRIPT_DIR={shlex.quote(str(script_dir))}
OUTPUT_DIR={shlex.quote(str(output))}
RAW_LOG="$OUTPUT_DIR/logcat.log"
PACKAGE=org.courville.nova
ARM_UNDERRUN=0
LOGCAT_KEEP='video_render_diag:|pcm_present_observer:|audio_present_diag:|AVOS_TEST_POLL'
. "$SCRIPT_DIR/stress_common.sh"
stress_preflight() {{ :; }}
fail() {{ printf '%s\\n' "$*" >&2; exit 2; }}
trap stress_stop_logcat EXIT
stress_start_logcat
exit 7
'''
                run = subprocess.run(['bash', '-c', shell], env=env, capture_output=True,
                                     text=True, timeout=15)
                self.assertEqual(run.returncode, 2 if (no_ack and not sample) or died else 7, run.stdout + run.stderr)
                if no_ack and not sample:
                    self.assertIn('confirmation not captured within 8s', run.stderr)
                if died:
                    self.assertIn('logcat collector stopped while waiting', run.stderr)
                if pcm:
                    self.assertEqual((output / 'commands').read_text().splitlines(),
                                     ['at_pcm_observe 600', 'at_pcm_observe 0'])
                report = output / 'surfaceflinger-report.json'
                if enabled:
                    result = json.loads(report.read_text())
                    self.assertEqual(result['status'], 'UNAVAILABLE' if unavailable else 'CORRELATED')
                    self.assertGreaterEqual(len(result['snapshots']), 2)  # Initial + final capture.
                else:
                    self.assertFalse(report.exists())


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
"""Regression fixtures for the actual analyzer used by device scripts."""
import unittest
from analyze_stress import analyze, config, high_windows, log_order, records


def fixture(mode='resume', seconds=3, speed=1, start=1000, sequence=False):
    rows = []
    marker = 'WALLCLOCK_RESET: by pause resume' if mode == 'resume' else 'VIDEO_SEEK_TARGET_READY: epoch=1'
    if mode != 'speed':
        rows.append(f'{start:.3f} {marker}')
    for i in range(seconds * 25 + 1):
        t = start + .01 + i * .04
        ns = round(t * 1e9)
        frame = i * 40
        rows += [
            f'{t:.3f} put_time_calc: speed={speed:.3f} allow_reanchor=0 disc=0',
            f'{t:.3f} audiotrack_write: wrote 7680 out of 7680 bytes',
            f'{t:.3f} audio_present_diag: presented={i*1920} source=playhead age_ms=0 sample_ns={ns}',
            f'{t:.3f} video_sched_diag: frame={frame} audio={frame} frame_minus_heard=0 seek_epoch=1',
            f'{t:.3f} video_render_diag: frame={frame} epoch=1 deadline_ns={ns+100000000} submit_ns={ns} interval_ms=40 anchor_age_ms=10 phase_ms=0' +
            (f' render_seq={i+1}' if sequence else ''),
        ]
    rows.append(f'{start+seconds+.02:.3f} AVOS_TEST_POLL_1')
    return '\n'.join(rows)


class AnalyzerTests(unittest.TestCase):
    def check(self, text, mode='resume', **overrides):
        return analyze(text, mode, config(overrides))

    def test_healthy_recovery(self):
        for mode in ('seek', 'resume'):
            self.assertEqual(self.check(fixture(mode), mode)['healthy'], 1)

    def test_older_observation_is_not_a_presentation_reset(self):
        text = fixture()
        row = '1001.010 audio_present_diag: presented=48000 source=playhead age_ms=0 sample_ns=1001010000000'
        older = '1001.010 audio_present_diag: presented=47744 source=playhead age_ms=0 sample_ns=1001008000000'
        self.assertIn(row, text)
        result = self.check(text.replace(row, row + '\n' + older))
        self.assertEqual(result['healthy'], 1, result)
        self.assertEqual(result['presentation_out_of_order'], 1)
        # Clearly newer observations and unclocked reversals still fail.
        for bad in (older.replace('1001008000000', '1001011000000'),
                    older.replace(' sample_ns=1001008000000', '')):
            self.assertIn('presentation_reset', self.check(text.replace(row, row + '\n' + bad))['reason'])

    def test_legacy_playhead_timestamp_tie_is_insufficient_evidence(self):
        row = '1001.010 audio_present_diag: presented=48000 source=playhead age_ms=0 sample_ns=1001010000000'
        reversal = row.replace('presented=48000', 'presented=46976')
        text = fixture().replace(row, row + '\n' + reversal)
        result = self.check(text)
        self.assertEqual(result['verdict'], 'INSUFFICIENT_EVIDENCE', result)
        self.assertEqual(result['presentation_ambiguous_order'], 1)
        self.assertNotIn('presentation_reset', result['reason'])
        # Equal DAC timestamp counters are not ambiguous JNI query times.
        result = self.check(text.replace('source=playhead', 'source=timestamp'))
        self.assertIn('presentation_reset', result['reason'])

    def test_overlapping_playhead_queries_do_not_prove_reset(self):
        row = '1001.010 audio_present_diag: presented=48000 source=playhead age_ms=0 sample_ns=1001010000000'
        first = row + ' query_start_ns=1001009900000'
        reversal = ('1001.010 audio_present_diag: presented=46976 source=playhead '
                    'age_ms=0 sample_ns=1001010500000 query_start_ns=1001009800000')
        text = fixture().replace(row, first + '\n' + reversal)
        result = self.check(text)
        self.assertEqual(result['verdict'], 'INSUFFICIENT_EVIDENCE', result)
        self.assertEqual(result['presentation_ambiguous_order'], 1)
        # A subsequent non-overlapping reversal must still fail, including
        # when the earlier ambiguous observation already reduced coverage.
        later = reversal.replace('1001010500000', '1001011000000').replace(
            '1001009800000', '1001010600000')
        result = self.check(text.replace(reversal, reversal + '\n' + later))
        self.assertEqual(result['verdict'], 'FAIL', result)
        self.assertIn('presentation_reset', result['reason'])
        result = self.check(fixture().replace(row, first + '\n' + later))
        self.assertIn('presentation_reset', result['reason'])

    def test_ambiguous_queries_cannot_supply_fresh_progress(self):
        rows = fixture().splitlines()
        for i, row in enumerate(rows):
            if 'audio_present_diag:' not in row:
                continue
            t = float(row.split()[0])
            if t <= 1001.01:
                rows[i] += ' query_start_ns=1000000000000'
            else:
                rows[i] = (row.split('presented=')[0] +
                           f'presented=0 source=playhead age_ms=0 sample_ns={round(t*1e9)} '
                           'query_start_ns=1000000000000')
        result = self.check('\n'.join(rows))
        self.assertIn('stale_presentation', result['reason'])
        self.assertIn('missing_presentation_order', result['reason'])

    def test_old_observations_cannot_prove_continued_presentation(self):
        rows = fixture().splitlines()
        for i, row in enumerate(rows):
            if 'audio_present_diag:' in row and float(row.split()[0]) > 1001:
                rows[i] = row.split('presented=')[0] + 'presented=0 source=playhead age_ms=0 sample_ns=1'
        result = self.check('\n'.join(rows))
        self.assertIn('stale_presentation', result['reason'])

    def test_audiotrack_speed_backend_requires_hardware_readback(self):
        text = fixture('speed', seconds=6, speed=1.6)
        text += '\n1006.020 put_time_calc: speed=1.000 allow_reanchor=0 disc=0'
        hw = '\n1001.020 at_speed_hw: req=1.600 applied=1.600 readback_ok=1'
        def check_hardware(value):
            ordered = '\n'.join(sorted(value.splitlines(), key=lambda row: float(row.split()[0])))
            return self.check(ordered, 'speed', REQUIRE_FILTER='audiotrack')
        good = check_hardware(text + hw)
        self.assertEqual(good['healthy'], 1, good)
        for bad in (text, text + hw.replace('readback_ok=1', 'readback_ok=0'),
                    text + hw.replace('applied=1.600', 'applied=1.000')):
            self.assertIn('audiotrack_speed_not_confirmed',
                          check_hardware(bad)['reason'])
        filtered = text + hw + '\n1001.030 stream_audio: applying speed filter [sonic]'
        self.assertIn('wrong_filter', check_hardware(filtered)['reason'])

    def queued_preview_fixture(self):
        prefix = ('999.800 _stream_play_n_frames(n=10, time=0)\n'
                  '999.900 SINK_REF_DEFERRED: frame_time=0 audio_time=-1 seek_epoch=1\n'
                  '1000.000 WALLCLOCK_RESET: by pause resume\n')
        return prefix + fixture('seek', sequence=True).replace(
            'deadline_ns=1000110000000', 'deadline_ns=999968000000')

    def test_queued_preview_survives_resume_without_hiding_lateness(self):
        from analyze_stress_recording import analyze_recording
        text = self.queued_preview_fixture()
        live = self.check(text, 'seek')
        self.assertEqual(live['healthy'], 1, live)
        self.assertEqual(live['seek_preview_count'], 1)
        self.assertEqual(live['seek_preview_max_lateness_ms'], 42)
        self.assertEqual(live['max_lateness_ms'], 0)
        self.assertEqual(live['render_count'], 75)
        self.assertAlmostEqual(live['first_render_ms'], 50)
        report = analyze_recording(text, config({}))
        self.assertEqual(report['verdict'], 'NO_ISSUES_OBSERVED', report)
        self.assertEqual(report['segments'][0]['findings'][0]['kind'], 'queued_seek_preview')
        # The very next regular frame remains subject to the strict limit.
        late = text.replace('deadline_ns=1000150000000', 'deadline_ns=999998000000')
        self.assertIn('late_submission', self.check(late, 'seek')['reason'])
        self.assertEqual(analyze_recording(late, config({}))['verdict'], 'ISSUES_OBSERVED')

    def test_preview_requires_matching_evidence_and_is_consumed_once(self):
        from analyze_stress_recording import analyze_recording
        text = self.queued_preview_fixture()
        for broken in (text.replace('seek_epoch=1\n', 'seek_epoch=2\n'),
                       text.replace('frame_time=0', 'frame_time=40'),
                       text.replace('_stream_play_n_frames(n=10, time=0)', 'unidentified seek'),
                       text.replace('1000.000 WALLCLOCK_RESET', '999.950 stream_open:\n1000.000 WALLCLOCK_RESET')):
            self.assertIn('late_submission', self.check(broken, 'seek')['reason'])
            self.assertEqual(analyze_recording(broken, config({}))['verdict'], 'ISSUES_OBSERVED')
        matched = [r for r in records(text) if r[2].get('_seek_preview')]
        self.assertEqual(len(matched), 1)
        duplicate = text + '\n' + matched[0][1]
        self.assertEqual(sum(bool(r[2].get('_seek_preview')) for r in records(duplicate)), 1)

    def test_preview_cannot_supply_coverage_or_mask_missing_sequence(self):
        from analyze_stress_recording import analyze_recording
        text = self.queued_preview_fixture()
        missing = '\n'.join(r for r in text.splitlines() if not r.endswith('render_seq=2'))
        self.assertIn('missing_render_records', self.check(missing, 'seek')['reason'])
        self.assertEqual(analyze_recording(missing, config({}))['verdict'], 'INSUFFICIENT_EVIDENCE')
        only_preview = '\n'.join(r for r in text.splitlines() if
                                'video_render_diag:' not in r or r.endswith('render_seq=1'))
        result = self.check(only_preview, 'seek')
        self.assertIn('missing_render_timing', result['reason'])
        self.assertIn('seek_startup_latency', result['reason'])
        self.assertEqual(analyze_recording(only_preview, config({}))['verdict'], 'INSUFFICIENT_EVIDENCE')
        overdue = text.replace('deadline_ns=999968000000', 'deadline_ns=998000000000')
        self.assertIn('seek_preview_startup_late', self.check(overdue, 'seek')['reason'])
        self.assertEqual(analyze_recording(overdue, config({}))['verdict'], 'ISSUES_OBSERVED')

    def test_capture_filters_preserve_preview_identity(self):
        from pathlib import Path
        import re
        import subprocess
        for name, variable in (('stress_common.sh', 'LOGCAT_KEEP'), ('stress_campaign.sh', 'SESSION_KEEP')):
            text = Path(__file__).with_name(name).read_text()
            assignments = '\n'.join(line.strip() for line in text.splitlines()
                                    if line.strip().startswith(variable + '='))
            result = subprocess.run(['bash', '-c', assignments + '\nprintf "%s" "$' + variable + '"'],
                                    check=True, capture_output=True, text=True)
            for marker in ('_stream_play_n_frames(n=10, time=0)',
                           'SINK_REF_DEFERRED: frame_time=0 seek_epoch=1'):
                self.assertRegex(marker, re.compile(result.stdout))

    def test_delayed_logd_summary_is_not_playback_clock_reversal(self):
        text = fixture() + '\n1001.000 3677 3883 I chatty  : uid=1000(system) Binder identical 3 lines'
        self.assertEqual(self.check(text)['healthy'], 1)
        self.assertIn('log_loss', self.check(text.replace('identical 3 lines', 'dropped 3 lines'))['reason'])
        self.assertIn('log_time_reversed', self.check(text.replace('I chatty  :', 'D avos_player:'))['reason'])

    def test_render_sequence_distinguishes_missing_logs_from_bad_cadence(self):
        from analyze_stress_recording import analyze_recording
        for sequence in (False, True):
            text = fixture(sequence=sequence)
            missing = '\n'.join(r for r in text.splitlines() if 'video_render_diag:' not in r or
                                not 1001.04 < float(r.split()[0]) < 1001.10)
            reason = 'missing_render_records' if sequence else 'missing_render_continuity'
            live = self.check(missing)
            self.assertEqual(live['healthy'], 0)
            self.assertEqual(live['verdict'], 'INSUFFICIENT_EVIDENCE', live)
            self.assertIn(reason, live['reason'])
            self.assertNotIn('scheduled_judder', live['reason'])
            self.assertEqual(live['missing_render_records'], 2 if sequence else 0)
            review = analyze_recording(missing, config({}))
            self.assertEqual(review['verdict'], 'INSUFFICIENT_EVIDENCE')
            self.assertIn(reason, review['evidence_gaps'])
            self.assertEqual(review['segments'][0]['findings'][0]['kind'], reason)
        # Consecutive sequence numbers establish adjacency even if media skips.
        text = fixture(sequence=True).replace('frame=1040 epoch=1', 'frame=1120 epoch=1')
        text = text.replace('deadline_ns=1001150000000', 'deadline_ns=1001230000000')
        self.assertIn('scheduled_judder', self.check(text)['reason'])

    def test_render_sequence_healthy_and_large_integer_precision(self):
        import re
        for base in (0, 2**63):
            text = re.sub(r'render_seq=(\d+)', lambda m: f'render_seq={int(m[1])+base}',
                          fixture(sequence=True))
            result = self.check(text)
            self.assertEqual(result['healthy'], 1, result)
            self.assertEqual(result['render_sequence'], 'available')

    def test_missing_sequence_does_not_hide_other_faults(self):
        from analyze_stress_recording import analyze_recording
        text = '\n'.join(r for r in fixture(sequence=True).splitlines()
                         if 'render_seq=27 ' not in r + ' ')
        # Still report an independently late submitted frame and persistent phase.
        text = text.replace('submit_ns=1002010000000', 'submit_ns=1002180000000')
        text = text.replace('phase_ms=0', 'phase_ms=150')
        result = self.check(text)
        for reason in ('missing_render_records', 'late_submission', 'sustained_av_phase_error'):
            self.assertIn(reason, result['reason'])
        self.assertEqual(result['verdict'], 'FAIL', result)
        self.assertEqual(analyze_recording(text, config({}))['verdict'], 'ISSUES_OBSERVED')

    def test_sequence_duplicates_and_mixed_logs_are_incomplete(self):
        from analyze_stress_recording import analyze_recording
        text = fixture(sequence=True)
        row = next(r for r in text.splitlines() if r.endswith('render_seq=27'))
        for broken, reason in (
                (text.replace(row, row + '\n' + row), 'missing_render_sequence_order'),
                (text.replace('render_seq=27\n', 'render_seq=20\n'), 'missing_render_sequence_order'),
                (text.replace(' render_seq=27\n', '\n'), 'missing_render_sequence')):
            report = analyze_recording(broken, config({}))
            self.assertEqual(report['verdict'], 'INSUFFICIENT_EVIDENCE', report)
            self.assertIn(reason, report['evidence_gaps'])

    def test_sequence_validation_and_epoch_boundaries(self):
        from analyze_stress_recording import analyze_recording
        text = fixture(sequence=True)
        for invalid in ('0', '-1', '1.5', 'nan', str(2**64)):
            bad = text.replace('render_seq=27\n', f'render_seq={invalid}\n')
            with self.assertRaises(ValueError):
                self.check(bad)
            self.assertIn('malformed_records', analyze_recording(bad, config({}))['evidence_gaps'])
        # Epoch/session boundaries must not compare two unrelated sequences.
        second = fixture(start=1005, sequence=True).replace('epoch=1', 'epoch=2')
        report = analyze_recording(text + '\n' + second, config({}))
        self.assertEqual(report['verdict'], 'NO_ISSUES_OBSERVED')

    def test_short_segment_sequence_gap_cannot_be_hidden_by_later_coverage(self):
        from analyze_stress_recording import analyze_recording
        short = fixture(seconds=1, sequence=True)
        short = '\n'.join(r for r in short.splitlines() if not r.endswith('render_seq=5'))
        report = analyze_recording(short + '\n' + fixture(start=1005, sequence=True), config({}))
        self.assertEqual(report['verdict'], 'INSUFFICIENT_EVIDENCE')
        self.assertIn('missing_render_records', report['evidence_gaps'])

    def test_phase_duration_does_not_bridge_missing_render_records(self):
        text = fixture(sequence=True).splitlines()
        for i, row in enumerate(text):
            if 'video_render_diag:' not in row:
                continue
            stamp = float(row.split()[0])
            if 1001.00 <= stamp <= 1001.37:
                text[i] = row.replace('phase_ms=0', 'phase_ms=150')
            if row.endswith('render_seq=31'):
                text[i] = ''
        result = self.check('\n'.join(text))
        self.assertIn('missing_render_records', result['reason'])
        self.assertNotIn('sustained_av_phase_error', result['reason'])

    def test_seek_startup_has_separate_budget(self):
        text = '1000.000 VIDEO_SEEK_TARGET_READY: epoch=1\n' + fixture('speed', start=1000.3)
        result = self.check(text, 'seek')
        self.assertEqual(result['healthy'], 1, result)
        self.assertAlmostEqual(result['first_video_ms'], 310)
        self.assertAlmostEqual(result['first_render_ms'], 310)
        self.assertLess(result['max_video_gap'], 250)
        self.assertIn('seek_startup_latency', self.check(text, 'seek', SEEK_STARTUP_MAX_MS=300)['reason'])
        slow = '1000.000 VIDEO_SEEK_TARGET_READY: epoch=1\n' + fixture('speed', start=1001.1)
        self.assertIn('seek_startup_latency', self.check(slow, 'seek')['reason'])
        # Neither a mid-playback gap nor a silent tail gets the startup budget.
        gap = '\n'.join(r for r in text.splitlines() if not 1001 < float(r.split()[0]) < 1001.4)
        for broken in (gap, text + '\n1004.000 AVOS_TEST_POLL_2'):
            result = self.check(broken, 'seek')
            self.assertIn('video_feed_gap', result['reason'])
            self.assertIn('audio_write_gap', result['reason'])

    def test_resume_startup_has_separate_budget(self):
        text = '1000.000 WALLCLOCK_RESET: by pause resume\n' + fixture('speed', start=1000.3)
        result = self.check(text)
        self.assertEqual(result['verdict'], 'PASS', result)
        self.assertAlmostEqual(result['first_video_ms'], 310)
        self.assertAlmostEqual(result['first_write_ms'], 310)
        self.assertLess(result['max_video_gap'], 250)
        self.assertIn('resume_latency', self.check(text, RESUME_LATENCY_MAX_MS=300)['reason'])
        slow = '1000.000 WALLCLOCK_RESET: by pause resume\n' + fixture('speed', start=1001.1)
        result = self.check(slow)
        self.assertEqual(result['verdict'], 'FAIL', result)
        self.assertIn('resume_latency', result['reason'])
        # Excluding the initial wait must not waive stalls during playback or
        # a silent tail, even when renderer evidence is also incomplete.
        gap = '\n'.join(r for r in text.splitlines() if not 1001 < float(r.split()[0]) < 1001.4)
        for broken in (gap, text + '\n1004.000 AVOS_TEST_POLL_2'):
            result = self.check(broken)
            self.assertIn('video_feed_gap', result['reason'])
            self.assertIn('audio_write_gap', result['reason'])
            self.assertEqual(result['verdict'], 'FAIL', result)

    def test_seek_startup_requires_render_and_progress(self):
        text = fixture('seek')
        no_render = '\n'.join(r for r in text.splitlines() if 'video_render_diag:' not in r)
        self.assertIn('seek_startup_latency', self.check(no_render, 'seek')['reason'])
        self.assertIn('missing_render_timing', self.check(no_render, 'seek')['reason'])
        self.assertEqual(self.check(no_render, 'seek', REQUIRE_RENDER_TIMING=0)['healthy'], 1)
        self.assertEqual(self.check('1000.000 VIDEO_SEEK_TARGET_READY: epoch=1\n'
                                   '1002.000 AVOS_TEST_POLL_2', 'seek')['healthy'], 0)

    def burst_fixture(self):
        return '\n'.join([
            '998.000 AVOS_TEST_BURST_BEGIN',
            '998.010 android_sync: pause start at 998010',
            '998.500 WALLCLOCK_RESET: by pause resume',
            '998.501 audio_resume_route: passthrough=1',
            '998.510 audiotrack_write: wrote 7680 out of 7680 bytes',
            '998.520 android_sync: pause start at 998520',
            # Input injection finishes before the final native callback.
            '999.990 AVOS_TEST_BURST_END', fixture()])

    def test_burst_checks_recovery_after_last_resume(self):
        result = self.check(self.burst_fixture(), RESUME_BURST_PAIRS=2)
        self.assertEqual(result['healthy'], 1, result)
        self.assertEqual(result['burst_pauses'], 2)
        self.assertEqual(result['burst_resumes'], 2)
        self.assertLess(result['max_write_gap'], 250)
        self.assertIn('audio_write_gap', self.check(self.burst_fixture())['reason'])
        from analyze_stress_recording import analyze_recording
        review = analyze_recording(self.burst_fixture(), config({'RESUME_BURST_PAIRS': 2}))
        self.assertEqual(review['verdict'], 'NO_ISSUES_OBSERVED')

    def test_burst_recording_gate_waits_for_coverage_with_original_deadline(self):
        import json
        import os
        from pathlib import Path
        import subprocess
        import tempfile
        script_dir = Path(__file__).resolve().parent
        script = (script_dir / 'stress_resume_validate.sh').read_text()
        # Exercise the production polling loop with a healthy live verdict and
        # the real recording analyzer; replace only device and wall-clock I/O.
        loop = script[script.index('\tstart_time=$(date +%s)'):script.index('\n\ti=$((i + 1))')]
        short = '\n'.join(r for r in fixture(seconds=2).splitlines()
                          if 'video_render_diag:' not in r or float(r.split()[0]) < 1001.95)
        complete = fixture()
        cases = [
            ('coverage_arrives', [short, complete], 3, 0, 2, 'NO_ISSUES_OBSERVED'),
            ('coverage_timeout', [short, short], 2, 3, 2, 'INSUFFICIENT_EVIDENCE'),
            ('timing_failure', [complete + '\n1003.021 late frame drop'],
             3, 1, 1, 'ISSUES_OBSERVED'),
            ('capture_loss', [complete + '\n1003.021 chatty dropped 3 lines'] * 2,
             2, 3, 2, 'INSUFFICIENT_EVIDENCE'),
        ]
        setup = r'''
set -u
i=1
target=burst
BURST_PAIRS=1
PLAYER_PID=123
PACKAGE=org.courville.nova
WRITE_GAP_MAX_MS=250
VIDEO_GAP_MAX_MS=250
UNDERRUN_MAX=0
POLL_MS=500
LATEST="$OUTPUT_DIR/current-resume.log"
RESULTS="$OUTPUT_DIR/results.txt"
RAW_LOG="$LATEST"
poll_count=0
date() { printf '%s\n' "$poll_count"; }
adb() { printf '123\n'; }
sleep() { :; }
fail() { printf 'ERROR: %s\n' "$1"; exit 2; }
. "$SCRIPT_DIR/stress_common.sh"
capture_failure() { stress_failure_status "$1"; printf '%s: %s\n' "$FAILURE_STATUS" "$1"; }
analyse_segment() { printf 'healthy=1 reason=ok\n'; }
stress_snapshot() {
    poll_count=$((poll_count + 1))
    printf 'SNAPSHOT=%s\n' "$poll_count"
    cp "$OUTPUT_DIR/snapshot-$poll_count.log" "$LATEST" || exit 2
}
'''
        for name, snapshots, timeout, rc, polls, verdict in cases:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                output = Path(tmp)
                (output / 'analyzer-config.json').write_text(json.dumps(config({})))
                for i, snapshot in enumerate(snapshots, 1):
                    (output / f'snapshot-{i}.log').write_text(snapshot)
                result = subprocess.run(['bash', '-c', setup + loop], env=dict(
                    os.environ, OUTPUT_DIR=tmp, SCRIPT_DIR=str(script_dir), TIMEOUT_SEC=str(timeout)),
                    capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, rc, result.stdout + result.stderr)
                self.assertEqual(result.stdout.count('SNAPSHOT='), polls, result.stdout)
                self.assertEqual(json.loads((output / 'burst-1-review.json').read_text())['verdict'], verdict)
                self.assertEqual((output / 'burst-1.log').read_text(), snapshots[-1])
                if rc == 0:
                    self.assertIn('PASS ', result.stdout)
                elif verdict == 'INSUFFICIENT_EVIDENCE':
                    self.assertIn('full-burst evidence timeout', result.stdout)
                    self.assertNotIn('PASS ', result.stdout)
                else:
                    self.assertIn('full-burst timing review found issues', result.stdout)

    def test_burst_requires_all_ordered_transitions_and_markers(self):
        for old, new, reason in [
                ('998.010 android_sync: pause start at 998010', '', 'burst_transition_mismatch'),
                ('998.500 WALLCLOCK_RESET: by pause resume', '', 'burst_transition_mismatch'),
                ('AVOS_TEST_BURST_END', 'missing', 'missing_burst_markers'),
                ('AVOS_TEST_BURST_BEGIN', 'missing', 'missing_burst_markers'),
                ('998.520 android_sync: pause start at 998520',
                 '998.520 WALLCLOCK_RESET: by pause resume', 'burst_transition_mismatch')]:
            with self.subTest(old=old):
                result = self.check(self.burst_fixture().replace(old, new), RESUME_BURST_PAIRS=2)
                self.assertEqual(result['healthy'], 0)
                self.assertIn(reason, result['reason'])

    def test_burst_retains_faults_before_final_resume(self):
        for message, reason in [
                ('android_sync: late frame drop', 'late_frames'),
                ('AudioTrack underruns: total=2 delta=+2', 'audio_underruns'),
                ('AUDIO_STARVED: waiting=300', 'audio_starved'),
                ('compressed write failed', 'compressed_write_failure')]:
            text = self.burst_fixture().replace('998.510 audiotrack_write:',
                                               f'998.509 {message}\n998.510 audiotrack_write:')
            self.assertIn(reason, self.check(text, RESUME_BURST_PAIRS=2)['reason'])

    def test_burst_configuration_rejects_fractional_pairs(self):
        for value in ('1.5', '-1', '101'):
            with self.assertRaises(ValueError):
                config({'RESUME_BURST_PAIRS': value})

    def test_burst_command_runs_keys_on_device_without_adb_roundtrips(self):
        import os
        from pathlib import Path
        import subprocess
        import tempfile
        script = Path(__file__).with_name('stress_resume_validate.sh').resolve()
        with tempfile.TemporaryDirectory() as tmp:
            for name in ('adb', 'input', 'sleep', 'log'):
                command = Path(tmp)/name
                command.write_text('#!/bin/sh\nprintf "' + name + ' %s\\n" "$*"\n')
                command.chmod(0o755)
            env = dict(os.environ, PATH=tmp+os.pathsep+os.environ['PATH'], DRY_RUN='1',
                       BURST_PAIRS='08', BURST_KEY='KEYCODE_DPAD_CENTER')
            for gap in ('0', '150'):
                env['BURST_GAP_MS'] = gap
                generated = subprocess.run(['bash', str(script)], env=env, capture_output=True, text=True)
                self.assertEqual(generated.returncode, 0, generated.stderr)
                self.assertNotIn('adb ', generated.stdout)
                executed = subprocess.run(['sh', '-c', generated.stdout], env=env,
                                          capture_output=True, text=True)
                self.assertEqual(executed.returncode, 0, executed.stderr)
                lines = executed.stdout.splitlines()
                self.assertEqual(executed.stdout.count('KEYCODE_DPAD_CENTER'), 16)
                self.assertEqual(sum(line.startswith('input ') for line in lines), 1 if gap == '0' else 16)
                self.assertEqual(sum(line == 'sleep 0.150' for line in lines), 0 if gap == '0' else 15)
                self.assertIn('AVOS_TEST_BURST_BEGIN', lines[0])
                self.assertIn('AVOS_TEST_BURST_END', lines[-1])
            for key, value in [('BURST_KEY', 'KEY; echo injected'), ('BURST_PAIRS', '101'),
                               ('BURST_GAP_MS', '-1')]:
                invalid = dict(env, **{key: value})
                r = subprocess.run(['bash', str(script)], env=invalid, capture_output=True, text=True)
                self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
                self.assertNotIn('adb ', r.stdout)

    def test_missing_fields_fail_closed(self):
        with self.assertRaises(KeyError):
            self.check(fixture().replace('frame_minus_heard=', 'other='))

    def test_frozen_presentation(self):
        import re
        text = re.sub(r'presented=\d+', 'presented=100', fixture())
        self.assertIn('presentation_stalled', self.check(text)['reason'])

    def test_pcm_observer_is_generation_scoped_and_still_detects_stalls(self):
        import re
        text = fixture().replace('source=playhead', 'source=pcm_observer generation=5')
        self.assertEqual(self.check(text)['healthy'], 1)
        frozen = re.sub(r'presented=\d+', 'presented=100', text)
        self.assertIn('presentation_stalled', self.check(frozen)['reason'])
        lines = []
        for line in text.splitlines():
            if 'audio_present_diag:' in line and float(line.split()[0]) >= 1001.5:
                line = line.replace('generation=5', 'generation=6')
                line = re.sub(r'presented=(\d+)', lambda m: 'presented=' + str(int(m[1]) - 60000), line)
            lines.append(line)
        self.assertNotIn('presentation_reset', self.check('\n'.join(lines))['reason'])
        sparse = '\n'.join(line for i, line in enumerate(text.splitlines())
                           if 'audio_present_diag:' not in line or i % 50 == 1)
        self.assertEqual(self.check(sparse)['verdict'], 'INSUFFICIENT_EVIDENCE')

    def test_missing_presentation(self):
        text = '\n'.join(x for x in fixture().splitlines() if 'audio_present_diag' not in x)
        self.assertIn('missing_presentation_progress', self.check(text)['reason'])

    def test_seek_underruns_and_drops(self):
        for bad, reason in [('AudioTrack underruns: total=9 delta=+9', 'audio_underruns'),
                            ('android_sync: late frame drop', 'late_frames'),
                            ('AUDIO_STARVED: waiting=300', 'audio_starved')]:
            result = self.check(fixture('seek') + '\n1003.021 ' + bad, 'seek')
            self.assertIn(reason, result['reason'])

    def test_silent_tail(self):
        result = self.check(fixture() + '\n1004.500 AVOS_TEST_POLL_2')
        self.assertIn('audio_write_gap', result['reason'])
        self.assertIn('stale_render_timing', result['reason'])

    def test_poll_marker_skew_does_not_reorder_playback(self):
        for mode in ('resume', 'seek'):
            # Last render is at 1003.010; the external poll arrives 1ms behind it.
            text = fixture(mode).replace('1003.020 AVOS_TEST_POLL_1', '1003.009 AVOS_TEST_POLL_1')
            result = self.check(text, mode)
            self.assertEqual(result['healthy'], 1, result)
            self.assertAlmostEqual(result['poll_marker_skew_ms'], 1)
            self.assertAlmostEqual(result['observation_ms'], 3010)
        # A poll may also arrive just ahead of the next player's timestamp.
        text = fixture().replace('1001.010 put_time_calc:',
                                 '1001.011 AVOS_TEST_POLL_2\n1001.010 put_time_calc:')
        self.assertEqual(self.check(text)['healthy'], 1)
        text = fixture().replace('1001.010 put_time_calc:',
                                 '1001.107 AVOS_TEST_POLL_2\n1001.010 put_time_calc:')
        result = self.check(text)
        self.assertEqual(result['healthy'], 1, result)
        self.assertAlmostEqual(result['poll_marker_skew_ms'], 97)

    def test_poll_skew_does_not_hide_clock_reversal_or_silent_tail(self):
        for text in (
                fixture() + '\n1003.000 AVOS_TEST_POLL_2',
                fixture().replace('1001.010 put_time_calc:', '1000.900 put_time_calc:'),
                fixture().replace('1001.010 put_time_calc:',
                                  '1001.011 AVOS_TEST_POLL_2\n1001.010 put_time_calc:')
                         .replace('1001.010 audiotrack_write:', '1001.009 audiotrack_write:'),
                fixture().replace('1003.020 AVOS_TEST_POLL_1', '1003.009 ordinary_message')):
            self.assertIn('log_time_reversed', self.check(text)['reason'])
        text = fixture().replace('1001.010 put_time_calc:',
                                 '1001.011 AVOS_TEST_POLL_2\n1001.010 put_time_calc:')
        result = self.check(text + '\n1005.000 AVOS_TEST_POLL_3')
        self.assertIn('audio_write_gap', result['reason'])
        self.assertIn('stale_presentation', result['reason'])

    def test_direct_renderer_logs_can_overtake_stdout_logs(self):
        rows = []
        for line in fixture(sequence=True).splitlines():
            stamp, message = line.split(' ', 1)
            tid = 200 if 'video_render_diag:' in message else 100
            if stamp == '1001.010' and 'video_render_diag:' in message:
                # Renderer records a slightly newer stamp before older stdout
                # messages reach logcat, as in the Pixel pause-burst capture.
                rows.insert(len(rows) - 4, f'1001.012 10 200 D avos_player: {message}')
            else:
                rows.append(f'{stamp} 10 {tid} D avos_player: {message}')
        text = '\n'.join(rows)
        result = self.check(text)
        self.assertEqual(result['healthy'], 1, result)
        self.assertAlmostEqual(result['thread_log_skew_ms'], 2)
        # Native presentation and render sequence checks remain independent
        # of permission to interleave delivery from different threads.
        self.assertIn('presentation_reset', self.check(text.replace(
            'presented=48000', 'presented=0'))['reason'])
        self.assertIn('missing_render_sequence_order', self.check(text.replace(
            'render_seq=26', 'render_seq=25'))['reason'])

    def test_thread_interleaving_does_not_hide_reversals(self):
        for suffix in (
                '1000.009 10 100 D avos_player: same thread reversed',
                '1000.009 untagged legacy record',
                '1000.009 10 D avos_player: missing tid'):
            text = ('1000.010 10 100 D avos_player: first\n'
                    '1000.012 10 200 D avos_player: renderer\n'
                    '1000.020 AVOS_TEST_POLL_1\n' + suffix)
            self.assertFalse(log_order(records(text))[0], suffix)
        # Compare against each producer's high-water mark, not just the
        # immediately preceding record from a different thread.
        text = ('1000.010 10 100 D avos_player: first\n'
                '1000.020 10 200 D avos_player: second\n'
                '1000.015 10 100 D avos_player: allowed\n'
                '1000.014 10 100 D avos_player: reversed')
        self.assertFalse(log_order(records(text))[0])

    def test_buffered_presentation_does_not_erase_write_gap(self):
        # Advancing buffered output alone cannot certify producer continuity.
        text = '\n'.join(line for line in fixture().splitlines() if not (
            'audiotrack_write:' in line and 1001 < float(line.split()[0]) < 1001.3))
        result = self.check(text)
        self.assertEqual(result['presentation'], 'advancing')
        self.assertIn('audio_write_gap', result['reason'])

    def test_short_deadline_judder(self):
        text = fixture().replace('deadline_ns=1001110000000', 'deadline_ns=1001170000000')
        self.assertNotEqual(text, fixture())
        self.assertIn('scheduled_judder', self.check(text)['reason'])

    def test_mode2_handoff_diagnostics_do_not_excuse_judder(self):
        from analyze_stress_recording import analyze_recording
        events = [
            'mode2_dynamic_clock_enter: epoch=35 heard=1241674 target=1241272 delay=904',
            'mode2_dynamic_clock_ready: epoch=35 heard=1241674 target=1241674 delay=889',
            'android_sync: mode2 dynamic clock transition active=1 hard_reanchor=1 smooth_entry=0 delta_us=176819',
            'mode2_dynamic_clock_fallback: epoch=35 heard=1242000 static=1242000',
        ]
        text = fixture().replace('deadline_ns=1001110000000', 'deadline_ns=1001286819000')
        text += '\n' + '\n'.join(events)
        report = analyze_recording(text)
        self.assertEqual([r['message'] for r in report['transition_records']], events)
        self.assertEqual(report['verdict'], 'ISSUES_OBSERVED')

    def test_internal_phase_error(self):
        self.assertIn('sustained_av_phase_error', self.check(fixture().replace('phase_ms=0', 'phase_ms=120'))['reason'])
        # Startup transients have a bounded exclusion window.
        rows = [x.replace('phase_ms=0', 'phase_ms=120') if float(x.split()[0]) < 1000.4 else x
                for x in fixture().splitlines()]
        self.assertEqual(self.check('\n'.join(rows))['healthy'], 1)

    def test_missing_render_evidence(self):
        text = '\n'.join(x for x in fixture().splitlines() if 'video_render_diag' not in x)
        self.assertIn('missing_render_timing', self.check(text)['reason'])
        result = self.check(text, REQUIRE_RENDER_TIMING='0')
        self.assertEqual(result['healthy'], 1)
        self.assertEqual(result['timing'], 'missing')
        self.assertEqual(result['physical_lipsync'], 'unmeasured')

    def test_speed_window_at_eof_is_finalized(self):
        text = fixture('speed', 6, 1.5)
        windows = high_windows(records(text), config({}))
        self.assertEqual(len(windows), 1)
        self.assertAlmostEqual(windows[0][1] - windows[0][0], 6000)
        result = self.check(text, 'speed')
        self.assertEqual(result['reason'], 'speed_not_restored')
        self.assertEqual(self.check(text + '\n1006.030 put_time_calc: speed=1.000 allow_reanchor=0', 'speed')['healthy'], 1)

    def test_frozen_speed_media(self):
        import re
        text = re.sub(r'(frame|audio)=\d+', r'\1=0', fixture('speed', 6, 1.5))
        text += '\n1006.030 put_time_calc: speed=1.000 allow_reanchor=0'
        self.assertIn('high_window_no_progress', self.check(text, 'speed')['reason'])

    def test_last_window_cannot_borrow_earlier_metrics(self):
        first = fixture('speed', 5, 1.5)
        last = fixture('speed', 8, 1.5, start=1006)
        lines = []
        for line in last.splitlines():
            t = float(line.split()[0])
            if 'put_time_calc:' in line or round((t - 1006.01) * 1000) % 400 == 0:
                lines.append(line)
        text = first + '\n1005.030 put_time_calc: speed=1.000\n' + '\n'.join(lines)
        result = self.check(text, 'speed')
        self.assertIn('audio_write_gap', result['reason'])
        self.assertGreaterEqual(result['hi_max_write_gap'], 399)

    def test_lost_logs(self):
        self.assertIn('log_loss', self.check(fixture()+'\n1003.030 chatty dropped 10 lines')['reason'])

    def test_sparse_advancing_presentation_is_not_continuous_evidence(self):
        rows = fixture().splitlines()
        present = [i for i, row in enumerate(rows) if 'audio_present_diag:' in row]
        text = '\n'.join(row for i, row in enumerate(rows) if
                         'audio_present_diag:' not in row or i in (present[0], present[-1]))
        result = self.check(text)
        self.assertEqual(result['healthy'], 0)
        self.assertIn('missing_presentation_observations', result['reason'])
        self.assertGreater(result['presentation_max_gap_ms'], 500)
        self.assertEqual(result['verdict'], 'INSUFFICIENT_EVIDENCE')

    def test_throttled_legacy_playhead_is_evidence_gap_not_stall(self):
        rows = []
        for row in fixture(seconds=8).splitlines():
            if 'audio_present_diag:' in row:
                if round((float(row.split()[0]) - 1000.01) * 1000) not in (0, 2000, 4000, 6000):
                    continue
                row = row.replace('audio_present_diag:', 'playhead_delay:')
            rows.append(row)
        text = '\n'.join(rows)
        result = self.check(text)
        self.assertEqual(result['presentation'], 'advancing')
        self.assertEqual(result['healthy'], 0)
        self.assertEqual(result['verdict'], 'INSUFFICIENT_EVIDENCE')
        self.assertNotIn('presentation_stalled', result['reason'])
        # Independent playback faults must outrank the sparse sampling gap.
        self.assertEqual(self.check(text + '\n1008.020 AUDIO_STARVED: waiting=100')['verdict'], 'FAIL')
        import re
        frozen = re.sub(r'presented=\d+', 'presented=0', text)
        self.assertIn('presentation_stalled', self.check(frozen)['reason'])
        self.assertEqual(self.check(frozen)['verdict'], 'FAIL')

    def test_offline_runtime_errors_and_capture_loss(self):
        from analyze_stress_recording import analyze_recording
        for message, reason, verdict in (
            ('FATAL EXCEPTION: playback', 'runtime_error', 'ISSUES_OBSERVED'),
            ('compressed write failed', 'compressed_write_failure', 'ISSUES_OBSERVED'),
            ('chatty dropped 300 lines', 'log_loss', 'INSUFFICIENT_EVIDENCE'),
            ('Unexpected EOF', 'log_loss', 'INSUFFICIENT_EVIDENCE')):
            text = fixture() + '\n1003.021 ' + message
            result = analyze_recording(text, config({}))
            self.assertEqual(result['verdict'], verdict)
            self.assertTrue(result['fault_lines'][reason])
            self.assertIn(reason, self.check(text)['reason'])

    def test_offline_and_live_event_budgets_agree(self):
        from analyze_stress_recording import analyze_recording
        for message, budget in (
            ('late frame drop', 'LATE_DROP_MAX'), ('AUDIO_STARVED: waiting=100', 'STARVED_MAX'),
            ('AudioTrack underruns: total=1 delta=1', 'UNDERRUN_MAX')):
            text = fixture() + '\n1003.021 ' + message
            for allowed in ('0', '1'):
                settings = config({budget: allowed})
                live = analyze(text, 'resume', settings)
                offline = analyze_recording(text, settings)
                self.assertEqual(live['healthy'], int(allowed))
                self.assertEqual(offline['verdict'], 'NO_ISSUES_OBSERVED' if allowed == '1' else 'ISSUES_OBSERVED')

    def test_resume_boundary_is_not_ordinary_judder(self):
        from analyze_stress_recording import analyze_recording
        rows = fixture().splitlines()
        import re
        boundary = False
        for i, row in enumerate(rows):
            if 'video_render_diag:' not in row or float(row.split()[0]) < 1001:
                continue
            ns = int(re.search(r'deadline_ns=(\d+)', row)[1])
            rows[i] = row.replace(f'deadline_ns={ns}', f'deadline_ns={ns+85000000}')
            if not boundary:
                stamp = row.split()[0]
                rows[i] = (f'{stamp} android_sync: mode1 resume applies paused correction=-38000us remaining=0us\n'
                           f'{stamp} android_sync: resume shift offset by 123ms -> 123456\n' + rows[i])
                boundary = True
        result = analyze_recording('\n'.join(rows), config({}))
        self.assertEqual(result['verdict'], 'NO_ISSUES_OBSERVED')
        finding = result['segments'][0]['findings'][0]
        self.assertEqual(finding['kind'], 'resume_boundary_adjustment')
        self.assertEqual(finding['accounted_shift_ms'], 85)
        self.assertEqual(finding['residual_error_ms'], 0)
        self.assertEqual(self.check('\n'.join(rows))['cadence_bad'], 0)
        pcm = '\n'.join(rows).replace('mode1 resume applies', 'PCM resume applies')
        self.assertEqual(self.check(pcm)['cadence_bad'], 0)
        pcm_review = analyze_recording(pcm, config({}))
        self.assertEqual(pcm_review['segments'][0]['findings'][0]['accounted_shift_ms'], 85)
        # Removing the boundary leaves the same unaccounted deadline jump.
        plain = '\n'.join(r for r in '\n'.join(rows).splitlines() if 'resume shift offset' not in r)
        self.assertIn('scheduled_judder', self.check(plain)['reason'])
        wrong_shift = '\n'.join(rows).replace('offset by 123ms', 'offset by 23ms')
        self.assertIn('scheduled_judder', self.check(wrong_shift)['reason'])

    def test_delayed_render_log_uses_embedded_resume_boundary(self):
        import re
        from analyze_stress_recording import analyze_recording
        for path in ('PCM', 'mode1'):
            rows = fixture().splitlines()
            delayed = False
            for i, row in enumerate(rows):
                if 'video_render_diag:' not in row or float(row.split()[0]) < 1001:
                    continue
                if not delayed:
                    # Submission precedes resume by 3ms, but logging follows it.
                    stamp = float(row.split()[0])
                    resume_ms = round(stamp * 1000) + 3
                    rows[i] = (f'{stamp:.3f} android_sync: pause start at {resume_ms-82}\n'
                               f'{stamp:.3f} WALLCLOCK_RESET: by pause resume\n'
                               f'{stamp:.3f} android_sync: resume state offset=2000000000 pending=0\n'
                               f'{stamp:.3f} android_sync: {path} resume applies paused correction=-44408us remaining=0us\n'
                               f'{stamp:.3f} android_sync: resume shift offset by 82ms -> 2037592077\n' + row)
                    delayed = True
                else:
                    ns = int(re.search(r'deadline_ns=(\d+)', row)[1])
                    rows[i] = row.replace(f'deadline_ns={ns}', f'deadline_ns={ns+33592077}')
            text = '\n'.join(rows)
            self.assertEqual(self.check(text)['cadence_bad'], 0)
            review = analyze_recording(text, config({}))
            self.assertEqual(review['verdict'], 'NO_ISSUES_OBSERVED')
            finding = review['segments'][1]['findings'][0]
            self.assertEqual(finding['kind'], 'resume_boundary_adjustment')
            self.assertAlmostEqual(finding['accounted_shift_ms'], 37.592077)
            self.assertAlmostEqual(finding['residual_error_ms'], 4)
            # An unrelated jump or missing clock evidence must still fail.
            self.assertIn('scheduled_judder', self.check(text.replace('offset=2000000000', 'offset=2100000000'))['reason'])
            missing = '\n'.join(r for r in text.splitlines() if 'pause start at' not in r)
            self.assertIn('scheduled_judder', self.check(missing)['reason'])

    def test_resume_boundaries_do_not_cross_seek_or_open(self):
        from analyze_stress import ResumeBoundary
        for reset in ('stream_open:', 'stream_stop:', 'VIDEO_SEEK_TARGET_READY: epoch=2'):
            resume = ResumeBoundary()
            for line in ('android_sync: pause start at 1000',
                         'android_sync: resume shift offset by 100ms -> 500', reset):
                resume.annotate(line, {})
            fields = {'submit_ns': '1050000000'}
            resume.annotate('video_render_diag:', fields)
            self.assertEqual(fields['_resume_boundary'], 1)
            self.assertNotIn('_resume_boundary_source', fields)

    def test_renderer_can_overtake_buffered_resume_message(self):
        from analyze_stress_recording import analyze_recording
        for path in ('PCM', 'mode1'):
            rows = ['999.000 android_sync: pause start at 998000']
            inserted = False
            for row in fixture(sequence=True).splitlines():
                rows.append(row)
                if 'video_render_diag:' in row and not inserted:
                    # The first post-resume submission arrives before the
                    # stdout thread delivers the shift that already affected it.
                    rows += ['1000.010 android_sync: resume state offset=2000000000 pending=0',
                             f'1000.010 android_sync: {path} resume applies paused correction=-40000us remaining=0us',
                             '1000.010 android_sync: resume shift offset by 2000ms -> 3960000000']
                    inserted = True
            text = '\n'.join(rows)
            render = [r[2] for r in records(text) if 'video_render_diag:' in r[1]]
            self.assertEqual(render[0]['_resume_boundary'], render[1]['_resume_boundary'])
            self.assertEqual(render[0]['_resume_adjustment_ms'], 1960)
            self.assertEqual(self.check(text)['healthy'], 1)
            self.assertEqual(analyze_recording(text, config({}))['verdict'], 'NO_ISSUES_OBSERVED')
            # Neither unknown boundaries nor genuine cadence errors get waived.
            missing = '\n'.join(row for row in rows if 'pause start at' not in row)
            self.assertIn('scheduled_judder', self.check(missing)['reason'])
            jumped = text.replace('deadline_ns=1000150000000', 'deadline_ns=1000170000000')
            self.assertNotEqual(jumped, text)
            self.assertIn('scheduled_judder', self.check(jumped)['reason'])

    def test_pending_resume_records_do_not_cross_reset(self):
        from analyze_stress import ResumeBoundary
        for reset in ('stream_open:', 'stream_stop:', 'VIDEO_SEEK_TARGET_READY: epoch=2'):
            resume = ResumeBoundary()
            resume.annotate('android_sync: pause start at 1000', {})
            first = {'submit_ns': '1200000000'}
            resume.annotate('video_render_diag:', first)
            resume.annotate(reset, {})
            resume.annotate('android_sync: resume shift offset by 100ms -> 500', {})
            self.assertEqual(first['_resume_boundary'], 0)
            self.assertNotIn('_resume_boundary_source', first)

    def test_recording_baseline_must_be_stable_and_not_borrowed(self):
        from analyze_stress_recording import analyze_recording
        text = fixture().replace('phase_ms=0', 'phase_ms=-303')
        text = 'libavos_set_passthrough: mode=1\n' + '\n'.join(
            row.replace('phase_ms=-303', 'phase_ms=-100') if
            'video_render_diag:' in row and 1000.8 < float(row.split()[0]) < 1001 else row
            for row in text.splitlines())
        result = analyze_recording(text + '\n' + fixture(start=1005), config({}))
        self.assertEqual(result['verdict'], 'INSUFFICIENT_EVIDENCE')
        self.assertEqual(result['phase_reference'], 'unavailable')

    def test_context_changes_do_not_inherit_phase_reference(self):
        from analyze_stress_recording import analyze_recording
        text = ('libavos_set_passthrough: mode=1\n' + fixture().replace('phase_ms=0', 'phase_ms=-303') +
                '\nlibavos_set_passthrough: mode=2\n' + fixture(start=1005))
        result = analyze_recording(text, config({}))
        self.assertEqual(result['verdict'], 'INSUFFICIENT_EVIDENCE')
        self.assertIn('playback_context_changed', result['evidence_gaps'])
        self.assertEqual(result['segments'][-1]['phase_reference'], 'unavailable')
        self.assertNotIn('sustained_av_phase_error', result['segments'][-1]['issues'])

    def test_partial_writes_are_distinguished_without_claiming_completion(self):
        from analyze_stress import write_metrics
        result = write_metrics('audiotrack_write: wrote 16 out of 24576 bytes\n'
                               'compressed short write 16/24576, continuing unit at 16/24576\n'
                               'audiotrack_write: wrote 24560 out of 24560 bytes')
        self.assertEqual(result['partial_writes'], 1)
        self.assertEqual(result['full_writes'], 1)
        self.assertEqual(result['accepted_bytes'], 24576)
        self.assertEqual(result['compressed_continuations'], 1)
        self.assertEqual(result['compressed_unit_completion'], 'not_proven')

    def test_recovery_summary_retains_transients_and_incomplete_windows(self):
        from analyze_stress_recording import analyze_recording
        text = '\n'.join(row.replace('phase_ms=0', 'phase_ms=120') if
                         float(row.split()[0]) < 1000.85 else row for row in fixture().splitlines())
        result = analyze_recording(text, config({}))
        recovery = result['segments'][0]['recovery']
        self.assertEqual(recovery['initial_deviation_ms'], 120)
        self.assertEqual(recovery['settled_phase_median_ms'], 0)
        self.assertAlmostEqual(recovery['recovery_ms'], 840)
        self.assertIn('sustained_av_phase_error', result['segments'][0]['issues'])
        short = '\n'.join(row for row in text.splitlines() if float(row.split()[0]) < 1000.3)
        recovery = analyze_recording(short, config({}))['segments'][0]['recovery']
        self.assertEqual(recovery['status'], 'not_observed_before_segment_end')
        self.assertIsNone(recovery['recovery_ms'])
        later_drift = '\n'.join(row.replace('phase_ms=0', 'phase_ms=120') if
                                float(row.split()[0]) > 1002 else row for row in fixture().splitlines())
        result = analyze_recording(later_drift, config({}))
        recovery = result['segments'][0]['recovery']
        self.assertEqual(recovery['recovery_ms'], 0)
        self.assertEqual(recovery['status'], 'not_settled_at_segment_end')
        self.assertIsNone(recovery['final_stable_run_ms'])
        self.assertIn('sustained_av_phase_error', result['segments'][0]['issues'])

    def test_bad_config(self):
        for value in ('-1', 'nan', 'inf'):
            with self.assertRaises(ValueError):
                config({'AV_PHASE_MAX_MS': value})

    def test_mode1_phase_reference(self):
        text = fixture().replace('phase_ms=0', 'phase_ms=-303')
        self.assertEqual(self.check(text, EXPECTED_PHASE_MS='-303')['healthy'], 1)
        self.assertIn('sustained_av_phase_error', self.check(text)['reason'])

    def test_mode1_observer_progress_and_underruns(self):
        text = '\n'.join(x for x in fixture().splitlines() if 'audio_present_diag' not in x)
        rows = text.splitlines()
        for i in range(6):
            rows.append(f'{1000.1+i*.5:.3f} mode1_iec_occupancy_shadow: epoch=8 generation=4 src=1 age=50 ts_age=50 presented={i*96000} underruns=7')
        rows.sort(key=lambda x: float(x.split()[0]))
        good = '\n'.join(rows)
        self.assertEqual(self.check(good)['healthy'], 1)
        self.assertEqual(self.check(good)['underruns'], 0)
        self.assertEqual(self.check(good)['underrun_monitor'], 'observer')
        bad = good.replace('presented=480000 underruns=7', 'presented=480000 underruns=8')
        self.assertIn('audio_underruns', self.check(bad)['reason'])

    def test_observer_does_not_join_generations(self):
        text = '\n'.join(x for x in fixture().splitlines() if 'audio_present_diag' not in x)
        rows = text.splitlines()
        for i in range(6):
            rows.append(f'{1000.1+i*.5:.3f} mode2_occupancy_shadow: epoch=8 generation={i} src=1 age=50 ts_age=50 presented={i*96000} underruns=0')
        rows.sort(key=lambda x: float(x.split()[0]))
        self.assertIn('missing_presentation_progress', self.check('\n'.join(rows))['reason'])

    def test_recording_brief_and_preview_exclusion(self):
        from analyze_stress_recording import analyze_recording
        text = '\n'.join('D/avos_player( 1): '+x.split(' ',1)[1] for x in fixture().splitlines())
        result = analyze_recording(text, config({}))
        self.assertEqual(result['verdict'], 'NO_ISSUES_OBSERVED')
        self.assertEqual(result['audio_write_gaps'], 'not_evaluated')
        self.assertEqual(result['physical_lipsync'], 'unmeasured')
        result = analyze_recording('D/avos_player( 1): _stream_play_n_frames(n=10)\n'+
                                   '\n'.join(x for x in text.splitlines() if 'WALLCLOCK_RESET' not in x), config({}))
        self.assertEqual(result['signals']['render_frames'], 0)
        self.assertGreater(result['signals']['excluded_pause_preview_frames'], 0)
        self.assertEqual(result['verdict'], 'INSUFFICIENT_EVIDENCE')

    def test_recording_finds_judder_and_keeps_lines(self):
        from analyze_stress_recording import analyze_recording
        text = fixture().replace('deadline_ns=1001110000000', 'deadline_ns=1001195000000')
        result = analyze_recording(text, config({}))
        self.assertIn('scheduled_judder', result['segments'][0]['issues'])
        self.assertGreater(result['segments'][0]['first_line'], 0)
        self.assertEqual(result['verdict'], 'ISSUES_OBSERVED')

    def test_recording_requires_fresh_anchors(self):
        from analyze_stress_recording import analyze_recording
        for text in (fixture().replace('anchor_age_ms=10', 'anchor_age_ms=500'),
                     '\n'.join(x.replace('anchor_age_ms=10', 'anchor_age_ms=500')
                               if float(x.split()[0]) > 1002 else x for x in fixture().splitlines())):
            result = analyze_recording(text, config({}))
            self.assertEqual(result['verdict'], 'INSUFFICIENT_EVIDENCE')
            self.assertIn('stale_audio_anchor', self.check(text)['reason'])

    def test_review_findings_identify_exact_lines(self):
        from analyze_stress_recording import analyze_recording, markdown_report
        text = fixture().replace('deadline_ns=1001110000000', 'deadline_ns=1001195000000')
        text = text.replace('phase_ms=0', 'phase_ms=120')
        report = analyze_recording(text, config({}))
        findings = report['segments'][0]['findings']
        cadence = next(f for f in findings if f['kind'] == 'scheduled_judder')
        self.assertAlmostEqual(cadence['interval_ms'], 125)
        self.assertIn('deadline_ns=1001195000000', text.splitlines()[cadence['last_line'] - 1])
        phase = [f for f in findings if f['kind'] == 'sustained_av_phase_error']
        self.assertEqual(len(phase), 1)
        self.assertGreater(phase[0]['duration_ms'], 2000)
        output = markdown_report(report, 'fixture.log')
        self.assertIn('scheduled_judder', output)
        self.assertIn('Physical lipsync and displayed cadence: **unmeasured**', output)

    def test_review_cli_replays_config(self):
        import json
        from pathlib import Path
        import subprocess
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp)/'recording.log'
            settings = Path(tmp)/'analyzer-config.json'
            log.write_text(fixture().replace('phase_ms=0', 'phase_ms=-303'))
            settings.write_text(json.dumps(config({'EXPECTED_PHASE_MS': '-303'})))
            cmd = ['python3', str(Path(__file__).with_name('analyze_stress_recording.py')),
                   str(log), '--config', str(settings), '--format', 'markdown']
            result = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('NO_ISSUES_OBSERVED', result.stdout)
            self.assertIn('explicit (-303 ms)', result.stdout)
            settings.write_text('[]')
            result = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('configuration must be a JSON object', result.stderr)

    def test_recording_reports_faults_without_render_coverage(self):
        from analyze_stress_recording import analyze_recording
        result = analyze_recording('D/avos_player(1): late frame drop', config({}))
        self.assertEqual(result['verdict'], 'ISSUES_OBSERVED')
        self.assertEqual(result['timing_coverage'], 'insufficient')
        self.assertEqual(result['signals']['late_drops'], 1)

    def test_speed_rejects_invalid_step_delay_before_adb(self):
        import subprocess
        from pathlib import Path
        script = Path(__file__).with_name('stress_speed_validate.sh')
        for value in ('.', '1.2.3', '-1', 'nan'):
            result = subprocess.run(['bash', str(script), '1', '8', value], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn('invalid step delay', result.stderr)

    def test_campaign_dry_run_never_contacts_adb(self):
        import os
        from pathlib import Path
        import subprocess
        import tempfile
        script = Path(__file__).with_name('stress_campaign.sh').resolve()
        with tempfile.TemporaryDirectory() as tmp:
            adb = Path(tmp)/'adb'
            adb.write_text('#!/bin/sh\necho UNEXPECTED_ADB >&2\nexit 99\n')
            adb.chmod(0o755)
            output = Path(tmp)/'output'
            env = dict(os.environ, PATH=tmp+os.pathsep+os.environ['PATH'],
                       DRY_RUN='1', SPEED_CYCLES='1', OUTPUT_DIR=str(output))
            r = subprocess.run(['bash', str(script)], env=env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout+r.stderr)
            self.assertIn('speed phase:', r.stdout)
            self.assertNotIn('UNEXPECTED_ADB', r.stderr)
            self.assertFalse(output.exists())

    def test_campaign_preserves_fault_priority_over_sparse_phases(self):
        from pathlib import Path
        import subprocess
        import tempfile
        script = Path(__file__).with_name('stress_campaign.sh').read_text()
        run_phase = script[script.index('run_phase()'):script.index('\naggregate_phase()')]
        with tempfile.TemporaryDirectory() as tmp:
            setup = 'CAMPAIGN_FAILED=0\nCAMPAIGN_RC=0\nSESSION_LOG="$1/session.log"\n'
            calls = '''
run_phase first "$1/first" bash -c 'exit 3'
test "$CAMPAIGN_RC" = 3 || exit 91
run_phase second "$1/second" bash -c 'exit 1'
test "$CAMPAIGN_RC" = 1 || exit 92
run_phase third "$1/third" bash -c 'exit 3'
test "$CAMPAIGN_RC" = 1 || exit 93
run_phase fourth "$1/fourth" bash -c 'exit 2'
test "$CAMPAIGN_RC" = 2 || exit 94
run_phase fifth "$1/fifth" bash -c 'exit 3'
test "$CAMPAIGN_RC" = 2 || exit 95
'''
            result = subprocess.run(['bash', '-c', setup + run_phase + calls, 'test', tmp],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_campaign_report_wrapper_keeps_failure_and_original_reference(self):
        import contextlib
        import io
        import json
        import os
        from pathlib import Path
        import tempfile
        from types import SimpleNamespace
        from unittest.mock import patch
        from run_stress_campaign import main
        with tempfile.TemporaryDirectory() as tmp:
            for campaign_rc, phase, expected in ((0, -303, 0), (1, -303, 1),
                                                (0, 0, 1), (2, -303, 2), (3, -303, 1), (3, 0, 1)):
                output = Path(tmp) / f'run-{campaign_rc}-{phase}'

                def campaign(command, env):
                    self.assertEqual(command[-1], '2')
                    self.assertEqual(env['BURST_PAIRS'], '10')
                    self.assertEqual(env['SESSION_LOGCAT'], '1')
                    target = Path(env['OUTPUT_DIR'])
                    (target / 'resume-1').mkdir()
                    (target / 'resume-1' / 'analyzer-config.json').write_text(
                        json.dumps(config({'EXPECTED_PHASE_MS': -303})))
                    (target / 'logcat-session.log').write_text(
                        fixture().replace('phase_ms=0', f'phase_ms={phase}'))
                    return SimpleNamespace(returncode=campaign_rc)

                with patch.dict(os.environ, {'BURST_PAIRS': '10'}, clear=True), \
                        patch('sys.argv', ['run_stress_campaign.py', '2', '--output-dir', str(output)]), \
                        patch('run_stress_campaign.subprocess.run', side_effect=campaign), \
                        contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(main(), expected)
                summary = json.loads((output / 'run-report.json').read_text())
                self.assertEqual(summary['campaign_exit_code'], campaign_rc)
                if campaign_rc == 3:
                    self.assertEqual(summary['verdict'], 'INSUFFICIENT_EVIDENCE' if phase == -303 else 'FAIL')
                report = json.loads((output / 'recording-review.json').read_text())
                self.assertEqual(report['expected_phase_ms'], -303)
                self.assertIn(report['verdict'], (output / 'recording-review.md').read_text())

    def test_campaign_report_missing_evidence_never_passes(self):
        import contextlib
        import io
        import json
        from pathlib import Path
        import tempfile
        from run_stress_campaign import generate_reports
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(generate_reports(output, 0), 2)
            summary = json.loads((output / 'run-report.json').read_text())
            self.assertEqual(summary['recording_verdict'], 'UNAVAILABLE')
            self.assertIn('unavailable', (output / 'recording-review.md').read_text())

    def test_campaign_report_wrapper_dry_run_and_preserves_previous_output(self):
        import os
        from pathlib import Path
        import subprocess
        import tempfile
        script = Path(__file__).with_name('run_stress_campaign.py').resolve()
        with tempfile.TemporaryDirectory() as tmp:
            adb = Path(tmp) / 'adb'
            adb.write_text('#!/bin/sh\necho UNEXPECTED_ADB >&2\nexit 99\n')
            adb.chmod(0o755)
            output = Path(tmp) / 'output'
            env = dict(os.environ, PATH=tmp+os.pathsep+os.environ['PATH'],
                       DRY_RUN='0', SESSION_LOGCAT='1', BURST_PAIRS='10')
            cmd = ['python3', str(script), '--output-dir', str(output)]
            r = subprocess.run(cmd + ['--dry-run'], env=env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('Resume burst pairs: 10', r.stdout)
            self.assertNotIn('UNEXPECTED_ADB', r.stderr)
            self.assertFalse(output.exists())
            output.mkdir()
            previous = output / 'logcat-session.log'
            previous.write_text('previous evidence')
            r = subprocess.run(cmd, env=env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
            self.assertIn('previous evidence is preserved', r.stderr)
            self.assertNotIn('UNEXPECTED_ADB', r.stderr)
            self.assertEqual(previous.read_text(), 'previous evidence')


if __name__ == '__main__':
    unittest.main()

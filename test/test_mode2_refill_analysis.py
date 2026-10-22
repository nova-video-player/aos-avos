"""Calibrated seeks may fill a compressed queue before the first DAC sample."""
import unittest

from analyze_stress import analyze, config
from test_analyze_stress import fixture


def refill_fixture():
    lines = []
    for line in fixture('seek', seconds=5).splitlines():
        t = float(line.split()[0])
        if 'audio_present_diag:' in line:
            continue
        if 'audiotrack_write:' in line:
            if 1000.09 < t < 1000.45:
                continue
            line += ' (format=4747, passthrough=2)'
            if t == 1000.45:
                line = line.replace('7680 out of 7680', '1536 out of 1536')
        lines.append(line)
    lines += [
        '1000.002 mode2_seek_calibration: seed correction=196 reused=1 generation=0',
        '1000.020 android_sync: mode2 seek qualification epoch=1 state=0 waited_ms=0 anchored=0',
        '1000.450 compressed_write_wait: epoch=1 fmt=4747 last_progress_ms=1000 '
        'start_ms=1010 end_ms=1360 blocked_ms=350 retries=35 interrupted=0 '
        'accepted=1024 unit_bytes=2560 written=1536 complete=1',
    ]
    for i in range(11):
        t = 1000.001 if i == 0 else 1000.01 + i * .5
        lines.append(
            f'{t:.3f} mode2_occupancy_shadow: epoch=22 generation=8 pt=2 '
            f'fmt=4747 src={int(i > 0)} framing={2 if i else 0} '
            f'logical={48000*i} encoded={80000*i} presented={24000*i} '
            'buffer=16384 age=20 ts_age=20 trusted=1 counter_advancing=1 underruns=0')
    return '\n'.join(sorted(lines, key=lambda line: float(line.split()[0])))


class Mode2RefillAnalysisTests(unittest.TestCase):
    def check(self, text, mode='seek', **overrides):
        return analyze(text, mode, config(overrides))

    def test_calibrated_startup_refill(self):
        result = self.check(refill_fixture())
        self.assertEqual(result['verdict'], 'PASS', result)
        self.assertEqual(result['seek_refill_write_gaps'], 1)
        self.assertEqual(result['seek_qualification_ms'], 0)
        self.assertEqual(result['backpressure_write_gaps'], 1)
        self.assertEqual(result['max_write_gap'], 360)

    def partial_recovery_fixture(self):
        text = refill_fixture().replace('written=1536 complete=1', 'written=1024 complete=0')
        text = text.replace('1000.450 audiotrack_write: wrote 1536 out of 1536',
                            '1000.450 audiotrack_write: wrote 1024 out of 1536')
        text += ('\n1000.450 stream_audio: compressed short write 1024/1536 fmt=4747, '
                 'continuing unit at 2048/2560\n'
                 '1000.451 audiotrack_write: wrote 512 out of 512 bytes (format=4747, passthrough=2)')
        return '\n'.join(sorted(text.splitlines(), key=lambda l: float(l.split()[0])))

    def test_recovered_wait_can_complete_in_following_writes(self):
        text = self.partial_recovery_fixture()
        for candidate in (text, text.replace(
                '1000.451 audiotrack_write: wrote 512 out of 512 bytes (format=4747, passthrough=2)',
                '1000.451 audiotrack_write: wrote 256 out of 512 bytes (format=4747, passthrough=2)\n'
                '1000.451 stream_audio: compressed short write 256/512 fmt=4747, continuing unit at 2304/2560\n'
                '1000.452 audiotrack_write: wrote 256 out of 256 bytes (format=4747, passthrough=2)')):
            result = self.check(candidate)
            self.assertEqual(result['verdict'], 'PASS', result)
            self.assertEqual(result['seek_refill_write_gaps'], 1)
            self.assertEqual(result['backpressure_write_gaps'], 1)
            self.assertEqual(result['incomplete_backpressure_write_gaps'], 0)
            # Raw diagnostic says only that the first recovered write was partial.
            self.assertEqual(result['compressed_wait_completed_units'], 0)
        result = self.check(text, SEEK_STARTUP_MAX_MS=450)
        self.assertEqual(result['seek_refill_write_gaps'], 0)
        self.assertIn('audio_write_gap', result['reason'])

    def test_partial_recovery_needs_exact_prompt_uninterrupted_continuation(self):
        text = self.partial_recovery_fixture()
        for old, new in (
                ('1000.451 audiotrack_write:', '1000.501 audiotrack_write:'),
                ('wrote 512 out of 512', 'wrote 512 out of 2560'),
                ('wrote 512 out of 512', 'wrote 0 out of 512'),
                ('wrote 512 out of 512', 'wrote -1 out of 512'),
                ('continuing unit at 2048/2560', 'continuing unit at 2048/4096'),
                ('short write 1024/1536 fmt=4747', 'short write 1024/1536 fmt=2000'),
                ('1000.451 audiotrack_write:', '1000.451 999 123 D avos_player: audiotrack_write:')):
            result = self.check(text.replace(old, new))
            self.assertEqual(result['seek_refill_write_gaps'], 0, (old, result))
            self.assertIn('audio_write_gap', result['reason'])
        for marker in ('continuing unit at', '1000.451 audiotrack_write:'):
            missing = '\n'.join(l for l in text.splitlines() if marker not in l)
            self.assertIn('audio_write_gap', self.check(missing)['reason'])
        for event in ('android_sync: pause start', 'stream_stop:',
                      'audiotrack_set_output_params: resolved'):
            changed = text + '\n1000.4505 ' + event
            changed = '\n'.join(sorted(changed.splitlines(), key=lambda l: float(l.split()[0])))
            self.assertIn('audio_write_gap', self.check(changed)['reason'])

    def test_requires_calibration_new_queue_and_bounded_startup(self):
        text = refill_fixture()
        for old, new in (
                ('reused=1', 'reused=0'),
                ('logical=0 encoded=0 presented=0', 'logical=1 encoded=1 presented=1'),
                ('buffer=16384', 'buffer=9999999'),
                ('buffer=16384', 'buffer=bad'),
                ('passthrough=2', 'passthrough=0'),
                ('fmt=4747', 'fmt=2000'),
                ('generation=8', 'generation=9',)):
            # Change only the queue origin for generation mismatch.
            changed = text.replace(old, new, 1) if old == 'generation=8' else text.replace(old, new)
            with self.subTest(old=old):
                result = self.check(changed)
                self.assertEqual(result['seek_refill_write_gaps'], 0)
                self.assertIn('audio_write_gap', result['reason'])
        self.assertIn('audio_write_gap', self.check(text, SEEK_STARTUP_MAX_MS=400)['reason'])

    def test_first_snapshot_after_filling_started(self):
        text = refill_fixture().replace('1000.001 mode2_occupancy_shadow:',
                                       '1000.075 mode2_occupancy_shadow:')
        text = text.replace('logical=0 encoded=0 presented=0',
                            'logical=9216 encoded=15360 presented=0')
        # Two complete writes at .010 and .050 precede the first snapshot.
        text = '\n'.join(sorted(text.splitlines(), key=lambda l: float(l.split()[0])))
        result = self.check(text)
        self.assertEqual(result['verdict'], 'PASS', result)
        self.assertEqual(result['seek_refill_write_gaps'], 1)
        for old, new in (('encoded=15360', 'encoded=15361'),
                         ('encoded=15360', 'encoded=7680'),
                         ('logical=9216', 'logical=0'),
                         ('presented=0', 'presented=1')):
            result = self.check(text.replace(old, new, 1))
            self.assertEqual(result['seek_refill_write_gaps'], 0)
            self.assertIn('audio_write_gap', result['reason'])
        missing = '\n'.join(l for l in text.splitlines()
                            if not l.startswith('1000.010 audiotrack_write:'))
        self.assertIn('audio_write_gap', self.check(missing)['reason'])

    def test_snapshot_excludes_pending_partial_unit(self):
        text = refill_fixture().replace('1000.001 mode2_occupancy_shadow:',
                                       '1000.075 mode2_occupancy_shadow:')
        text = text.replace('logical=0 encoded=0 presented=0',
                            'logical=4608 encoded=7680 presented=0')
        text = text.replace('1000.050 audiotrack_write: wrote 7680 out of 7680',
                            '1000.050 audiotrack_write: wrote 1536 out of 7680')
        text = text.replace('1000.090 audiotrack_write: wrote 7680 out of 7680',
                            '1000.090 audiotrack_write: wrote 6144 out of 6144')
        text = text.replace('buffer=16384', 'buffer=8192')
        text = '\n'.join(sorted(text.splitlines(), key=lambda l: float(l.split()[0])))
        result = self.check(text)
        self.assertEqual(result['verdict'], 'PASS', result)
        self.assertEqual(result['seek_refill_write_gaps'], 1)
        # Counting the partial prefix as already ledger-committed is wrong.
        self.assertIn('audio_write_gap', self.check(text.replace('encoded=7680', 'encoded=9216'))['reason'])
        # A second fragment must request exactly the first fragment's remainder.
        split = text.replace('1000.050 audiotrack_write: wrote 1536 out of 7680',
                             '1000.050 audiotrack_write: wrote 1536 out of 7680\n'
                             '1000.060 audiotrack_write: wrote 512 out of 7680')
        self.assertIn('audio_write_gap', self.check(split)['reason'])

    def test_cached_snapshot_matches_only_totals_allowed_by_its_age(self):
        text = refill_fixture().replace('1000.001 mode2_occupancy_shadow:',
                                       '1000.075 mode2_occupancy_shadow:')
        text = text.replace('logical=0 encoded=0 presented=0',
                            'logical=4608 encoded=7680 presented=0')
        # At .075 the observer's 30ms-old snapshot predates the completed
        # write at .050. Its total must match the exact earlier unit boundary.
        text = text.replace('age=20 ts_age=20', 'age=30 ts_age=20', 1)
        text = '\n'.join(sorted(text.splitlines(), key=lambda l: float(l.split()[0])))
        result = self.check(text)
        self.assertEqual(result['verdict'], 'PASS', result)
        self.assertEqual(result['seek_refill_write_gaps'], 1)
        for old, new in (('age=30 ts_age=20', 'age=24 ts_age=20'),
                         ('age=30 ts_age=20', 'age=-1 ts_age=20'),
                         ('age=30 ts_age=20', 'age=751 ts_age=20'),
                         ('encoded=7680', 'encoded=7681'),
                         ('encoded=7680', 'encoded=30720')):
            result = self.check(text.replace(old, new, 1))
            self.assertEqual(result['seek_refill_write_gaps'], 0, (old, new, result))
            self.assertIn('audio_write_gap', result['reason'])
        with self.assertRaises(ValueError):
            self.check(text.replace('age=30 ts_age=20', 'age=bad ts_age=20', 1))

    def test_bypass_report_is_not_required_for_refill_evidence(self):
        text = refill_fixture()
        marker = '1000.020 android_sync: mode2 seek qualification epoch=1 state=0 waited_ms=0 anchored=0'
        for replacement in ('', marker.replace('waited_ms=0', 'waited_ms=21'),
                            marker.replace('1000.020', '999.942').replace('waited_ms=0', 'waited_ms=21')):
            changed = '\n'.join(sorted(text.replace(marker, replacement).splitlines(),
                                       key=lambda l: float(l.split()[0]) if l else 0))
            result = self.check(changed)
            self.assertEqual(result['verdict'], 'PASS', result)
            self.assertEqual(result['seek_refill_write_gaps'], 1)
            self.assertEqual(result['seek_qualification_ms'], 0)
        seed = next(l for l in text.splitlines() if 'mode2_seek_calibration: seed ' in l)
        for replacement in ('', seed + '\n' + seed, seed.replace('1000.002', '999.999')):
            changed = '\n'.join(sorted(text.replace(seed, replacement).splitlines(),
                                       key=lambda l: float(l.split()[0]) if l else 0))
            result = self.check(changed)
            self.assertEqual(result['seek_refill_write_gaps'], 0)
            self.assertIn('audio_write_gap', result['reason'])

    def test_requires_completed_uninterrupted_matching_wait(self):
        text = refill_fixture()
        for old, new in (('interrupted=0', 'interrupted=1'),
                         ('unit_bytes=2560', 'unit_bytes=4096'),
                         ('compressed_write_wait: epoch=1', 'compressed_write_wait: epoch=2'),
                         ('blocked_ms=350', 'blocked_ms=1')):
            result = self.check(text.replace(old, new))
            self.assertEqual(result['seek_refill_write_gaps'], 0)
            self.assertIn('audio_write_gap', result['reason'])
        no_wait = '\n'.join(l for l in text.splitlines() if 'compressed_write_wait:' not in l)
        self.assertIn('audio_write_gap', self.check(no_wait)['reason'])

    def test_requires_fresh_same_domain_advancing_presentation(self):
        text = refill_fixture()
        for old, new in (('presented=48000', 'presented=24000'),
                         ('ts_age=20', 'ts_age=900'),
                         ('trusted=1', 'trusted=0'),
                         ('framing=2', 'framing=1'),
                         ('underruns=0', 'underruns=-1')):
            result = self.check(text.replace(old, new))
            self.assertEqual(result['seek_refill_write_gaps'], 0)
            self.assertIn('audio_write_gap', result['reason'])
        for markers in (('1000.001 mode2_occupancy_shadow:',),
                        ('1000.510 mode2_occupancy_shadow:', '1001.010 mode2_occupancy_shadow:')):
            changed = '\n'.join(l for l in text.splitlines() if not any(m in l for m in markers))
            self.assertIn('audio_write_gap', self.check(changed)['reason'])

    def test_later_gap_tail_underrun_and_pause_still_fail(self):
        text = refill_fixture()
        later = '\n'.join(l for l in text.splitlines()
                          if not ('audiotrack_write:' in l and 1002.09 < float(l.split()[0]) < 1002.45))
        self.assertIn('audio_write_gap', self.check(later)['reason'])
        self.assertIn('audio_write_gap', self.check(text + '\n1005.800 AVOS_TEST_POLL_2')['reason'])
        for event, reason in (
                ('1000.300 android_sync: pause start', 'audio_write_gap'),
                ('1000.300 stream_stop:', 'audio_write_gap'),
                ('1000.300 AudioTrack underruns: delta=1', 'audio_underruns'),
                ('1000.300 AUDIO_STARVED:', 'audio_starved')):
            changed = '\n'.join(sorted(text.splitlines() + [event], key=lambda l: float(l.split()[0])))
            self.assertIn(reason, self.check(changed)['reason'])

    def test_resume_keeps_write_gap_check(self):
        text = refill_fixture().replace('VIDEO_SEEK_TARGET_READY: epoch=1', 'WALLCLOCK_RESET: by pause resume')
        result = self.check(text, 'resume')
        self.assertEqual(result['seek_refill_write_gaps'], 0)
        self.assertIn('audio_write_gap', result['reason'])


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
"""Shared recovery, scheduled cadence and internal A/V phase checks.

Input is logcat -v epoch. This measures software/platform evidence, not light or
sound at the listener. No third party Python modules are required.
"""
import argparse
import math
import os
from pathlib import Path
import re
import statistics


DEFAULTS = {
    'STABLE_MEDIA_MS': 2000, 'AV_DIFF_MAX_MS': 1000,
    'VIDEO_GAP_MAX_MS': 250, 'WRITE_GAP_MAX_MS': 250,
    'RESUME_LATENCY_MAX_MS': 1000, 'SEEK_STARTUP_MAX_MS': 1000, 'REBASE_MAX': 1,
    'LATE_DROP_MAX': 0, 'UNDERRUN_MAX': 0, 'STARVED_MAX': 0,
    'HI_SPEED_MIN': 1.45, 'WINDOW_GAP_MS': 1500, 'HOLD_MIN_MS': 4800,
    'REQUIRE_AUDIO_PRESENTATION': 1, 'REQUIRE_RENDER_TIMING': 1,
    'EXPECTED_PHASE_MS': 0, 'AV_PHASE_MAX_MS': 80, 'AV_BAD_DURATION_MS': 250, 'AV_SETTLE_MS': 500,
    'RECOVERY_PHASE_MAX_MS': 8, 'RECOVERY_STABLE_MS': 250,
    'CADENCE_TOLERANCE_MS': 8, 'CADENCE_BAD_MAX': 0,
    'PRESENTATION_STALL_MS': 500, 'OBSERVER_FRESH_MS': 750, 'ARM_UNDERRUN': 0,
    'RESUME_BURST_PAIRS': 0,
}
FIELDS = re.compile(r'(?<![\w])([A-Za-z_][\w]*)=([^\s]+)')
RESUME_APPLIED = re.compile(r'android_sync: resume (?:shift offset|shift skipped|invalidates stale)')
POLL_MARKER = re.compile(r'(?:^|\s)AVOS_TEST_POLL_[0-9_]+(?:\s|$)')
FAULTS = {
    'runtime_error': re.compile(r'Fatal signal|FATAL EXCEPTION|ANR in|(?:ERROR_)?DEAD_OBJECT|seek watchdog restart failed'),
    'compressed_write_failure': re.compile(r'compressed write failed'),
    'log_loss': re.compile(r'dropped [0-9]+ lines|chatty.*expir|Unexpected EOF'),
}


class SeekPreview:
    """Match deferred preview output by media timestamp and seek epoch.

    A preview prepared while seeking can reach the renderer after resume. Do
    not classify it from submission time alone, or infer it from lateness.
    Missing identifying records leave the ordinary strict checks in force.
    """
    def __init__(self):
        self.active = False
        self.pending = set()

    def annotate(self, line, fields):
        if ('stream_open:' in line or 'stream_stop:' in line or
                'libavos_set_passthrough:' in line or
                'audiotrack_set_output_params: resolved' in line):
            self.active = False
            self.pending.clear()
        if '_stream_play_n_frames(n=' in line:
            if not self.active:
                self.pending.clear()
            self.active = True
        if 'SINK_REF_DEFERRED:' in line and self.active:
            try:
                self.pending.add((int(fields['seek_epoch']), int(fields['frame_time'])))
            except (KeyError, ValueError):
                pass
        if 'WALLCLOCK_RESET: by pause resume' in line or 'VIDEO_SEEK_TARGET_READY:' in line:
            self.active = False
        if 'video_render_diag:' in line:
            try:
                key = (int(fields['epoch']), int(fields['frame']))
            except (KeyError, ValueError):
                return
            if key in self.pending:
                fields['_seek_preview'] = True
                self.pending.remove(key)
            elif not self.active:
                # Once regular output resumes, never borrow an old identity
                # for a later frame, even if its timestamp happens to match.
                self.pending.clear()


class ResumeBoundary:
    """Account only for the wall shift/correction explicitly reported by the sink."""
    def __init__(self):
        self.generation = 0
        self.adjustment_ms = 0
        self.pending_correction_ms = 0
        self.pause_start_ms = None
        self.offset_before_ns = None
        self.boundaries = []

    def annotate(self, line, fields):
        if 'stream_open:' in line or 'stream_stop:' in line or 'VIDEO_SEEK_TARGET_READY:' in line:
            # Never borrow a monotonic-clock boundary from another session/seek.
            self.pause_start_ms = self.offset_before_ns = None
            self.pending_correction_ms = 0
            self.boundaries.clear()
        pause = re.search(r'android_sync: pause start at (\d+)', line)
        if pause:
            self.pause_start_ms = int(pause[1])
            self.offset_before_ns = None
            self.pending_correction_ms = 0
        state = re.search(r'android_sync: resume state offset=(-?\d+)', line)
        if state:
            self.offset_before_ns = int(state[1])
        correction = re.search(r'(?:mode1|PCM) resume applies paused correction=(-?\d+)us', line)
        if correction:
            self.pending_correction_ms += int(correction[1]) / 1000
        if RESUME_APPLIED.search(line):
            shift = re.search(r'resume shift offset by (\d+)ms', line)
            before = (self.generation, self.adjustment_ms)
            resume_ns = None
            if shift:
                after = re.search(r'-> (-?\d+)', line)
                if after and self.offset_before_ns is not None and self.offset_before_ns >= 0:
                    self.adjustment_ms += (int(after[1]) - self.offset_before_ns) / 1e6
                else:
                    self.adjustment_ms += int(shift[1]) + self.pending_correction_ms
                if self.pause_start_ms is not None:
                    resume_ns = (self.pause_start_ms + int(shift[1])) * 1000000
            self.pending_correction_ms = 0
            self.pause_start_ms = self.offset_before_ns = None
            self.generation += 1
            self.boundaries.append((resume_ns, before))
        fields['_resume_boundary'] = self.generation
        fields['_resume_adjustment_ms'] = self.adjustment_ms
        if 'video_render_diag:' in line and 'submit_ns' in fields:
            # Rendering releases the scheduler lock before emitting its log. A
            # resume can therefore be logged before an older submission. Only
            # undo a boundary when the embedded clock proves submission preceded
            # it; never infer a boundary from the size of a deadline jump.
            try:
                submit_ns = int(fields['submit_ns'])
            except ValueError:
                return  # The caller's render validation reports malformed data.
            for resume_ns, before in reversed(self.boundaries):
                if resume_ns is None or submit_ns >= resume_ns:
                    break
                fields['_resume_boundary'], fields['_resume_adjustment_ms'] = before
                fields['_resume_boundary_source'] = 'embedded_submit_before_resume'


def fault_signals(text):
    return {kind: [i for i, line in enumerate(text.splitlines(), 1) if pattern.search(line)]
            for kind, pattern in FAULTS.items()}


def write_metrics(text):
    counts = dict(full_writes=0, partial_writes=0, zero_writes=0,
                  failed_writes=0, accepted_bytes=0, compressed_continuations=0)
    for line in text.splitlines():
        match = re.search(r'audiotrack_write: wrote (-?\d+) out of (\d+)', line)
        if match:
            accepted, requested = map(int, match.groups())
            kind = ('failed_writes' if accepted < 0 or accepted > requested else
                    'zero_writes' if accepted == 0 else
                    'partial_writes' if accepted < requested else 'full_writes')
            counts[kind] += 1
            counts['accepted_bytes'] += max(0, accepted)
        counts['compressed_continuations'] += 'continuing unit at' in line
    # Interleaved writes have no transaction ID. Matching counts do not prove
    # that every compressed unit completed or that its samples were presented.
    counts['compressed_unit_completion'] = 'not_proven'
    return counts


def config(env=None):
    env = os.environ if env is None else env
    result = {}
    for key, default in DEFAULTS.items():
        value = float(env.get(key, default))
        if not math.isfinite(value) or (value < 0 and key != 'EXPECTED_PHASE_MS'):
            raise ValueError(f'{key} must be finite and non-negative')
        if key.startswith('REQUIRE_') or key == 'ARM_UNDERRUN':
            if value not in (0, 1):
                raise ValueError(f'{key} must be 0 or 1')
        if key == 'RESUME_BURST_PAIRS' and (not value.is_integer() or value > 100):
            raise ValueError('RESUME_BURST_PAIRS must be an integer from 0 to 100')
        result[key] = value
    result['PHASE_REFERENCE'] = env.get('PHASE_REFERENCE', 'explicit' if 'EXPECTED_PHASE_MS' in env else 'zero')
    result['REQUIRE_FILTER'] = env.get('REQUIRE_FILTER', '')
    return result


def records(text):
    result = []
    resume = ResumeBoundary()
    preview = SeekPreview()
    for line in text.splitlines():
        fields = dict(FIELDS.findall(line))
        resume.annotate(line, fields)
        preview.annotate(line, fields)
        try:
            stamp = float(line.split()[0]) * 1000
        except (ValueError, IndexError):
            continue
        if math.isfinite(stamp):
            result.append((stamp, line, fields))
    return result


def number(row, key):
    value = float(row[2][key])
    if not math.isfinite(value):
        raise ValueError(key)
    return value


def render_sequence(row):
    value = row[2].get('render_seq')
    if value is None:
        return None
    if not re.fullmatch(r'[0-9]+', value) or not 0 < int(value) < 2**64:
        raise ValueError('invalid render_seq')
    return int(value)


def render_continuity(a, b):
    """Return an evidence gap and known omitted count; never invent submissions."""
    first, last = render_sequence(a), render_sequence(b)
    if first is not None and last is not None:
        if last > first + 1:
            return 'missing_render_records', last - first - 1
        if last <= first:
            return 'missing_render_sequence_order', 0
    elif first is not None or last is not None:
        return 'missing_render_sequence', 0
    elif number(b, 'frame') - number(a, 'frame') > 1.5 * max(
            number(a, 'interval_ms'), number(b, 'interval_ms')):
        # Legacy logs cannot distinguish omitted records from skipped frames.
        # This is not enough evidence to label the interval judder or clean.
        return 'missing_render_continuity', 0
    return None, 0


def span(rows, key):
    return number(rows[-1], key) - number(rows[0], key) if len(rows) > 1 else 0


def gap(rows, end=None):
    times = [r[0] for r in rows]
    if end is not None and times:
        times.append(end)
    return max([b - a for a, b in zip(times, times[1:])] or [0])


def log_order(rows):
    """Check player and poll ordering separately; poll delivery is not a barrier."""
    high = None
    previous = {False: None, True: None}
    high_is_poll = False
    valid, poll_skew_ms = True, 0
    for stamp, line, _ in rows:
        # logd emits suppression summaries with older timestamps, including for
        # unrelated processes. They are not playback clock observations. Keep
        # them in fault_signals so explicit dropped/expired logs still fail.
        if re.match(r'^\s*\d+(?:\.\d+)?\s+\d+\s+\d+\s+[VDIWEF]\s+chatty\s*:', line):
            continue
        is_poll = bool(POLL_MARKER.search(line))
        last = previous[is_poll]
        if last is not None and stamp < last:
            valid = False
        previous[is_poll] = stamp if last is None else max(last, stamp)
        if high is not None and stamp < high:
            skew = high - stamp
            # A marker can overtake queued player logs by much more than clock
            # quantization. Keep this visible without confusing delivery order
            # with reversed playback time. Each stream must remain monotonic.
            if is_poll != high_is_poll:
                poll_skew_ms = max(poll_skew_ms, skew)
            else:
                valid = False
        if high is None or stamp > high or (stamp == high and not is_poll):
            high, high_is_poll = stamp, is_poll
    return valid, poll_skew_ms


def high_windows(rows, cfg):
    """Finalize every window before measuring it, including the final window."""
    windows = []
    first = last = None
    for row in rows:
        if 'put_time_calc:' not in row[1]:
            continue
        high = number(row, 'speed') >= cfg['HI_SPEED_MIN']
        if first is not None and (not high or row[0] - last > cfg['WINDOW_GAP_MS']):
            windows.append((first, last))
            first = last = None
        if high:
            first = row[0] if first is None else first
            last = row[0]
    if first is not None:
        windows.append((first, last))
    return windows


def scheduled_metrics(renders, start, end, cfg, findings=None):
    out, reasons = {}, []

    def finding(kind, first, last, **values):
        if findings is not None:
            findings.append(dict(kind=kind, first_line=first[2].get('line'),
                                 last_line=last[2].get('line'), **values))

    def require(ok, reason):
        if not ok and reason not in reasons:
            reasons.append(reason)

    playback = [r for r in renders if not r[2].get('_seek_preview')]
    out['render_count'] = len(playback)
    out['seek_preview_count'] = len(renders) - len(playback)
    out['seek_preview_max_lateness_ms'] = 0
    out['cadence_bad'] = 0
    out['max_cadence_error_ms'] = 0
    out['max_lateness_ms'] = 0
    out['missing_render_records'] = 0
    out['render_continuity_gaps'] = 0
    sequenced = sum(render_sequence(r) is not None for r in renders)
    out['render_sequence'] = ('missing' if not renders else 'legacy' if not sequenced else
                              'available' if sequenced == len(renders) else 'mixed')
    phase_rows = []
    out['expected_phase_ms'] = cfg['EXPECTED_PHASE_MS']
    out['phase_reference'] = cfg['PHASE_REFERENCE']
    for r in renders:
        interval = number(r, 'interval_ms')
        late = (int(r[2]['submit_ns']) - int(r[2]['deadline_ns'])) / 1e6
        if r[2].get('_seek_preview'):
            out['seek_preview_max_lateness_ms'] = max(out['seek_preview_max_lateness_ms'], late)
            finding('queued_seek_preview', r, r, lateness_ms=late)
            require(late <= cfg['SEEK_STARTUP_MAX_MS'], 'seek_preview_startup_late')
            if late > cfg['SEEK_STARTUP_MAX_MS']:
                finding('seek_preview_startup_late', r, r, lateness_ms=late,
                        limit_ms=cfg['SEEK_STARTUP_MAX_MS'])
            continue
        out['max_lateness_ms'] = max(out['max_lateness_ms'], late)
        require(interval > 0, 'missing_frame_rate')
        require(late <= max(interval, cfg['CADENCE_TOLERANCE_MS']), 'late_submission')
        if late > max(interval, cfg['CADENCE_TOLERANCE_MS']):
            finding('late_submission', r, r, lateness_ms=late,
                    limit_ms=max(interval, cfg['CADENCE_TOLERANCE_MS']))
        if r[0] >= start + cfg['AV_SETTLE_MS'] and 0 <= number(r, 'anchor_age_ms') <= 100:
            phase_rows.append(r)
    continuity_run = 0
    if renders:
        renders[0][2]['_render_continuity_run'] = continuity_run
    for a, b in zip(renders, renders[1:]):
        evidence_gap, omitted = render_continuity(a, b) if a[2]['epoch'] == b[2]['epoch'] else (None, 0)
        if evidence_gap or a[2]['epoch'] != b[2]['epoch']:
            continuity_run += 1
        b[2]['_render_continuity_run'] = continuity_run
        if a[2]['epoch'] != b[2]['epoch']:
            continue
        if a[2].get('_seek_preview') or b[2].get('_seek_preview'):
            # Keep sequence-loss checks at this boundary, but preview deadlines
            # do not describe ordinary playing cadence.
            if evidence_gap:
                out['missing_render_records'] += omitted
                out['render_continuity_gaps'] += 1
                require(False, evidence_gap)
                finding(evidence_gap, a, b, omitted_records=omitted)
            continue
        # Changing cadence is expected during a speed ramp. Use the new cadence
        # and tolerate the larger adjacent interval only at the boundary.
        expected = number(b, 'interval_ms')
        actual = (int(b[2]['deadline_ns']) - int(a[2]['deadline_ns'])) / 1e6
        if evidence_gap:
            require(False, evidence_gap)
            out['render_continuity_gaps'] += 1
            out['missing_render_records'] += omitted
            finding(evidence_gap, a, b, previous_seq=render_sequence(a),
                    current_seq=render_sequence(b), omitted_records=omitted,
                    interval_ms=actual, media_delta_ms=number(b, 'frame') - number(a, 'frame'))
            # Increasing sequence numbers can independently prove a deadline
            # reversal. Duplicated/reordered diagnostics cannot prove cadence.
            if evidence_gap != 'missing_render_records' or actual > 0:
                continue
        error = min(abs(actual - expected), abs(actual - number(a, 'interval_ms')))
        if a[2].get('_resume_boundary') != b[2].get('_resume_boundary'):
            adjustment = b[2].get('_resume_adjustment_ms', 0) - a[2].get('_resume_adjustment_ms', 0)
            adjusted = actual - adjustment
            error = min(abs(adjusted - expected), abs(adjusted - number(a, 'interval_ms')))
            finding('resume_boundary_adjustment', a, b, interval_ms=actual,
                    expected_ms=expected, accounted_shift_ms=adjustment, residual_error_ms=error)
            if adjusted > 0 and error <= cfg['CADENCE_TOLERANCE_MS']:
                continue
        out['max_cadence_error_ms'] = max(out['max_cadence_error_ms'], error)
        if actual <= 0 or error > cfg['CADENCE_TOLERANCE_MS']:
            out['cadence_bad'] += 1
            finding('scheduled_judder', a, b, interval_ms=actual, expected_ms=expected,
                    previous_expected_ms=number(a, 'interval_ms'), error_ms=error)
    require(out['cadence_bad'] <= cfg['CADENCE_BAD_MAX'], 'scheduled_judder')
    out['timing'] = 'scheduled' if len(playback) >= 3 else 'missing'
    if cfg['REQUIRE_RENDER_TIMING']:
        require(len(playback) >= 3, 'missing_render_timing')
        require(bool(playback) and end - playback[-1][0] <= cfg['VIDEO_GAP_MAX_MS'], 'stale_render_timing')
        require(len(phase_rows) >= 3, 'missing_fresh_audio_anchor')
        require(bool(phase_rows) and end - phase_rows[-1][0] <= cfg['VIDEO_GAP_MAX_MS'], 'stale_audio_anchor')
    out['phase_raw_min_ms'] = min([number(r, 'phase_ms') for r in phase_rows] or [0])
    out['phase_raw_max_ms'] = max([number(r, 'phase_ms') for r in phase_rows] or [0])
    out['phase_median_ms'] = statistics.median([number(r, 'phase_ms') for r in phase_rows]) if phase_rows else None
    out['phase_max_ms'] = max([abs(number(r, 'phase_ms') - cfg['EXPECTED_PHASE_MS']) for r in phase_rows] or [0])
    bad_start = previous_time = previous_run = None
    bad_rows = []

    def finish_phase_run():
        if bad_rows and bad_rows[-1][0] - bad_rows[0][0] >= cfg['AV_BAD_DURATION_MS']:
            finding('sustained_av_phase_error', bad_rows[0], bad_rows[-1],
                    duration_ms=bad_rows[-1][0] - bad_rows[0][0],
                    deviation_max_ms=max(abs(number(r, 'phase_ms') - cfg['EXPECTED_PHASE_MS']) for r in bad_rows),
                    reference_ms=cfg['EXPECTED_PHASE_MS'])
        bad_rows.clear()

    worst_run = 0
    for r in phase_rows:
        current_run = r[2]['_render_continuity_run']
        if previous_time is not None and (r[0] - previous_time > cfg['VIDEO_GAP_MAX_MS'] or
                                          current_run != previous_run):
            bad_start = None
            finish_phase_run()
        if abs(number(r, 'phase_ms') - cfg['EXPECTED_PHASE_MS']) > cfg['AV_PHASE_MAX_MS']:
            bad_rows.append(r)
            bad_start = r[0] if bad_start is None else bad_start
            worst_run = max(worst_run, r[0] - bad_start)
        else:
            bad_start = None
            finish_phase_run()
        previous_time = r[0]
        previous_run = current_run
    finish_phase_run()
    out['phase_bad_ms'] = worst_run
    require(worst_run < cfg['AV_BAD_DURATION_MS'] or out['phase_max_ms'] <= cfg['AV_PHASE_MAX_MS'], 'sustained_av_phase_error')
    return out, reasons


def analyze(text, mode, cfg=None):
    cfg = config() if cfg is None else cfg
    all_rows = records(text)
    out = {'healthy': 0, 'physical_lipsync': 'unmeasured'}
    out.update(write_metrics(text))
    reasons = []

    def require(ok, reason):
        if not ok and reason not in reasons:
            reasons.append(reason)

    rows = all_rows
    expected_pid = os.environ.get('PLAYER_PID', '')
    if expected_pid:
        rows = [r for r in rows if 'avos_player:' not in r[1] or
                (len(r[1].split()) > 2 and r[1].split()[1] == expected_pid)]
    burst = mode == 'resume' and cfg['RESUME_BURST_PAIRS'] > 0
    signal_rows = None
    if burst:
        begin = [i for i, r in enumerate(rows) if 'AVOS_TEST_BURST_BEGIN' in r[1]]
        finish = [i for i, r in enumerate(rows) if 'AVOS_TEST_BURST_END' in r[1]]
        require(len(begin) == len(finish) == 1 and begin[0] < finish[0], 'missing_burst_markers')
        if begin:
            rows = rows[begin[0] + 1:]
        signal_rows = rows
        # Count actual native transitions, not injected keys or the complementary
        # audio_resume_route marker. Callbacks may arrive after injection END.
        transitions = []
        for i, r in enumerate(rows):
            if 'android_sync: pause start' in r[1]:
                transitions.append(('pause', i))
            elif 'WALLCLOCK_RESET: by pause resume' in r[1]:
                transitions.append(('resume', i))
        out['burst_requested_pairs'] = int(cfg['RESUME_BURST_PAIRS'])
        out['burst_pauses'] = sum(kind == 'pause' for kind, _ in transitions)
        out['burst_resumes'] = sum(kind == 'resume' for kind, _ in transitions)
        require([kind for kind, _ in transitions] == ['pause', 'resume'] * out['burst_requested_pairs'],
                'burst_transition_mismatch')
        require(not any('VIDEO_SEEK_TARGET_READY:' in r[1] for r in rows), 'unexpected_seek_in_burst')
        markers = [i for kind, i in transitions if kind == 'resume']
        out['resume'] = len(markers)
        require(bool(markers), 'missing_transition')
        if markers:
            rows = rows[markers[-1]:]
    elif mode != 'speed':
        markers = [i for i, r in enumerate(rows) if
                   ('VIDEO_SEEK_TARGET_READY:' in r[1] if mode == 'seek' else
                    'WALLCLOCK_RESET: by pause resume' in r[1] or 'audio_resume_route:' in r[1])]
        out['ready' if mode == 'seek' else 'resume'] = len(markers)
        require(bool(markers), 'missing_transition')
        if markers:
            # Resume emits two complementary markers; seek may supersede epochs.
            rows = rows[markers[-1] if mode == 'seek' else markers[0]:]
    if signal_rows is None:
        signal_rows = rows
    if not rows:
        return dict(out, reason='missing_evidence')
    start, end = rows[0][0], max(r[0] for r in rows)
    ordered, out['poll_marker_skew_ms'] = log_order(signal_rows)
    require(ordered, 'log_time_reversed')
    # Live snapshots end with a device log marker, so a silent tail is measured.
    # If the marker trails a player stamp in delivery order, retain that
    # observed player time as the endpoint instead of shortening the window.
    out['observation_ms'] = end - start
    writes = [r for r in rows if re.search(r'audiotrack_write: wrote [1-9][0-9]* out of', r[1])]
    videos = [r for r in rows if 'video_sched_diag:' in r[1]]
    renders = [r for r in rows if 'video_render_diag:' in r[1]]
    clocks = [r for r in rows if 'put_time_calc:' in r[1]]
    for r in videos:
        for key in ('frame', 'audio', 'frame_minus_heard', 'seek_epoch'):
            number(r, key)
    for r in renders:
        for key in ('frame', 'epoch', 'deadline_ns', 'submit_ns', 'interval_ms', 'anchor_age_ms', 'phase_ms'):
            number(r, key)
    epoch = rows[0][2].get('epoch') if mode == 'seek' else None
    if videos:
        epoch = epoch or videos[0][2]['seek_epoch']
        require(all(r[2]['seek_epoch'] == epoch for r in videos), 'unexpected_seek_epoch')
        require(all(r[2]['epoch'] == epoch for r in renders), 'stale_render_epoch')
    out['epoch'] = epoch or 'unknown'
    require(len(writes) >= 2 and len(videos) >= 3, 'insufficient_progress_records')
    out.update(writes=len(writes), video_count=len(videos),
               video_span=span(videos, 'frame'), audio_span=span(videos, 'audio'))
    minimum = cfg['STABLE_MEDIA_MS']
    require(out['video_span'] >= minimum and out['audio_span'] >= minimum, 'insufficient_media_progress')
    require(end - start >= minimum, 'insufficient_wall_progress')
    require(all(number(b, 'frame') > number(a, 'frame') for a, b in zip(videos, videos[1:])), 'video_not_monotonic')
    # Transition ramps can legitimately remap buffered TS. Each high window is
    # checked separately below; ordinary recovery must stay in one time mapping.
    if mode == 'speed':
        reasons = [x for x in reasons if x != 'video_not_monotonic']
    out['max_write_gap'] = gap(writes, end)
    out['max_video_gap'] = gap(videos, end)
    out['starved'] = sum('AUDIO_STARVED:' in r[1] for r in signal_rows)
    out['late_drops'] = sum('late frame drop' in r[1] for r in signal_rows)
    out['clock_records'] = len(clocks)
    require(bool(clocks), 'missing_clock_records')
    out['rebases'] = sum(r[2].get('allow_reanchor') == '1' for r in clocks)
    out['disc'] = sum(r[2].get('disc') == '1' for r in clocks)
    out['pcm_rebase'] = sum('PCM resume anchor' in r[1] for r in rows)
    out['pcm_slew'] = sum('PCM resume slew' in r[1] for r in rows)
    underruns = [r for r in signal_rows if 'AudioTrack underruns:' in r[1]]
    out['underruns'] = sum(max(0, number(r, 'delta')) for r in underruns)
    out['underrun_monitor'] = 'requested' if cfg['ARM_UNDERRUN'] else 'unverified'
    require(out['starved'] <= cfg['STARVED_MAX'], 'audio_starved')
    require(out['late_drops'] <= cfg['LATE_DROP_MAX'], 'late_frames')
    require(out['underruns'] <= cfg['UNDERRUN_MAX'], 'audio_underruns')
    for kind, lines in fault_signals(text).items():
        require(not lines, kind)
    if mode in ('resume', 'seek'):
        startup_limit = cfg['SEEK_STARTUP_MAX_MS'] if mode == 'seek' else cfg['RESUME_LATENCY_MAX_MS']
        startup_items = [('write', writes), ('video', videos)]
        if mode == 'seek' and cfg['REQUIRE_RENDER_TIMING']:
            startup_items.append(('render', [r for r in renders if not r[2].get('_seek_preview')]))
        for label, items in startup_items:
            latency = items[0][0] - start if items else -1
            out[f'first_{label}_ms'] = latency
            require(0 <= latency <= startup_limit,
                    'seek_startup_latency' if mode == 'seek' else 'resume_latency')
    if mode == 'resume':
        require(out['rebases'] <= cfg['REBASE_MAX'], 'rebase_churn')

    # Counters are observed, not extrapolated. Prefer explicit observations over
    # old fallback logs, and never merge timestamp and playback-head domains.
    explicit = [r for r in rows if 'audio_present_diag:' in r[1]]
    shadows = [r for r in rows if 'mode1_iec_occupancy_shadow:' in r[1] or 'mode2_occupancy_shadow:' in r[1]]
    present = [r for r in explicit if 0 <= number(r, 'age_ms') <= 100]
    presentation_fresh_ms = cfg['PRESENTATION_STALL_MS']
    if shadows and not present:
        # The compressed observer emits every 500ms. Its counter is in the
        # route's logical/carrier domain; never mix it with PCM/playhead units.
        presentation_fresh_ms = cfg['OBSERVER_FRESH_MS']
        present = [r for r in shadows if
                   0 <= number(r, 'age') <= cfg['OBSERVER_FRESH_MS'] and
                   r[2].get('src') == '1' and
                   0 <= number(r, 'ts_age') <= cfg['OBSERVER_FRESH_MS']]
    if not explicit and not shadows:
        present = [r for r in rows if 'playhead_delay:' in r[1] or 'playhead_streak:' in r[1]]
    streams = {}
    ordered_present = []
    newest_sample = {}
    out['presentation_out_of_order'] = 0
    for r in present:
        domain = (r[2].get('source', r[2].get('src', 'playhead')),
                  r[2].get('generation', ''), r[2].get('epoch', ''))
        sample_ns = r[2].get('sample_ns')
        if sample_ns is not None:
            sample_ns = int(sample_ns)
            if sample_ns < newest_sample.get(domain, sample_ns):
                # Concurrent queries can log an older observation after a newer
                # one. It proves neither a counter reset nor fresh progress.
                out['presentation_out_of_order'] += 1
                continue
            newest_sample[domain] = sample_ns
        streams.setdefault(domain, []).append(r)
        ordered_present.append(r)
    present = ordered_present
    advancing = False
    for samples in streams.values():
        advancing |= len(samples) >= 2 and span(samples, 'presented') > 0
        frozen_since = samples[0][0]
        for previous, sample in zip(samples, samples[1:]):
            delta = number(sample, 'presented') - number(previous, 'presented')
            require(delta >= 0, 'presentation_reset')
            if delta > 0:
                frozen_since = sample[0]
            require(sample[0] - frozen_since <= presentation_fresh_ms, 'presentation_stalled')
    out['presented_count'] = len(present)
    out['presentation_max_gap_ms'] = max(gap(present, end),
                                       present[0][0] - start if present else end - start)
    out['presented_span'] = max([span(v, 'presented') for v in streams.values()] or [0])
    out['presentation'] = 'advancing' if advancing else 'missing_or_stalled'
    if cfg['REQUIRE_AUDIO_PRESENTATION']:
        require(advancing, 'missing_presentation_progress')
        require(out['presentation_max_gap_ms'] <= presentation_fresh_ms, 'missing_presentation_observations')
        require(bool(present) and end - present[-1][0] <= presentation_fresh_ms, 'stale_presentation')
    # Observer underruns are cumulative and generation scoped. Count changes,
    # not the first historical total; the write-side delta may duplicate them.
    observer_underruns = 0
    previous_counts = {}
    signal_shadows = [r for r in signal_rows if 'mode1_iec_occupancy_shadow:' in r[1] or
                      'mode2_occupancy_shadow:' in r[1]]
    for r in signal_shadows:
        count = number(r, 'underruns')
        generation = r[2]['generation']
        if count >= 0:
            if generation in previous_counts:
                observer_underruns += max(0, count - previous_counts[generation])
            previous_counts[generation] = count
    out['underruns'] = max(out['underruns'], observer_underruns)
    if len(signal_shadows) >= 2 and previous_counts:
        out['underrun_monitor'] = 'observer'
    require(out['underruns'] <= cfg['UNDERRUN_MAX'], 'audio_underruns')

    windows = [(start, end)]
    if mode == 'speed':
        require(out['max_write_gap'] <= cfg['WRITE_GAP_MAX_MS'], 'audio_write_gap')
        require(out['max_video_gap'] <= cfg['VIDEO_GAP_MAX_MS'], 'video_feed_gap')
    if mode == 'speed':
        windows = high_windows(rows, cfg)
        out['hi_ms'] = max([b - a for a, b in windows] or [0])
        require(out['hi_ms'] >= cfg['HOLD_MIN_MS'], 'high_window_too_short')
        speed_rows = [r for r in all_rows if 'put_time_calc:' in r[1]]
        require(bool(speed_rows) and abs(number(speed_rows[-1], 'speed') - 1) <= .01, 'speed_not_restored')
        filters = set(re.findall(r'applying speed filter \[([^]]+)\]', text))
        require(not cfg['REQUIRE_FILTER'] or filters == {cfg['REQUIRE_FILTER']}, 'wrong_filter')
    for lo, hi in windows:
        window_v = [r for r in videos if lo <= r[0] <= hi]
        window_w = [r for r in writes if lo <= r[0] <= hi]
        # Seek target readiness precedes audio readiness and render scheduling.
        # Check that leading wait against the startup budget above; retain the
        # ordinary gap limit between records and through the silent tail.
        vg = gap(window_v, hi) if window_v else hi - lo
        wg = gap(window_w, hi) if window_w else hi - lo
        if mode != 'seek':
            vg = max(vg, window_v[0][0] - lo if window_v else hi - lo)
            wg = max(wg, window_w[0][0] - lo if window_w else hi - lo)
        if mode == 'speed':
            out['hi_max_video_gap'] = max(out.get('hi_max_video_gap', 0), vg)
            out['hi_max_write_gap'] = max(out.get('hi_max_write_gap', 0), wg)
            if hi - lo >= cfg['HOLD_MIN_MS']:
                require(span(window_v, 'frame') >= cfg['HOLD_MIN_MS'] * .8 and
                        span(window_v, 'audio') >= cfg['HOLD_MIN_MS'] * .8, 'high_window_no_progress')
            require(all(number(b, 'frame') > number(a, 'frame') for a, b in zip(window_v, window_v[1:])), 'video_not_monotonic')
        require(vg <= cfg['VIDEO_GAP_MAX_MS'], 'video_feed_gap')
        require(wg <= cfg['WRITE_GAP_MAX_MS'], 'audio_write_gap')

    out['last_diff'] = number(videos[-1], 'frame_minus_heard') if videos else 0
    steady_v = [r for r in videos if r[0] >= start + cfg['AV_SETTLE_MS']]
    require(not any(abs(number(r, 'frame_minus_heard')) > cfg['AV_DIFF_MAX_MS'] for r in steady_v), 'gross_av_divergence')
    timing, failures = scheduled_metrics(renders, start, end, cfg)
    out.update(timing)
    for failure in failures:
        require(False, failure)
    out['healthy'] = int(not reasons)
    out['reason'] = ','.join(reasons) or 'ok'
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['seek', 'resume', 'speed'])
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    try:
        result = analyze(args.log.read_text(errors='replace'), args.mode)
    except (ValueError, KeyError, OSError) as error:
        print('healthy=0 reason=invalid_evidence_or_config detail=' + str(error).replace(' ', '_'))
        return 2
    print(' '.join(f'{k}={v:.3f}' if isinstance(v, float) else f'{k}={v}' for k, v in result.items()))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

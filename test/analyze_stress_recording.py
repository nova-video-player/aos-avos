#!/usr/bin/env python3
"""Analyze render records in an arbitrary saved log, including logcat brief.

No timestamps are synthesized for unclocked writes or lifecycle messages.
Physical output remains unmeasured. JSON output is suitable for sharing.
"""
import argparse
import json
from pathlib import Path
import re
import statistics

from analyze_stress import (FIELDS, RESUME_APPLIED, ResumeBoundary, SeekPreview, config, fault_signals, number,
                            render_sequence, scheduled_metrics, write_metrics)


BOUNDARY = re.compile(r'WALLCLOCK_RESET: by pause resume|VIDEO_SEEK_TARGET_READY:|stream_open:|stream_stop:')


def recovery_metrics(frames, cfg):
    """Recovery relative to the first render submission, not an unclocked key event."""
    frames = [r for r in frames if not r[2].get('_seek_preview')]
    fresh = [r for r in frames if 0 <= number(r, 'anchor_age_ms') <= 100]
    good = []
    settled = None
    first_recovery = None
    for row in fresh:
        if good and (row[0] - good[-1][0] > cfg['VIDEO_GAP_MAX_MS'] or
                     row[2].get('_render_continuity_run') != good[-1][2].get('_render_continuity_run')):
            good = []
            settled = None
        if abs(number(row, 'phase_ms') - cfg['EXPECTED_PHASE_MS']) <= cfg['RECOVERY_PHASE_MAX_MS']:
            good.append(row)
            if settled is None and len(good) >= 3 and good[-1][0] - good[0][0] >= cfg['RECOVERY_STABLE_MS']:
                settled = good[0][0]
                if first_recovery is None:
                    first_recovery = settled
        else:
            good = []
            settled = None
    if not fresh or frames[-1][0] - fresh[-1][0] > cfg['VIDEO_GAP_MAX_MS']:
        settled = None
    return dict(time_origin='first_render_submission',
                initial_phase_ms=number(fresh[0], 'phase_ms') if fresh else None,
                initial_deviation_ms=number(fresh[0], 'phase_ms') - cfg['EXPECTED_PHASE_MS'] if fresh else None,
                status='settled' if settled is not None else
                'not_settled_at_segment_end' if first_recovery is not None else 'not_observed_before_segment_end',
                recovery_ms=round(first_recovery-frames[0][0], 3) if first_recovery is not None else None,
                final_stable_run_ms=round(settled-frames[0][0], 3) if settled is not None else None,
                settled_phase_median_ms=statistics.median([number(r, 'phase_ms') for r in good]) if settled is not None else None)


def analyze_recording(text, cfg=None):
    cfg = config() if cfg is None else dict(cfg)
    sections = []
    frames = []
    boundary = 'start'
    paused_or_preview = False
    excluded_frames = 0
    counts = {'late_drops': 0, 'starved': 0, 'resume_markers': 0, 'seek_markers': 0}
    shadow_groups = {}
    invalid_records = []
    write_underruns = 0
    resume = ResumeBoundary()
    preview = SeekPreview()
    transitions = []
    context_changes = []
    current_mode = None
    player_pid = None
    output_signature = None
    seen_render = False

    def finish():
        nonlocal frames
        if frames:
            sections.append((boundary, frames))
            frames = []

    for line_number, line in enumerate(text.splitlines(), 1):
        fields = dict(FIELDS.findall(line))
        resume.annotate(line, fields)
        preview.annotate(line, fields)
        mode = re.search(r'libavos_set_passthrough: mode=(\d+)', line)
        changed = False
        pid = re.search(r'avos_player\(\s*(\d+)\)', line) or re.match(
            r'^\d+(?:\.\d+)?\s+(\d+)\s+\d+\s+[VDIWEF]\s+avos_player\s*:', line)
        if pid:
            changed = player_pid is not None and pid[1] != player_pid
            player_pid = pid[1]
        if mode:
            mode = int(mode[1])
            changed |= current_mode is not None and mode != current_mode
            current_mode = mode
        elif current_mode is None:
            if 'mode1_iec_occupancy_shadow:' in line:
                current_mode = 1
            elif 'mode2_occupancy_shadow:' in line:
                current_mode = 2
        if 'audiotrack_set_output_params: resolved' in line:
            signature = tuple(fields.get(k) for k in ('out_rate', 'out_channels', 'track_format', 'passthrough'))
            changed |= output_signature is not None and signature != output_signature
            output_signature = signature
        if seen_render and (changed or 'stream_open:' in line):
            preview = SeekPreview()
            finish()
            context_changes.append(line_number)
            boundary = 'playback_context_change'
        if ('android_sync: mode1 ' in line or 'PCM resume applies paused correction=' in line
                or 'android_sync: mode2 ' in line or 'mode2_epoch_seed:' in line
                or re.search(r'mode2_dynamic_clock_(enter|ready|fallback):', line)
                or RESUME_APPLIED.search(line)):
            transitions.append(dict(line=line_number, message=line.strip()))
        if 'android_sync: pause start' in line or '_stream_play_n_frames(n=' in line:
            finish()
            paused_or_preview = True
        if 'WALLCLOCK_RESET: by pause resume' in line or 'VIDEO_SEEK_TARGET_READY:' in line:
            paused_or_preview = False
        if BOUNDARY.search(line):
            finish()
            boundary = line.strip().split(': ', 1)[-1]
        counts['late_drops'] += 'late frame drop' in line
        counts['starved'] += 'AUDIO_STARVED:' in line
        counts['resume_markers'] += 'WALLCLOCK_RESET: by pause resume' in line
        counts['seek_markers'] += 'VIDEO_SEEK_TARGET_READY:' in line
        if 'AudioTrack underruns:' in line:
            try:
                write_underruns += max(0, int(fields['delta']))
            except (KeyError, ValueError):
                invalid_records.append(line_number)
        if 'occupancy_shadow:' in line:
            try:
                domain = (fields['generation'], fields['epoch'], fields['src'])
                shadow_groups.setdefault(domain, []).append((int(fields['presented']), int(fields['underruns'])))
            except (KeyError, ValueError):
                invalid_records.append(line_number)
        if 'video_render_diag:' not in line:
            continue
        if paused_or_preview:
            excluded_frames += 1
            continue
        try:
            row = (int(fields['submit_ns']) / 1e6, line, fields)
            for key in ('frame', 'epoch', 'deadline_ns', 'interval_ms', 'anchor_age_ms', 'phase_ms'):
                number(row, key)
            int(fields['deadline_ns'])
            render_sequence(row)
        except (KeyError, ValueError):
            invalid_records.append(line_number)
            continue
        row[2]['line'] = line_number
        seen_render = True
        if frames and (fields['epoch'] != frames[-1][2]['epoch'] or row[0] < frames[-1][0]):
            finish()
            boundary = 'epoch_or_monotonic_reset'
        frames.append(row)
    finish()
    # An initial route-relative reference is explicit in the report. It can
    # detect later shifts but cannot establish the absolute lipsync of the clip.
    reference_missing = False
    if cfg['PHASE_REFERENCE'] == 'zero' and ('mode1_iec_occupancy_shadow:' in text or
                                           re.search(r'passthrough=1\b|libavos_set_passthrough: mode=1\b', text)):
        reference_missing = True
        for _, frames in sections:
            samples = [number(r, 'phase_ms') for r in frames if not r[2].get('_seek_preview') and
                       cfg['AV_SETTLE_MS'] <= r[0] - frames[0][0] <= cfg['AV_SETTLE_MS'] + 1000 and
                       0 <= number(r, 'anchor_age_ms') <= 100]
            if len(samples) >= 3:
                # Do not borrow a later, possibly already drifted baseline.
                if max(samples) - min(samples) <= 80:
                    cfg['EXPECTED_PHASE_MS'] = statistics.median(samples)
                    cfg['PHASE_REFERENCE'] = 'recording_initial'
                    reference_missing = False
                break
        if reference_missing:
            cfg['PHASE_REFERENCE'] = 'unavailable'
    reports = []
    for index, (boundary, frames) in enumerate(sections, 1):
        playback = [r for r in frames if not r[2].get('_seek_preview')]
        start, end = (playback[0][0] if playback else frames[0][0]), frames[-1][0]
        findings = []
        metrics, reasons = scheduled_metrics(frames, start, end, cfg, findings)
        # A route/session change needs a new capture reference. Keep cadence
        # evidence, but never compare unrelated outputs against the old phase.
        phase_unavailable = reference_missing or any(i <= frames[0][2]['line'] for i in context_changes)
        if phase_unavailable:
            reasons = [r for r in reasons if r != 'sustained_av_phase_error']
            findings = [f for f in findings if f['kind'] != 'sustained_av_phase_error']
            reasons.append('missing_stable_phase_reference')
            metrics.update(expected_phase_ms=None, phase_reference='unavailable',
                           phase_max_ms=None, phase_bad_ms=None)
        complete = end - start >= cfg['STABLE_MEDIA_MS'] and len(playback) >= 3
        gaps = [r for r in reasons if r.startswith('missing_') or r.startswith('stale_')]
        issues = [r for r in reasons if r not in gaps]
        reports.append(dict(segment=index, boundary=boundary,
                            first_line=frames[0][2]['line'], last_line=frames[-1][2]['line'],
                            epoch=frames[0][2]['epoch'], duration_ms=round(end-start, 3),
                            coverage='sufficient' if complete else 'short',
                            issues=issues, evidence_gaps=gaps, findings=findings,
                            recovery=recovery_metrics(frames, cfg) if not phase_unavailable else
                            {'status': 'reference_unavailable'}, **metrics))
        reports[-1]['recovery']['ended_by'] = sections[index][0] if index < len(sections) else 'end_of_capture'
    # Avoid double counting the writer and observer versions of the same event.
    observer_underruns = sum(max(0, b[1] - a[1]) for values in shadow_groups.values()
                            for a, b in zip(values, values[1:]) if min(a[1], b[1]) >= 0)
    counts['underruns_observed'] = max(observer_underruns, write_underruns)
    counts['presentation_domains_advancing'] = sum(len(v) > 1 and v[-1][0] > v[0][0] for v in shadow_groups.values())
    counts['excluded_pause_preview_frames'] = excluded_frames
    counts['render_frames'] = sum(len(frames) for _, frames in sections)
    counts.update(write_metrics(text))
    faults = fault_signals(text)
    signal_issues = [reason for count, setting, reason in (
        ('late_drops', 'LATE_DROP_MAX', 'late_frames'),
        ('starved', 'STARVED_MAX', 'audio_starved'),
        ('underruns_observed', 'UNDERRUN_MAX', 'audio_underruns')) if counts[count] > cfg[setting]]
    signal_issues += [kind for kind, lines in faults.items() if lines and kind != 'log_loss']
    sufficient = any(r['coverage'] == 'sufficient' and not r['evidence_gaps'] for r in reports)
    evidence_gaps = (['log_loss'] if faults['log_loss'] else []) + (['malformed_records'] if invalid_records else [])
    # Short interrupted segments need not prove sustained recovery, but missing
    # render diagnostics anywhere must not disappear behind later good coverage.
    evidence_gaps += sorted({gap for r in reports for gap in r['evidence_gaps']
                             if gap.startswith('missing_render_') and gap != 'missing_render_timing'})
    if context_changes:
        evidence_gaps.append('playback_context_changed')
    if reference_missing:
        evidence_gaps.append('missing_stable_phase_reference')
    issues = signal_issues or any(r['issues'] for r in reports)
    return {
        'scope': 'scheduled_video_and_internal_phase',
        'physical_lipsync': 'unmeasured', 'displayed_cadence': 'unmeasured',
        'audio_write_gaps': 'not_evaluated', 'resume_latency': 'not_evaluated',
        'phase_reference': cfg['PHASE_REFERENCE'], 'expected_phase_ms': cfg['EXPECTED_PHASE_MS'],
        'config': cfg, 'signals': counts, 'malformed_record_lines': invalid_records,
        'signal_issues': signal_issues, 'fault_lines': faults, 'evidence_gaps': evidence_gaps,
        'context_change_lines': context_changes, 'transition_records': transitions,
        'timing_coverage': 'sufficient' if sufficient and not evidence_gaps else 'insufficient',
        'segments': reports,
        'verdict': ('ISSUES_OBSERVED' if issues else
                    'INSUFFICIENT_EVIDENCE' if evidence_gaps or not sufficient or any(
                        r['evidence_gaps'] for r in reports if r['coverage'] == 'sufficient') else
                    'NO_ISSUES_OBSERVED'),
    }


def markdown_report(report, source):
    """Readable review artifact; JSON retains the same evidence and thresholds."""
    def cell(value):
        return str(value).replace('|', '\\|').replace('\n', ' ')

    lines = ['# Playback timing review', '', f"Source: {cell(source)}", '',
             f"Verdict: **{report['verdict']}**", '',
             f"Timing coverage: {report['timing_coverage']}. "
             f"Phase reference: {report['phase_reference']} ({report['expected_phase_ms']:g} ms).", '',
             'Signal issues: ' + (', '.join(report['signal_issues']) or 'none observed') + '.',
             'Evidence gaps: ' + (', '.join(report['evidence_gaps']) or 'none') + '.', '',
             'Physical lipsync and displayed cadence: **unmeasured**. '
             'Write gaps and resume latency: **not evaluated**.', '',
             'A relative phase reference detects changes; it does not establish absolute lipsync. '
             'Short segments can expose faults but cannot establish sustained healthy playback.', '',
             '## Recorded signals', '', '| Signal | Count |', '|---|---:|']
    lines += [f'| {key} | {value} |' for key, value in report['signals'].items()]
    lines += ['', '## Segments', '',
              '| Segment | Source lines | Duration ms | Coverage | Issues | Evidence gaps |',
              '|---|---|---:|---|---|---|']
    for segment in report['segments']:
        lines.append(f"| {segment['segment']} | {segment['first_line']}–{segment['last_line']} | "
                     f"{segment['duration_ms']:.3f} | {segment['coverage']} | "
                     f"{', '.join(segment['issues']) or 'none observed'} | "
                     f"{', '.join(segment['evidence_gaps']) or 'none'} |")
    lines += ['', '## Recovery and settled phase', '',
              'Times start at the first render submission in each segment, not the resume key. '
              'Settling requires fresh phase samples within tolerance for the configured duration; '
              'recovery time records the first qualifying run. Later drift or observation gaps '
              'invalidate the current settled status; JSON also records the final stable run. '
              'Short interrupted windows retain an incomplete status.', '',
              '| Segment | Initial phase ms | Recovery ms | Settled median ms | Status |',
              '|---|---|---|---|---|']
    for segment in report['segments']:
        recovery = segment['recovery']
        values = [recovery.get(k) for k in ('initial_phase_ms', 'recovery_ms', 'settled_phase_median_ms')]
        cells = ['unavailable' if v is None else f'{v:.3f}' for v in values]
        lines.append(f"| {segment['segment']} | {' | '.join(cells)} | {recovery['status']} |")
    lines += ['', '## Timing findings', '',
              'Threshold crossings use the configured event budgets. Resume-boundary adjustments '
              'and identified queued seek previews are shown separately from playback judder. '
              'Preview lateness retains the seek startup limit; ordinary submissions retain '
              'the frame-interval limit.', '',
              '| Segment | Source lines | Finding | Measurements |', '|---|---|---|---|']
    for segment in report['segments']:
        for finding in segment['findings']:
            values = ', '.join(f'{key}={value:.3f}' for key, value in finding.items()
                               if key not in ('kind', 'first_line', 'last_line'))
            lines.append(f"| {segment['segment']} | {finding['first_line']}–{finding['last_line']} | "
                         f"{finding['kind']} | {values} |")
    lines += ['', '## Transition diagnostics', '',
              'Original anchor/correction records retain source lines; unclocked completion '
              'messages are not assigned inferred timestamps.', '']
    lines += [f"- Line {r['line']}: `{cell(r['message'])}`" for r in report['transition_records']]
    lines += ['', 'Fault lines: ' + cell(report['fault_lines']),
              'Context change lines: ' + cell(report['context_change_lines']),
              'Malformed record lines: ' + (', '.join(map(str, report['malformed_record_lines'])) or 'none'),
              '', '## Resolved thresholds', '', '```json',
              json.dumps(report['config'], indent=2, sort_keys=True), '```', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--format', choices=('json', 'markdown'), default='json',
                        help='output format (default: json)')
    parser.add_argument('--config', type=Path, help='replay a captured analyzer-config.json')
    args = parser.parse_args()
    try:
        saved = json.loads(args.config.read_text()) if args.config else None
        if args.config and not isinstance(saved, dict):
            raise ValueError('configuration must be a JSON object')
        cfg = config(saved)
        report = analyze_recording(args.log.read_text(errors='replace'), cfg)
        report['source'] = str(args.log)
    except (TypeError, ValueError, OSError) as error:
        parser.error(str(error))
    print(markdown_report(report, args.log) if args.format == 'markdown' else
          json.dumps(report, indent=2, sort_keys=True))
    return 0 if report['verdict'] == 'NO_ISSUES_OBSERVED' else 1


if __name__ == '__main__':
    raise SystemExit(main())

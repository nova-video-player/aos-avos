#!/usr/bin/env python3
"""Optional compositor evidence; never changes playback or stress verdicts."""
import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import re
import shlex
import signal
import statistics
import subprocess
import threading
import time

from analyze_stress import SeekPreview

PENDING = (1 << 63) - 1
FIELDS = re.compile(r'(\w+)=([^\s]+)')
BOUNDARY = re.compile(r'WALLCLOCK_RESET: by pause resume|VIDEO_SEEK_TARGET_READY:|'
                      r'stream_open:|stream_stop:|android_sync: pause start|_stream_play_n_frames\(n=')


def parse_latency(text):
    """AOSP FrameTracker: refresh period, then desired/actual/ready in ns."""
    lines = text.splitlines()
    counts = Counter()
    try:
        refresh = int(lines[0])
        if not 0 < refresh < 1_000_000_000:
            raise ValueError('invalid refresh period')
    except (IndexError, ValueError):
        return None, [], {'invalid_header': 1}
    rows = []
    for index, line in enumerate(lines[1:]):
        if not line.strip():
            continue
        try:
            values = tuple(map(int, line.split()))
            if len(values) != 3:
                raise ValueError('expected three timestamps')
        except ValueError:
            counts['malformed'] += 1
            continue
        if any(value >= PENDING for value in values):
            counts['pending'] += 1
        elif any(value <= 0 for value in values):
            counts['zero_or_invalid'] += 1
        else:
            rows.append((index, *values))
    return refresh, rows, dict(counts)


def select_layer(listing, package, override=None):
    names = listing.splitlines()
    if override:
        return override if override in names else None
    # Do not accidentally measure the UI, background or a container layer.
    candidates = [name for name in names if package + '/' in name and
                  (name.startswith('SurfaceView') or name.startswith('SurfaceView['))]
    # Android's BLAST hierarchy exposes both the SurfaceView container and its
    # buffer layer. Resolve only a single, exact parent/child naming pair; two
    # unrelated video surfaces or overlapping recreated layers remain ambiguous.
    if len(candidates) == 2:
        for parent in candidates:
            match = re.fullmatch(r'(SurfaceView\[.+\])#\d+', parent)
            if match:
                children = [name for name in candidates if re.fullmatch(
                    re.escape(match[1]) + r'\(BLAST\)#\d+', name)]
                if len(children) == 1:
                    return children[0]
    return candidates[0] if len(candidates) == 1 else None


def adb_shell(*args):
    started = time.monotonic()
    try:
        result = subprocess.run(['adb', 'shell', shlex.join(args)], capture_output=True,
                                text=True, timeout=3)
        return dict(stdout=result.stdout, stderr=result.stderr, rc=result.returncode,
                    duration_ms=round((time.monotonic() - started) * 1000, 3))
    except (OSError, subprocess.SubprocessError) as error:
        return dict(stdout='', stderr=str(error), rc=-1,
                    duration_ms=round((time.monotonic() - started) * 1000, 3))


def collect(output, package, interval, layer=None, duration=None):
    stopped = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stopped.set())
    started = time.monotonic()
    previous_layer = None
    generation = 0
    output.parent.mkdir(parents=True, exist_ok=True)
    # Never overwrite an existing capture on accidental restart.
    with output.open('x') as handle:
        while True:
            final_snapshot = stopped.is_set()
            now = time.monotonic()
            listing = adb_shell('dumpsys', 'SurfaceFlinger', '--list')
            selected = select_layer(listing['stdout'], package, layer) if listing['rc'] == 0 else None
            if selected != previous_layer:
                generation += 1
            previous_layer = selected
            capture = adb_shell('dumpsys', 'SurfaceFlinger', '--latency', selected) if selected else None
            handle.write(json.dumps(dict(host_monotonic_ns=int(now * 1e9),
                                         host_wall_ns=time.time_ns(), generation=generation,
                                         layer=selected, requested_layer=layer, interval_seconds=interval,
                                         listing=listing, capture=capture)) + '\n')
            handle.flush()
            if final_snapshot or (duration is not None and time.monotonic() - started >= duration):
                break
            stopped.wait(max(0, interval - (time.monotonic() - now)))


def stats(values):
    values = sorted(values)
    if not values:
        return None
    return dict(count=len(values), min=round(values[0], 3),
                median=round(statistics.median(values), 3),
                p95=round(values[math.ceil(.95 * len(values)) - 1], 3),
                max=round(values[-1], 3))


def render_records(text):
    records = defaultdict(list)
    segment = 0
    boundary = 'initial'
    preview = SeekPreview()
    paused = False
    backend = None
    previous_interval = None
    for line_no, line in enumerate(text.splitlines(), 1):
        fields = dict(FIELDS.findall(line))
        preview.annotate(line, fields)
        if 'stream_open:' in line or 'stream_stop:' in line:
            backend = previous_interval = None
            paused = False
        if 'android_sync: pause start' in line or '_stream_play_n_frames(n=' in line:
            paused = True
        if 'WALLCLOCK_RESET: by pause resume' in line or 'VIDEO_SEEK_TARGET_READY:' in line:
            paused = False
        # This message is emitted for every filtered PCM buffer, not only
        # speed changes. Split on an actual backend or frame-interval change.
        applied = re.search(r'applying speed filter \[([^]]+)\]', line)
        backend_changed = applied and backend is not None and applied[1] != backend
        if applied:
            backend = applied[1]
        if BOUNDARY.search(line) or backend_changed:
            segment += 1
            boundary = line.strip()
        if 'video_render_diag:' not in line:
            continue
        try:
            record = dict(deadline_ns=int(fields['deadline_ns']), epoch=int(fields['epoch']),
                          render_seq=int(fields['render_seq']), frame=int(fields['frame']),
                          interval_ms=float(fields['interval_ms']), phase_ms=float(fields['phase_ms']),
                          anchor_age_ms=float(fields['anchor_age_ms']), segment=segment,
                          boundary=boundary, source_line=line_no,
                          paused_or_preview=paused or bool(fields.get('_seek_preview')))
            if not all(math.isfinite(record[key]) for key in ('interval_ms', 'phase_ms', 'anchor_age_ms')):
                continue
        except (KeyError, ValueError):
            continue
        if previous_interval is not None and abs(record['interval_ms'] - previous_interval) > .001:
            segment += 1
            boundary = 'render interval changed'
            record.update(segment=segment, boundary=boundary)
        previous_interval = record['interval_ms']
        records[record['deadline_ns']].append(record)
    # Repeated/colliding deadlines are ambiguous, including across player restarts.
    return records


def analyze(captures, log_text):
    renders = render_records(log_text)
    frames = {}
    pairs = set()
    counts = Counter()
    query_times = []
    snapshots = []
    refresh_periods = set()
    previous_keys = set()
    previous_generation = None
    for sample, capture in enumerate(captures):
        duration = sum((capture.get(key) or {}).get('duration_ms', 0) for key in ('listing', 'capture'))
        query_times.append(duration)
        result = capture.get('capture')
        if not result or result.get('rc') != 0:
            counts['unavailable_snapshots'] += 1
            snapshots.append(dict(sample=sample, status='unavailable', query_ms=duration))
            continue
        refresh, rows, invalid = parse_latency(result['stdout'])
        counts.update(invalid)
        if refresh:
            refresh_periods.add(refresh)
        generation = (capture['generation'], capture['layer'])
        keys = set()
        previous = None
        for index, desired, actual, ready in rows:
            key = (*generation, desired, actual, ready)
            keys.add(key)
            frames.setdefault(key, dict(desired_ns=desired, actual_ns=actual, ready_ns=ready,
                                        refresh_ns=refresh, generation=capture['generation'],
                                        layer=capture['layer'], first_snapshot=sample))
            if previous and index == previous[0] + 1:
                pairs.add((previous[1], key))
            previous = (index, key)
        overlap = len(keys & previous_keys) if generation == previous_generation else 0
        snapshots.append(dict(sample=sample, status='data' if rows else 'no_completed_frames',
                              completed_rows=len(rows), new_rows=len(keys - previous_keys),
                              overlap_rows=overlap, query_ms=duration,
                              unchanged_history=bool(keys and keys == previous_keys and generation == previous_generation),
                              history_overlap_missing=bool(keys and previous_keys and
                                                           generation == previous_generation and not overlap)))
        previous_keys, previous_generation = keys, generation

    # One desired timestamp must identify one presentation and one submission.
    by_desired = Counter(frame['desired_ns'] for frame in frames.values())
    matched = {}
    for key, frame in frames.items():
        candidates = renders.get(frame['desired_ns'], [])
        if len(candidates) != 1 or by_desired[frame['desired_ns']] != 1:
            counts['ambiguous_frames' if candidates else 'unmatched_frames'] += 1
            continue
        match = dict(frame, **{k: v for k, v in candidates[0].items() if k != 'deadline_ns'})
        match['present_delay_ms'] = (frame['actual_ns'] - frame['desired_ns']) / 1e6
        matched[key] = match

    cadence = []
    for before, after in sorted(pairs):
        a, b = matched.get(before), matched.get(after)
        if (a and a['paused_or_preview']) or (b and b['paused_or_preview']):
            continue
        if not a or not b or (a['epoch'], a['segment'], a['generation'], a['refresh_ns']) != (
                b['epoch'], b['segment'], b['generation'], b['refresh_ns']):
            continue
        if b['render_seq'] != a['render_seq'] + 1:
            continue
        desired = (b['desired_ns'] - a['desired_ns']) / 1e6
        actual = (b['actual_ns'] - a['actual_ns']) / 1e6
        refresh = b['refresh_ns'] / 1e6
        if desired <= 0:
            continue
        # Cadence quantization: e.g. 23.976fps on 59.94Hz alternates 2/3 refreshes.
        # Use scheduled spacing, so an AVOS deadline jump is not blamed on SF.
        ticks = desired / refresh
        low, high = math.floor(ticks + .001), math.ceil(ticks - .001)
        error = min(abs(actual - low * refresh), abs(actual - high * refresh))
        cadence.append(dict(source_line=b['source_line'], epoch=b['epoch'], segment=b['segment'],
                            desired_interval_ms=round(desired, 3), actual_interval_ms=round(actual, 3),
                            refresh_quantization_error_ms=round(error, 3),
                            review=actual <= 0 or error > 2))

    ordered = sorted(matched.values(), key=lambda f: f['source_line'])
    groups = []
    for frame in ordered:
        key = (frame['generation'], frame['epoch'], frame['segment'], frame['refresh_ns'])
        if not groups or groups[-1]['key'] != key:
            groups.append(dict(key=key, frames=[]))
        groups[-1]['frames'].append(frame)
    # Fixed initial observation, never re-zero after a seek/resume or speed ramp.
    # This is a compositor baseline; it cannot calibrate the downstream audio route.
    baseline = None
    if groups and groups[0]['frames'][0]['segment'] == 0:
        initial = groups[0]['frames']
        first = initial[0]['desired_ns']
        window = [f for f in initial if f['desired_ns'] - first <= 1_000_000_000]
        continuous = all(b['render_seq'] == a['render_seq'] + 1 for a, b in zip(window, window[1:]))
        if (len(window) >= 3 and window[-1]['desired_ns'] - first >= 250_000_000 and continuous
                and not any(f['paused_or_preview'] for f in window)):
            baseline = dict(present_delay_median_ms=statistics.median(f['present_delay_ms'] for f in window),
                            layer=window[0]['layer'],
                            refresh_ns=window[0]['refresh_ns'], generation=window[0]['generation'],
                            first_line=window[0]['source_line'], last_line=window[-1]['source_line'])
    segments = []
    for group in groups:
        rows = group['frames']
        delays = stats([f['present_delay_ms'] for f in rows])
        fresh = [f['phase_ms'] for f in rows
                 if not f['paused_or_preview'] and 0 <= f['anchor_age_ms'] <= 100]
        comparable = baseline and baseline['refresh_ns'] == rows[0]['refresh_ns'] and baseline['generation'] == rows[0]['generation']
        segments.append(dict(epoch=rows[0]['epoch'], segment=rows[0]['segment'],
                             generation=rows[0]['generation'], layer=rows[0]['layer'],
                             refresh_ns=rows[0]['refresh_ns'], boundary=rows[0]['boundary'],
                             first_line=rows[0]['source_line'], last_line=rows[-1]['source_line'],
                             present_delay_ms=delays, scheduled_phase_estimate_ms=stats(fresh),
                             baseline_delay_change_ms=round(delays['median'] - baseline['present_delay_median_ms'], 3)
                             if comparable else None))
    count = len(matched)
    return dict(schema_version=1, status='CORRELATED' if count else 'UNCORRELATED' if frames else 'UNAVAILABLE',
                physical_lipsync='unmeasured', verdict_effect='none', exact_matches=count,
                render_records=sum(len(v) for v in renders.values()), unique_presentations=len(frames),
                counts=dict(counts), refresh_periods_ns=sorted(refresh_periods),
                query_duration_ms=stats(query_times), snapshots=snapshots,
                baseline=baseline, segments=segments, cadence=cadence,
                cadence_review_count=sum(row['review'] for row in cadence), matches=ordered)


def markdown(report):
    lines = ['# SurfaceFlinger presentation evidence', '',
             f'Status: **{report["status"]}**. Diagnostic only; stress verdict unchanged.', '',
             f'Exact matches: {report["exact_matches"]} / {report["render_records"]} AVOS render records; '
             f'{report["unique_presentations"]} unique completed compositor records.', '',
             f'Capture query duration (ms): `{report["query_duration_ms"]}`.', '',
             f'Cadence pairs requiring review: {report["cadence_review_count"]}. '
             'Expected floor/ceiling refresh intervals are allowed, with 2 ms measurement tolerance. '
             'These are review candidates, not automatic stutter failures.', '',
             f'Fixed initial compositor baseline: `{report["baseline"]}`.', '',
             '| Epoch / segment | Render log lines | Present delay median / max (ms) | Change from baseline (ms) | Scheduled phase estimate median (ms) |',
             '| --- | --- | --- | --- | --- |']
    for segment in report['segments']:
        phase = segment['scheduled_phase_estimate_ms']
        delay = segment['present_delay_ms']
        lines.append(f'| {segment["epoch"]} / {segment["segment"]} | '
                     f'{segment["first_line"]}–{segment["last_line"]} | {delay["median"]} / {delay["max"]} | '
                     f'{segment["baseline_delay_change_ms"]} | {phase["median"] if phase else "unavailable"} |')
    lines += ['', 'Presentation delay = compositor actual timestamp minus AVOS scheduled deadline. '
              'Only unique exact deadline matches are correlated; timestamps are device monotonic ns, '
              'not logcat wall time or host time. Phase stays an independent AVOS audio-clock estimate. '
              'Do not add/subtract these columns to calibrate physical lipsync, particularly during speed changes.', '',
              'The baseline is the first continuous 250–1000 ms of matched initial playback, if available; '
              'it is never replaced by post-transition samples. Compare only the same layer generation and '
              'refresh period. Inspect JSON snapshots for missing history overlap, repeated/stale windows, '
              'invalid/pending timestamps and query overhead. No matches or missing coverage is not a pass.', '',
              'This observes the compositor, not photons or heard audio. HDMI/TV/AVR/soundbar buffering '
              'and DSP remain unmeasured. Raw capture: surfaceflinger-samples.jsonl. Per-frame matches and '
              'cadence candidates, with render log line references, are in surfaceflinger-report.json.', '']
    return '\n'.join(lines)


def write_report(samples, log):
    captures = [json.loads(line) for line in samples.read_text().splitlines() if line.strip()]
    report = analyze(captures, log.read_text(errors='replace'))
    report['source_log'] = str(log)
    samples.with_name('surfaceflinger-report.json').write_text(json.dumps(report, indent=2) + '\n')
    samples.with_name('surfaceflinger-report.md').write_text(markdown(report))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    capture = sub.add_parser('collect')
    capture.add_argument('--output', type=Path, required=True)
    capture.add_argument('--package', default='org.courville.nova')
    capture.add_argument('--layer', help='exact layer override when automatic selection is ambiguous')
    capture.add_argument('--interval', type=float, default=2)
    capture.add_argument('--duration', type=float, help='optional bounded feasibility capture in seconds')
    report = sub.add_parser('report')
    report.add_argument('--samples', type=Path, required=True)
    report.add_argument('--log', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'collect':
        if not math.isfinite(args.interval) or args.interval < .5:
            parser.error('interval must be finite and at least 0.5 seconds')
        if args.duration is not None and (not math.isfinite(args.duration) or args.duration <= 0):
            parser.error('duration must be positive and finite')
        collect(args.output, args.package, args.interval, args.layer, args.duration)
    else:
        result = write_report(args.samples, args.log)
        print(f'SurfaceFlinger: {result["status"]}; {result["exact_matches"]} exact matches (diagnostic only)')


if __name__ == '__main__':
    main()

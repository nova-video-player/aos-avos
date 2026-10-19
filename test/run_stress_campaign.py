#!/usr/bin/env python3
"""Run the device campaign and generate JSON/Markdown recording reviews."""
import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import subprocess
import sys

from analyze_stress import config
from analyze_stress_recording import analyze_recording, markdown_report
from campaign_startup import capture_startup, positive, video_uri


def generate_reports(output, campaign_rc, settings_relative='resume-1/analyzer-config.json',
                     *, unavailable_reason=None):
    log = output / 'logcat-session.log'
    settings = output / settings_relative
    report_error = None
    recording_verdict = 'UNAVAILABLE'
    try:
        if unavailable_reason:
            raise ValueError(unavailable_reason)
        saved = json.loads(settings.read_text())
        if not isinstance(saved, dict):
            raise ValueError('saved analyzer configuration must be a JSON object')
        # Keep the first preflight reference: never recalibrate against the
        # possibly drifted end of the campaign or silently use default settings.
        report = analyze_recording(log.read_text(errors='replace'), config(saved))
        report['source'] = str(log)
        (output / 'recording-review.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
        (output / 'recording-review.md').write_text(markdown_report(report, log))
        recording_verdict = report['verdict']
    except (OSError, ValueError, TypeError, KeyError) as error:
        report_error = str(error)
        (output / 'recording-review.md').write_text(
            '# Recording review unavailable\n\n' + report_error +
            ('\n' if unavailable_reason else
             f'\n\nThe session log and original {settings_relative} are required.\n'))

    if campaign_rc >= 128:
        status, result = 'INTERRUPTED', campaign_rc
    elif campaign_rc not in (0, 1, 3) or report_error:
        status, result = 'ERROR', 2
    elif campaign_rc == 1 or recording_verdict == 'ISSUES_OBSERVED':
        status, result = 'FAIL', 1
    elif campaign_rc == 3 or recording_verdict != 'NO_ISSUES_OBSERVED':
        status, result = 'INSUFFICIENT_EVIDENCE', 1
    else:
        status, result = 'PASS', 0

    combined = dict(verdict=status, campaign_exit_code=campaign_rc,
                    recording_verdict=recording_verdict, report_error=report_error,
                    physical_lipsync='unmeasured', exit_code=result)
    startup_path = output / 'startup/startup-report.json'
    if startup_path.exists():
        combined['startup'] = json.loads(startup_path.read_text())
    # Optional compositor evidence is deliberately independent of the established
    # playback verdict. The shared driver generates reports on success or failure.
    compositor = []
    compositor_baseline = None
    sf_phases = {path.parent for pattern in ('*/surfaceflinger-samples.jsonl', '*/surfaceflinger-collector.log',
                                             '*/surfaceflinger-analysis.log') for path in output.glob(pattern)}
    for phase_dir in sorted(sf_phases):
        if phase_dir.name == 'startup':
            # Startup has its own evidence report. Layer generations are local
            # to each collector; do not use it as a cross-phase sync baseline.
            continue
        samples = phase_dir / 'surfaceflinger-samples.jsonl'
        report_path = samples.with_name('surfaceflinger-report.json')
        try:
            sf = json.loads(report_path.read_text())
            if not compositor:
                compositor_baseline = sf.get('baseline')
            comparison = []
            for segment in sf['segments']:
                comparable = compositor_baseline and all(
                    segment.get(key) == compositor_baseline.get(key)
                    for key in ('layer', 'refresh_ns', 'generation'))
                comparison.append(dict(epoch=segment['epoch'], segment=segment['segment'],
                                       first_line=segment['first_line'], last_line=segment['last_line'],
                                       baseline_delay_change_ms=round(segment['present_delay_ms']['median'] -
                                           compositor_baseline['present_delay_median_ms'], 3) if comparable else None))
            compositor.append(dict(phase=samples.parent.name, status=sf['status'],
                                   exact_matches=sf['exact_matches'], report=str(report_path),
                                   initial_session_baseline_comparison=comparison))
        except (OSError, ValueError, KeyError, TypeError):
            compositor.append(dict(phase=samples.parent.name, status='UNAVAILABLE', report=str(report_path)))
    combined['surfaceflinger'] = compositor
    combined['surfaceflinger_initial_baseline'] = compositor_baseline
    (output / 'run-report.json').write_text(json.dumps(combined, indent=2, sort_keys=True) + '\n')
    print(f'\nCombined verdict: {status}')
    print(f'Campaign exit code: {campaign_rc}; recording review: {recording_verdict}')
    if report_error:
        print(f'Report error: {report_error}', file=sys.stderr)
    print(f'Campaign summary: {output / "summary.md"}')
    print(f'Recording review: {output / "recording-review.md"}')
    print(f'Combined result: {output / "run-report.json"}')
    if 'startup' in combined:
        print(f'Startup capture: {combined["startup"]["status"]}; {startup_path}')
    for sf in compositor:
        print(f'SurfaceFlinger {sf["phase"]}: {sf["status"]}; {sf["report"]}')
    print('Physical lipsync and panel cadence remain unmeasured.')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, epilog=(
        'Accepts the same environment controls as stress_campaign.sh, including '
        'BURST_PAIRS, BURST_GAP_MS, ROUNDS, SEEK_CYCLES and SPEED_CYCLES. '
        'Start playback before running, or use --video to capture a fresh launch.'))
    parser.add_argument('rounds', nargs='?', help='override ROUNDS')
    parser.add_argument('--video', type=video_uri, help='launch URI or absolute device path before the campaign')
    parser.add_argument('--serial', help='override ANDROID_SERIAL')
    parser.add_argument('--package', help='override PACKAGE (default org.courville.nova)')
    parser.add_argument('--startup-timeout', type=positive, default=60, help='startup evidence timeout in seconds')
    parser.add_argument('--startup-hold', type=positive, default=5, help='startup baseline capture duration in seconds')
    parser.add_argument('--output-dir', type=Path, help='override OUTPUT_DIR; must be absent or empty')
    parser.add_argument('--dry-run', action='store_true', help='print the plan without contacting adb')
    args = parser.parse_args()
    script_dir = Path(__file__).resolve().parent
    env = dict(os.environ)
    if args.serial:
        env['ANDROID_SERIAL'] = args.serial
    if args.package:
        env['PACKAGE'] = args.package
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    output = (args.output_dir or Path(env.get('OUTPUT_DIR') or
              str(script_dir.parent / f'campaign-session-{stamp}'))).resolve()
    # A continuous capture is required for the post-campaign review.
    if env.get('SESSION_LOGCAT', '1') != '1':
        parser.error('SESSION_LOGCAT must be 1 to generate the recording review')
    env['SESSION_LOGCAT'] = '1'
    env['OUTPUT_DIR'] = str(output)
    if args.dry_run:
        env['DRY_RUN'] = '1'
    command = ['bash', str(script_dir / 'stress_campaign.sh')]
    if args.rounds is not None:
        command.append(args.rounds)
    if env.get('DRY_RUN', '0') == '1':
        if args.video:
            print(f'Startup capture: launch {args.video}; preserve playback preferences; '
                  f'wait up to {args.startup_timeout}s, then capture {args.startup_hold}s.', flush=True)
        result = subprocess.run(command, env=env).returncode
        print(f'Recording reports would be written to: {output}', flush=True)
        print(f'Resume burst pairs: {env.get("BURST_PAIRS", "0")}; '
              f'added key gap: {env.get("BURST_GAP_MS", "0")} ms')
        return result if result >= 0 else 128 - result
    try:
        if output.exists() and (not output.is_dir() or any(output.iterdir())):
            parser.error('output directory must be absent or empty; previous evidence is preserved')
        output.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        parser.error(str(error))
    print(f'Campaign and recording reports: {output}', flush=True)
    unavailable_reason = None
    try:
        if args.video:
            print('Capturing startup before the resume/seek campaign.', flush=True)
            capture_startup(output, args.video, env.get('PACKAGE', 'org.courville.nova'),
                            env, args.startup_timeout, args.startup_hold)
        campaign_rc = subprocess.run(command, env=env).returncode
        if campaign_rc < 0:
            campaign_rc = 128 - campaign_rc
    except KeyboardInterrupt:
        campaign_rc = 130
        if not (output / 'resume-1/analyzer-config.json').exists():
            unavailable_reason = 'Interrupted before campaign preflight completed; see startup/.'
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        campaign_rc = 2
        unavailable_reason = f'Campaign did not start: {error}'
    if unavailable_reason:
        (output / 'summary.md').write_text('# Campaign setup incomplete\n\n' + unavailable_reason + '\n')
    # Deliberately report failed/partial runs as well as successful campaigns.
    try:
        return generate_reports(output, campaign_rc, unavailable_reason=unavailable_reason)
    except OSError as error:
        print(f'Could not write reports: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())

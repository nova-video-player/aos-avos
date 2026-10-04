# AVOS playback validation

The device harnesses drive playback that is already running. They share
`test/analyze_stress.py` for recovery, stutter indicators and internal A/V phase
checks. A PASS applies to the captured evidence and configured thresholds.
**Physical lipsync and displayed-frame cadence remain unmeasured.**

## Review a saved log (no device required)

Generate a readable report and retain JSON for comparison:

```bash
python3 test/analyze_stress_recording.py avos-21.log --format markdown > /tmp/avos-21-review.md
python3 test/analyze_stress_recording.py avos-21.log > /tmp/avos-21-review.json
```

Both commands write a complete report even when they exit **1** for observed
issues or insufficient evidence. Exit **2** indicates invalid arguments,
configuration or an unreadable file. The default format remains JSON.

For logs captured by a device harness, replay its saved thresholds and reference:

```bash
python3 test/analyze_stress_recording.py path/to/phase/logcat.log \
  --config path/to/phase/analyzer-config.json --format markdown > /tmp/playback-review.md
```

`--config` uses the saved configuration instead of environment overrides. Use
the configuration from that capture: copying another route's phase reference
can hide or invent a lipsync indication. Without `--config`, settings come from
the environment and defaults below. For mode 1, the recording analyzer uses the
first eligible fresh phase window as a relative reference and labels it
`recording_initial`. Like live preflight, it requires at least three samples
with a spread no greater than 80ms. An unstable first eligible window makes the
reference unavailable; a later, potentially drifted window cannot replace it.

### Reading the report

1. Check **verdict, coverage and phase reference** first. `ISSUES_OBSERVED`
   means a recorded fault was found; `INSUFFICIENT_EVIDENCE` means the required
   observations are missing. `NO_ISSUES_OBSERVED` applies only to the measured
   scope and is not proof of physical lipsync or uninterrupted playback.
2. In **Timing findings**, review the exact source lines:
   - `scheduled_judder`: compare the actual deadline interval with the expected
     frame interval and error allowance for consecutive submissions. For example,
     126.7ms instead of 41.7ms is an 85ms scheduling disturbance when adjacent
     `render_seq` values establish that no render records were omitted.
   - `missing_render_records`: increasing `render_seq` values have a gap. The
     omitted submissions succeeded, but their timing records are unavailable.
   - `missing_render_continuity`: a legacy recording has a media-timestamp gap
     without sequence numbers to distinguish omitted logs from skipped frames.
     Both missing-record findings prevent a clean verdict, including in short
     segments. They are evidence gaps, not measured frame-spacing errors.
   - `late_submission`: a frame reached the renderer after its deadline by
     more than the configured limit. Check the surrounding seek/resume records.
   - `resume_boundary_adjustment`: the pair straddles the renderer's resume
     update. Subtract the explicitly logged pause shift and signed correction
     before judging cadence. An unexplained residual still counts as judder;
     a resume marker alone does not excuse an arbitrary deadline jump.
   - `sustained_av_phase_error`: the deviation from the recorded phase reference
     persisted beyond the duration budget. This is an internal lipsync
     indication; check raw phase extrema in JSON and the manual-delay setting.
3. Check **Recorded signals** for drops, starvation and underrun increases.
   Zero counts alone are insufficient: inspect coverage and evidence gaps.
   Short segments can show real faults but cannot establish sustained health.
   Full, partial and zero writes are counted separately, alongside accepted
   bytes and compressed continuation records. Matching partial/continuation
   counts do not prove every compressed unit completed: these logs have no
   transaction IDs. Presentation evidence remains a separate check.
4. Inspect surrounding source lines, for example:

   ```bash
   sed -n '96340,96680p' avos-21.log
   ```

5. Share the original log, Markdown/JSON report, and capture configuration,
   together with the build revision, device/audio route, clip frame rate,
   manual A/V delay, and actions that reproduced the symptom. Note whether the
   symptom was seen/heard or only detected in the log.

Finding rows list individual threshold crossings even when an explicit event
budget allows them; segment issues and the overall verdict apply those budgets.
The report includes every segment, including short windows and excluded-frame
counts, so a good interval cannot conceal a fault elsewhere in the recording.
Offline and live reports apply the same drop, starvation and underrun budgets.
Both detect runtime errors, compressed write failures and known capture loss.
Offline capture loss or malformed records makes timing coverage insufficient;
independently observed faults remain reported.

**Recovery and settled phase** reports the initial phase/deviation and the
start of the first qualifying run within `RECOVERY_PHASE_MAX_MS` of the
reference. That run must contain at least three fresh samples spanning
`RECOVERY_STABLE_MS`. Subsequent drift or an observation gap cancels the current
settled status; JSON separately records the start of the final stable run.
This distinguishes first recovery from later small excursions around the limit.
This tighter recovery criterion is separate from the sustained-error budget.
Times are measured from the segment's first render submission, not an
unclocked resume key or an inferred clock-completion time. Short interrupted
segments report recovery as unobserved. Raw anchor, correction and completion
messages retain their source lines under **Transition diagnostics**. Sustained
errors remain findings even if the segment eventually recovers.

Playback restarts, detected output-format/mode changes and player PID changes
invalidate comparisons with the preceding phase reference. Cadence analysis
continues, but the affected phase coverage is unavailable. Split such captures
and supply the matching reference/configuration for each playback context.
Unlogged route changes cannot be identified reliably.

## Requirements

- A debug APK containing `video_render_diag` and `audio_present_diag` records.
  Build/install using your normal workflow; the test scripts do neither.
- `adb`, Bash, Python 3 and host `grep --line-buffered`.
- One connected device, or `ANDROID_SERIAL=<serial>`.
- An actively playing video with audio. Use `dbgs=2`, `dbgsink=2`, `dbga=2`.
- Speed tests require decoded PCM, the selected speed backend enabled, and
  playback initially at 1.0x. MediaCodec audio decoding deliberately rejects
  variable speed; use FFmpeg audio decoding for the speed campaign.

The preflight requires live video records and, by default, render timing.
Missing evidence stops the test rather than silently reporting success.
Passthrough is supported by the resume/seek observers; speed testing rejects it.

## Commands

From the repository root:

To run the campaign and automatically generate both recording reports:

```bash
ROUNDS=2 RESUME_CYCLES=5 SEEK_CYCLES=10 \
BURST_PAIRS=10 BURST_GAP_MS=0 SESSION_RAW=1 \
python3 test/run_stress_campaign.py

# Preview without contacting the device:
BURST_PAIRS=10 python3 test/run_stress_campaign.py --dry-run
```

Start video playback first. The wrapper accepts the campaign's environment
controls and an optional rounds argument. Omit `BURST_PAIRS` for ordinary
pauses; add `SPEED_CYCLES=2` for a PCM speed phase. Speed testing requires the
appropriate decoder/settings described above and is disabled for passthrough.
`--output-dir PATH` overrides `OUTPUT_DIR`; the directory must be absent or
empty to preserve earlier evidence. Continuous log capture must remain enabled.

After the campaign, including a failed campaign, the wrapper writes
`recording-review.md`, `recording-review.json`, and the combined `run-report.json`
beside `summary.md`. It uses the first resume phase's saved configuration and
phase reference. Missing original configuration or logs produce an error;
the wrapper never substitutes a new baseline. Exit 0 requires both campaign
success and `NO_ISSUES_OBSERVED` in the recording review. Exit 1 indicates
playback findings or insufficient evidence; exit 2 indicates an infrastructure
or report error. Interruptions stay nonzero, with partial reports when possible.
The wrapper prints the report paths when finished. It records logs, not video
of the screen; physical lipsync remains unmeasured.

### Separate speed session with automatic setup

Build and install a **debug APK** containing Video's `SpeedTestReceiver` first.
The receiver is absent from release builds and accepts configuration broadcasts
only from callers with Android's `DUMP` permission (including adb shell).

```bash
python3 test/run_speed_session.py \
  --video /sdcard/Movies/test.mkv --backend atempo --cycles 5 --hold 8

python3 test/run_speed_session.py \
  --video 'smb://server/share/test.mkv' --backend sonic --cycles 5 --hold 8

python3 test/run_speed_session.py \
  --video /sdcard/Movies/test.mkv --backend audiotrack --cycles 5 --hold 8
```

`--video` accepts a device path or a URI Nova can already access. It does not
upload a file from the host or configure network credentials. `--serial` selects
an adb device; `--package` defaults to `org.courville.nova`. `--dry-run` prints
the plan without contacting adb or changing playback.

The launcher stops existing Nova playback, saves the speed-mode, decoder,
passthrough and saved-speed preferences, then selects FFmpeg PCM and 1.00x.
It enables `dbgs`, `dbgsink` and `dbga` at level 2 through AVSH before opening
the video from the beginning. A matching configuration acknowledgement and
native backend/filter, PCM writes, 1x and render records are required before
starting the driver. Each ramp runs twelve key events in one adb shell, reaching
1.60x from 1.00x in 0.05x steps; the default inter-key sleep is 20ms. `--hold`
applies at both 1.60x and 1.00x. Input processing adds its own latency.

The existing speed driver validates the filter actually used. AudioTrack runs
require successful hardware speed readback and no software speed-filter records.
The observed high-speed window must reach at least 1.59x and return to 1x.

Artifacts live in `speed-session-TIMESTAMP/`: continuous raw `logcat-session.log`,
`session.log`, `manifest.json`, `summary.md`, `speed-1/` per-cycle evidence,
`recording-review.md/json` and `run-report.json`. Reports use the saved
`speed-1/analyzer-config.json`, never an invented resume phase. Startup failures
and interrupted runs retain their evidence and cannot produce a PASS.

At completion, failure or Ctrl-C the launcher stops test playback and restores
the saved preferences. If the host is killed or adb disconnects, the on-device
backup remains; the recovery ID is printed and saved in `manifest.json`:

```bash
python3 test/run_speed_session.py --restore SESSION_ID
```

Use the same device/package for recovery. An outstanding backup blocks a new
configuration so the original settings cannot be overwritten. This launcher
does not run pause or seek phases. Physical lipsync remains unmeasured.

### Individual drivers

Individual drivers and the campaign without automatic report generation:

```bash
# Debug commands; the helper uses PACKAGE (default org.courville.nova).
test/av.sh dbgs 2
test/av.sh dbgsink 2
test/av.sh dbga 2

# count, recovery timeout seconds, minimum media/wall progress milliseconds
# Resume also accepts minimum and maximum pause milliseconds.
test/stress_resume_validate.sh 20 8 2000 500 3000
test/stress_seek_validate.sh 20 8 2000
test/stress_seek_dpad_validate.sh 20 8 2000
test/stress_seek_dpad_hold_validate.sh 10 8 2000

# cycles, high-speed hold seconds, delay between keys, return-to-1x settle seconds
LABEL=atempo REQUIRE_FILTER=atempo test/stress_speed_validate.sh 5 8 0.02 2
LABEL=sonic REQUIRE_FILTER=sonic test/stress_speed_validate.sh 5 8 0.02 2
LABEL=audiotrack REQUIRE_FILTER=audiotrack test/stress_speed_validate.sh 5 8 0.02 2

# Resume + seek; speed is optional and does not select/change the backend.
DRY_RUN=1 SPEED_CYCLES=1 test/stress_campaign.sh
ROUNDS=2 RESUME_CYCLES=20 SEEK_CYCLES=20 test/stress_campaign.sh
SPEED_CYCLES=2 SPEED_HOLD_SEC=8 test/stress_campaign.sh
```

The speed backend preference is resolved when playback starts. Start a new
playback after changing it in the app. The harness verifies observed speed;
it does not assume injected keys were accepted. `SPEED_STEPS` defaults to 10
(1.0x to 1.5x) and accepts 1–20. Lower targets require adjusting `HI_SPEED_MIN`.
Every high-speed window is finalized before measurement, including the last
window. The longest must meet `HOLD_MIN_MS`; shorter windows and ramp/return
failures cannot be hidden by an earlier successful window. Return to 1.0x is
required. `REQUIRE_FILTER` checks the filter records in the captured cycle.

The D-pad wrappers select `SEEK_DRIVER=dpad` or `dpad_hold`. Bursts/holds are
paired in opposite directions. Timed holds require device support for
`input keyevent --duration` and a minimum of 5000ms. Use sufficiently long media.

## Rapid pause/play bursts

Use the resume validator's optional burst mode to interrupt recovery repeatedly,
as when tapping the remote's OK button quickly. Start with video playing and
the play/pause control focused; OK can operate another control if focus moves.

```bash
# Five rounds, each with 10 pause/play pairs (20 OK presses), no added delay.
BURST_PAIRS=10 test/stress_resume_validate.sh 5 8 2000

# Add 150 ms between presses on the device.
BURST_PAIRS=10 BURST_GAP_MS=150 test/stress_resume_validate.sh 5 8 2000

# Use the media toggle to avoid dependence on OK-button focus.
BURST_PAIRS=10 BURST_KEY=KEYCODE_MEDIA_PLAY_PAUSE test/stress_resume_validate.sh 5 8 2000

# Print the device command without contacting adb or creating artifacts.
BURST_PAIRS=10 DRY_RUN=1 test/stress_resume_validate.sh
```

Each burst uses one `adb shell` invocation. With `BURST_GAP_MS=0` (default),
one `input keyevent` process sends all keys sequentially. A nonzero gap uses a
device-side loop; input-process overhead adds to the requested sleep. Neither
mode guarantees precise key spacing or bypasses Android/UI event handling.
If presses are ignored or coalesced, missing native transitions fail the check.

`BURST_PAIRS` accepts 1–100 (0 keeps ordinary pause testing), and
`BURST_GAP_MS` accepts 0–5000. The count argument means rounds; the ordinary
minimum/maximum pause arguments are unused in burst mode. Every burst has an
even number of toggles so it should finish playing. Recovery is checked after
the final actual resume, without treating intentional pauses as playback stalls.
The validator requires exactly the requested alternating native pause/resume
pairs, checks faults throughout the burst, and runs the recording analyzer over
the complete round before passing. Short interrupted windows remain identified
as incomplete; they cannot establish that every intermediate resume recovered.
The initial phase reference is retained across rounds to detect cumulative drift.
If live recovery passes before the recording has enough rendered-frame coverage,
the validator keeps sampling the same burst until the recording review passes or
the original timeout expires. It does not resend keys or restart the timeout.
Observed timing issues still fail immediately; unresolved evidence gaps fail at
the deadline with `full-burst evidence timeout` and the saved review JSON.

Artifacts include `burst-command.sh`, `burst-N-injection.txt`, and, once the
final recovery passes, `burst-N.log` and `burst-N-review.json`. Failures also
retain the usual failure snapshot and device diagnostics. An injection failure
leaves its command output in `burst-N-injection.txt`; inspect it before retrying.

## What is checked

| Evidence | Check | Limit |
|---|---|---|
| `audiotrack_write` | Positive writes, gaps including the observation tail | Buffered audio can conceal a feeder gap |
| `video_sched_diag` | Advancing media timestamps, epoch consistency, gross A/V divergence | Logged before final sink release; not presentation cadence |
| `video_render_diag` | Successful timed submissions, deadline spacing versus effective frame interval, submission lateness | Android can still display differently |
| `audio_present_diag` | Actual sampled counter progress, resets, frozen/stale evidence | Timestamp extrapolation is deliberately excluded; no downstream sound measurement |
| `mode1_iec_occupancy_shadow` / `mode2_occupancy_shadow` | Fresh direct-timestamp counter progress within one generation/epoch/source; underrun deltas | Counters use the route's carrier/logical units, not interchangeable PCM units |
| `AudioTrack underruns` | Positive reported deltas | Absence of records is not proof monitoring worked |
| `AUDIO_STARVED`, late-frame drops | Counts must stay within explicit budgets | Recorded events do not establish acoustic severity |
| `put_time_calc` | Clock evidence; resume reanchor budget | A reanchor is not automatically a visible glitch |

Audio presentation uses fresh explicit records, then the compressed observer,
then legacy playback-head diagnostics if neither is present. Counters from
separate sources/generations/epochs are never subtracted from each other.
Compressed observer records are emitted every 500ms, so their freshness budget
is separate from per-write PCM observations.
The live analyzer also checks gaps between observations, including the leading
and trailing gaps. Two advancing counters several seconds apart cannot prove
continuous audio presentation; such a gap is missing evidence, not proof of a
physical audio stall.

`video_render_diag.render_seq` counts successful timed MediaCodec submissions,
including when diagnostics are disabled. It starts with a new decoder instance
and persists through pause, seek and flush. A gap in a sequence within a playback
segment reports missing diagnostics; the analyzer does not divide the elapsed
deadline interval by the missing count and assume clean cadence. Mixed,
duplicated or reversed sequence records also leave continuity unverified. A
known seek/session boundary starts a separate comparison.

Legacy logs remain supported. When media timestamps jump by more than 1.5 times
the larger adjacent frame interval without sequence evidence, the comparison is
inconclusive. New consecutive sequence numbers still allow genuine skipped-frame
deadline gaps to be reported as judder. Late submissions and explicit drops remain
failures independently of missing records. Phase-duration and recovery runs do
not bridge a detected continuity gap.

Cadence uses **monotonic render deadlines**, not the arrival spacing of log
messages. Duplicate/reversed deadlines and departures from the effective frame
interval fail the configured budget. The two adjacent intervals are accepted
at a speed-change boundary. This assumes the reported frame rate is meaningful;
variable-frame-rate content needs a suitable tolerance or separate analysis.
Submission later than one frame interval (or the cadence tolerance, whichever
is larger) is reported as late even if the engine's 200ms drop limit is not hit.

A seek preview can be queued while paused and submitted after resume. The
analyzers match `SINK_REF_DEFERRED` frame timestamps and seek epochs inside an
explicit `_stream_play_n_frames` preview interval. That identity survives resume
until the preview is submitted or ordinary output begins. Such submissions are
reported as `queued_seek_preview`, with `seek_preview_count` and
`seek_preview_max_lateness_ms`, and use `SEEK_STARTUP_MAX_MS` for lateness.
They do not establish ordinary playback cadence, phase, coverage or first-render
recovery. Sequence gaps and subsequent late frames remain checked. Without the
identifying records, strict ordinary-frame checks remain in force; older filtered
captures may need replay from the raw session log. Both capture filters retain
`SINK_REF_DEFERRED` for new runs.

### Internal A/V phase versus physical lipsync

`video_render_diag.phase_ms` projects the last audio-owned anchor to the video
presentation deadline and removes the requested manual delay. Its sign is
video-media-time minus projected heard-audio-time. Negative means the scheduled
picture is behind that audio estimate. Only anchors aged 0–100ms are used.

The phase is checked after a bounded settling interval. A deviation must persist
for `AV_BAD_DURATION_MS`; a final good sample does not erase a sustained error.
This can detect clock drift and changes following seeks/resumes. It cannot
measure unknown AVR, soundbar, display or acoustic latency.

Mode 1 intentionally preserves a static-clock phase. Unless
`EXPECTED_PHASE_MS` is supplied, preflight takes the median of fresh render
phase samples, requires at least three samples with a spread no greater than
80ms, and records `phase_reference=preflight`. The campaign retains its first
reference across all phases/rounds so cumulative drift is visible. This is a
relative reference, **not calibration of absolute lipsync**. PCM and mode 2
default to zero. A known reference can be supplied explicitly, including negative
values. Reports retain both raw phase extrema and deviation from that reference.

To establish physical lipsync, use a synchronized external recording of a known
flash/beep or equivalent reference at the display/speakers. Measure paired light
and sound events on the same clock and account for capture delay. Internal phase,
`dumpsys gfxinfo`, and advancing AudioTrack counters alone cannot establish it.
Surface/display presentation traces can add evidence about actual video cadence;
these scripts do not currently collect or validate such traces.

## Settings

| Variable | Default | Meaning |
|---|---:|---|
| `REQUIRE_AUDIO_PRESENTATION` | 1 | Require fresh advancing audio observations |
| `REQUIRE_RENDER_TIMING` | 1 | Require rendered-schedule records and fresh phase evidence |
| `VIDEO_GAP_MAX_MS`, `WRITE_GAP_MAX_MS` | 250 | Feed/log gap limits, including a silent tail |
| `AV_DIFF_MAX_MS` | 1000 | Gross admission-time A/V bound |
| `EXPECTED_PHASE_MS` | 0 / mode-1 preflight | Reference for internal phase |
| `AV_PHASE_MAX_MS` | 80 | Maximum deviation from reference |
| `AV_BAD_DURATION_MS` | 250 | Sustained deviation duration |
| `AV_SETTLE_MS` | 500 | Initial phase settling interval |
| `RECOVERY_PHASE_MAX_MS` | 8 | Offline recovery proximity to the phase reference |
| `RECOVERY_STABLE_MS` | 250 | Fresh in-tolerance span needed to confirm offline recovery |
| `CADENCE_TOLERANCE_MS` | 8 | Scheduled interval error allowance |
| `CADENCE_BAD_MAX` | 0 | Allowed duplicate/reversed/irregular deadlines |
| `PRESENTATION_STALL_MS` | 500 | PCM presentation stall/freshness bound |
| `OBSERVER_FRESH_MS` | 750 | Compressed observer freshness/stall bound |
| `LATE_DROP_MAX`, `UNDERRUN_MAX`, `STARVED_MAX` | 0 | Event budgets |
| `REBASE_MAX` | 1 | Resume reanchor budget |
| `RESUME_LATENCY_MAX_MS` | 1000 | First resumed write/admitted-frame limit |
| `SEEK_STARTUP_MAX_MS` | 1000 | Target-ready to first write, scheduled frame and render log; separate from subsequent feed gaps |
| `HI_SPEED_MIN` | 1.45 | Minimum high-window speed |
| `HOLD_MIN_MS` | 60% of hold | Minimum high-window duration |
| `WINDOW_GAP_MS` | 1500 | Maximum clock-record gap within a high window |
| `ARM_UNDERRUN` | 0 | Enable writer-side underrun queries; can perturb timing |

Disabling a required evidence source explicitly reduces coverage. Its absence
remains visible in the results. Underrun coverage is reported as `unverified`,
`requested`, or `observer`; zero events without verified observation is not a
clean acoustic result. The observer's first cumulative total is a baseline,
not a new underrun; generation changes reset the counter comparison.

Other driver controls include `POLL_MS`, `MIN_PERCENT`/`MAX_PERCENT`,
`PAUSE_KEY`/`PLAY_KEY`, `CAPTURE_BUGREPORT`, `PACKAGE`, and `OUTPUT_DIR`.
Campaign controls include `ROUNDS`, `RESUME_CYCLES`, `SEEK_CYCLES`,
`SPEED_CYCLES` (default 0), `SPEED_HOLD_SEC`, `TIMEOUT_SEC`, `STABLE_MEDIA_MS`,
`MIN_PAUSE_MS`/`MAX_PAUSE_MS`, `SESSION_LOGCAT`, `SESSION_RAW`, and
`CONTINUE_ON_FAIL`. Speed backend selection and media selection remain manual.

## Capture and artifacts

Device logs use `logcat -v epoch`. The collectors request a 16MiB device buffer,
preserve stderr, retain required diagnostics, and do not clear the device ring
between phases. A device-timestamped `AVOS_TEST_POLL` marker ends each live
snapshot; both collector processes must be alive and the marker must arrive.
This bounds silent tails without comparing host and device clocks. Known log
loss records make the evidence fail; undetectable omissions remain a limitation.
Poll markers can overtake queued player logs; receiving one does not prove all
earlier player records have arrived. The analyzer checks player timestamps and
poll timestamps separately, reporting cross-stream skew as `poll_marker_skew_ms`.
It never sorts playback events or excuses reversals within either stream. The
observation ends at the latest captured timestamp, so an early poll cannot
shorten it. Stale presentation, silent tails and log-loss checks remain active;
later snapshots and the full-session review retain subsequently delivered logs.
Write gaps use outer logcat timestamps and remain producer-evidence checks:
logging delays can inflate them, while queued audio can keep playing through
them. Advancing presentation alone does not waive the configured write-gap
limit or establish the exact duration of a sink write.
Both capture filters retain Mode 1 published anchors, baselines, corrections
and completion messages, renderer pause/resume updates, preview boundaries,
output configuration changes, and compressed-write continuation/failure records.
The phase captures also retain PCM startup corrections, renderer anchor targets,
startup commits, heard-clock diagnostics and atempo ledger calibration. These
show whether a seek correction precedes a change of audio clock source.

Seek startup uses its own `SEEK_STARTUP_MAX_MS` budget because target readiness
precedes audio readiness and renderer scheduling. `first_write_ms`,
`first_video_ms` and `first_render_ms` report this wait using logcat timestamps;
they are not measurements of first audible/visible output. The 250ms feed-gap
limits still apply between subsequent records and through a silent tail.

Default directories are `<name>-validation-<stamp>/` and
`campaign-session-<stamp>/`, or `OUTPUT_DIR`:

- `logcat.log`: filtered evidence; `logcat-stderr.txt`: collector diagnostics.
- `current-*.log`: last immutable observation segment.
- `results.txt`: one PASS/FAIL record per cycle, with metrics/reasons.
- `analyzer-config.json`: resolved thresholds and phase reference for replay.
- On failure: the failed segment, relevant dumpsys output and a screenshot;
  optional full bugreport. The failed playback state is retained.
- Campaign: continuous log, context/manifest, phase artifacts, and
  `summary.md`, `summary.json`, `summary.tsv`, `metrics.tsv`, `session-signals.tsv`.

Campaign success requires the requested number of cycles to pass in every run
phase. Session-wide signal totals are context; intentional pause/seek intervals
are not judged as continuous playback. Exit codes: 0 success, 1 failed playback
checks, 2 usage/precondition/infrastructure failure. Interruptions are nonzero.

## Offline checks and regression tests

```bash
python3 test/test_analyze_stress.py

# One epoch-timestamped cycle, with its original threshold environment:
python3 test/analyze_stress.py resume path/to/current-resume.log
python3 test/analyze_stress.py seek path/to/current-seek.log
python3 test/analyze_stress.py speed path/to/current-cycle.log

# Saved recordings, including logcat brief without outer timestamps:
python3 test/analyze_stress_recording.py avos-21.log > /tmp/avos-21-report.json
```

The single-cycle analyzer prints `healthy=0|1`; exit 0 means parsing completed,
not that playback passed. Invalid evidence/configuration exits 2. The device
drivers consume `healthy` to decide the playback verdict.

The recording analyzer uses embedded `submit_ns` and `deadline_ns`. It splits
at lifecycle and epoch boundaries, excludes explicit pause/seek-preview output,
and reports short segments separately from adequate observations. Untimestamped
writes and resume markers never receive invented timestamps. Write-gap and
resume-latency checks are therefore reported as not evaluated. Its JSON includes
source line numbers, coverage, signals and the phase reference. Exit 0 means
`NO_ISSUES_OBSERVED`; issues or insufficient evidence return nonzero. This is
not a complete device-campaign PASS.
Explicit faults are reported even without adequate render coverage. Long
segments with missing or stale audio anchors cannot produce a clean verdict;
the JSON keeps timing coverage separate from observed faults.

`stress_pause.sh`, `stress_seek.sh` and `stress_speed.sh` remain unvalidated
chaos drivers; successful completion is not a playback verdict.

The separate native host harnesses are indexed in `AGENT.md`. `make test`
builds `ff`, `comp`, and `stream_url`; it does not run the Python or shell suites
or automatically execute those binaries. The historical
`test/pause_resume_sync.py` extracts a removed helper and currently fails; do not
count it as validation of current playback. The Sonic document also references
replacement suites absent from this checkout.

# AVOS playback validation

The device harnesses drive playback that is already running. They share
`test/analyze_stress.py` for recovery, stutter indicators and internal A/V phase
checks. A PASS applies to the captured evidence and configured thresholds.
**Physical lipsync and displayed-frame cadence remain unmeasured.**

## Optional compositor presentation capture

`SURFACEFLINGER_CAPTURE=1` adds read-only, periodic `dumpsys SurfaceFlinger
--latency` snapshots to the shared seek, resume and speed drivers. It also works
through `run_stress_campaign.py` and `run_speed_session.py`. It requires no new
APK and is disabled by default. Start playback as usual, then for example:

```bash
ANDROID_SERIAL=192.168.0.9:5555 SURFACEFLINGER_CAPTURE=1 \
  bash test/stress_seek_validate.sh 10 8 2000

ANDROID_SERIAL=192.168.0.9:5555 SURFACEFLINGER_CAPTURE=1 \
  CONTINUE_ON_FAIL=1 ROUNDS=2 RESUME_CYCLES=5 SEEK_CYCLES=10 \
  BURST_PAIRS=10 BURST_GAP_MS=150 SESSION_RAW=1 \
  python3 test/run_stress_campaign.py
```

Use the address of the device being tested. The collector inherits
`ANDROID_SERIAL`; it never changes playback or clears SurfaceFlinger history.
`SURFACEFLINGER_INTERVAL_SEC` defaults to 2 seconds (minimum 0.5).
The video layer is rediscovered each poll. A single exact `SurfaceView[...]#id`
and `SurfaceView[...](BLAST)#id` pair selects the BLAST buffer layer, not its
container. Other multiple candidates or no video layer are recorded as
unavailable. Pixel-style `RequestedLayerState{...}` listings are also supported:
the collector removes state metadata while preserving the exact layer name,
including any hexadecimal prefix and layer ID. For a vendor-specific name, set
`SURFACEFLINGER_LAYER` to the exact name from `adb shell dumpsys SurfaceFlinger
--list`. Do not select the app UI or background layer.

Each phase saves `surfaceflinger-samples.jsonl`, `surfaceflinger-collector.log`,
`surfaceflinger-report.json` and `surfaceflinger-report.md`. Cleanup captures a
final snapshot and generates the report for both successful and failed tests.
An unavailable/failed optional collector does not change the original test
exit code. The Python session wrapper lists these reports in `run-report.json`.
It also retains the first phase's compositor baseline for cross-phase comparison;
if that baseline is unavailable it does not substitute a later phase's baseline.

To regenerate a phase report offline:

```bash
python3 test/surfaceflinger_timing.py report \
  --samples path/to/phase/surfaceflinger-samples.jsonl \
  --log path/to/phase/logcat.log
```

This is a separate diagnostic signal, not an additional PASS/FAIL gate:

- Only unique exact matches between desired-present timestamps and
  `video_render_diag` deadlines establish correspondence. No nearest-frame guess.
- Zero, pending and malformed timestamps are excluded; overlapping history is
  deduplicated. Unchanged history and absent overlap are reported, not assumed
  to mean frozen playback or uninterrupted capture. Pending/unobserved records
  and nonconsecutive render sequences are never bridged for cadence checks.
- Cadence comparisons allow floor/ceiling display refresh counts. For example,
  23.976 fps on a 59.94 Hz display normally alternates approximately 33/50 ms.
  A residual above 2 ms is a review candidate, not a measured physical stutter.
  Repeated filter-buffer messages do not create transitions; backend changes
  and changes of the rendered frame interval do. Paused and identified seek-preview
  frames retain compositor evidence but are excluded from playback cadence,
  audio-phase summaries and initial baseline selection.
- Presentation delay and the fresh AVOS audio-phase estimate appear separately.
  The fixed initial compositor baseline describes delivery after the scheduled
  deadline; it does not calibrate the soundbar or manual A/V delay. No new baseline
  is learned after seek/resume. Layer-generation and refresh changes are excluded
  from baseline comparisons. Keep the device, route and clip fixed for comparisons.
- Query durations are recorded. Check overhead and history overlap on each device
  before relying on capture; shorten the interval only when history coverage
  requires it. Compare with capture disabled when investigating marginal cadence.

`CORRELATED` means matching evidence exists, not complete coverage or healthy
physical output. `UNCORRELATED` means completed compositor records exist but
cannot be matched; `UNAVAILABLE` means usable presentation evidence is absent.
This measures the compositor, not the panel or heard audio. See
[analyze_logs.md](analyze_logs.md) for interpretation.

### Optional fresh PCM presentation observations

Some PCM routes query timing only every two seconds once the AudioTrack timestamp
is trusted. Their fallback `playhead_delay` logs cannot satisfy the analyzer's
500 ms observation requirement. Do not relax that requirement or treat cached
clock extrapolation as measured progress.

After installing a build with `at_pcm_observe`, enable independent diagnostics:

```bash
PCM_PRESENTATION_CAPTURE=1 SURFACEFLINGER_CAPTURE=1 \
  bash test/stress_seek_validate.sh 10 8 2000
```

This also works in campaign and speed-session wrappers. The existing native
observer thread samples the PCM playback head about every 100 ms while unpaused,
logging `audio_present_diag: ... source=pcm_observer generation=...`. It neither
updates the playback clock nor changes the production timing-query throttle.
Queries crossing a flush/recreation are discarded; generations isolate counter
resets. Passthrough sampling and clock selection remain unchanged. Playback-head
progress still does not prove heard audio at the soundbar.

The driver explicitly arms a 600-second lease after logcat startup, waits up to
eight seconds for its acknowledgement or fresh `pcm_observer` samples, and
disables it on cleanup. A device-timestamped setup marker excludes older records;
periodic samples can confirm startup even if the one-off acknowledgement is lost.
Collector failure or missing confirmation stops before key injection; a timeout
can mean delayed log delivery as well as missing command support. This setup wait
does not change playback recovery thresholds.
A killed/disconnected host leaves at most the remaining
lease; a longer single phase needs a deliberate lease renewal or another phase.
An old APK without this command stops before key injection with an explanatory
error. Both capture options default to off and are independent: repeat with
`PCM_PRESENTATION_CAPTURE=1 SURFACEFLINGER_CAPTURE=0` to compare compositor
collection overhead while keeping audio evidence unchanged.

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

Start video playback first, or supply `--video` to capture a fresh launch:

```bash
PCM_PRESENTATION_CAPTURE=1 SURFACEFLINGER_CAPTURE=1 \
CONTINUE_ON_FAIL=1 ROUNDS=2 RESUME_CYCLES=5 SEEK_CYCLES=10 \
BURST_PAIRS=10 BURST_GAP_MS=150 SESSION_RAW=1 \
python3 test/run_stress_campaign.py \
  --serial 67301FDDV0027C \
  --video 'content://com.archos.media.videocommunity/external/video/media/277'
```

`--video` accepts a URI or absolute path on the device. It stops Nova, starts
raw log capture, initializes the existing debug receiver and enables `dbgs 2`,
`dbgsink 2`, `dbga 2` before opening the requested video from position zero.
It preserves the selected decoder, speed and PCM/passthrough preferences;
it does not use the speed runner's preference override or restore mechanism.
Select the intended settings (normally 1x for this campaign) beforehand.
`--serial` overrides `ANDROID_SERIAL`; `--package` overrides `PACKAGE`.

Startup waits up to `--startup-timeout 60` seconds for playback diagnostics,
then captures `--startup-hold 5` seconds of baseline before running the existing
campaign. `startup/logcat.log`, `startup/commands.log` and
`startup/startup-report.json` retain launch evidence even when setup fails.
The optional PCM observer is armed before launch; optional SurfaceFlinger
capture also starts before launch and writes its own report under `startup/`.
The startup collector stops before the campaign's existing collectors start;
these are separate captures, with a setup gap between them. `READY` confirms
launch evidence, not startup health or physical lipsync. Startup is not included
in resume/seek verdicts or their phase baseline. Its compositor generation is
also independent of later phase collectors. No APK change is needed beyond
the debug/observer support already required by these tests.

Without `--video`, playback and collection start exactly as before.
The wrapper accepts the campaign's environment
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
configuration so the original settings cannot be overwritten. On that rejection,
the launcher reads the debug test backup to print the previous session's recovery
command when available; it does not attempt restoration with the rejected new
session's ID. Restore the previous session, then rerun the speed command. A setup
failure reports that speed cycles did not start, without requiring a nonexistent
driver configuration. This launcher
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
earlier player records have arrived. The analyzer checks poll timestamps and
each identified player PID/TID separately, reporting poll delivery skew as
`poll_marker_skew_ms` and interleaved player-thread delivery as `thread_log_skew_ms`.
Direct renderer records can overtake older stdout-thread records. This is
accepted only when both records identify distinct producers; logs without
PID/TID retain strict ordering. The analyzer never sorts playback events or
excuses reversals within one producer. Embedded presentation counters and render
sequences retain their independent monotonicity and continuity checks. The
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
Both capture filters also retain Mode 2 dynamic-clock entry, readiness, fallback
and renderer handoff records; the recording report lists those transitions with
their source lines.
Routine AudioTrack capacity checks, JNI integer-call entry
and sink write-attempt traces require audio debug level 3. Level 2 retains
accepted writes, errors and presentation evidence while reducing retry-loop
traffic that competes with render records. Missing records still mean incomplete
evidence; no timing thresholds are relaxed.

Seek startup uses its own `SEEK_STARTUP_MAX_MS` budget because target readiness
precedes audio readiness and renderer scheduling. `first_write_ms`,
`first_video_ms` and `first_render_ms` report this wait using logcat timestamps;
they are not measurements of first audible/visible output. The 250ms feed-gap
limits still apply between subsequent records and through a silent tail.
Resume uses the same separation with `RESUME_LATENCY_MAX_MS`: the initial
write/video wait is checked against that budget, not also against the steady
250ms feed-gap limit. Speed hold windows retain their leading-gap checks.

A gap in renderer sequence numbers (`missing_render_records`), or missing
adjacency evidence in legacy logs (`missing_render_continuity`), leaves a live
phase `INSUFFICIENT_EVIDENCE` and non-successful, matching the recording review.
Independent stalls, late submissions, cadence violations and sustained phase
errors still produce `FAIL` even when records are also missing.

Audio presentation records go directly to logcat, preserving the querying
thread. Playhead records include the monotonic nanosecond query interval
(`query_start_ns` through `sample_ns`); timestamp records retain the actual
AudioTrack timestamp. A backward counter between overlapping playhead queries
cannot establish observation order. The analyzer reports
`missing_presentation_order` / `INSUFFICIENT_EVIDENCE` and excludes that sample
from progress evidence. The same applies to conflicting playhead counters with
equal millisecond sample times in older captures. A backward counter from a
later, non-overlapping query still fails as `presentation_reset`; DAC timestamp
conflicts, stalled counters and independent playback failures remain failures.

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
checks, 2 usage/precondition/infrastructure failure, 3 insufficient evidence
from a phase driver or the shell campaign. The Python report wrapper returns 1
for insufficient evidence and records `INSUFFICIENT_EVIDENCE` in `run-report.json`.
Interruptions are nonzero. An incomplete phase still stops the campaign by
default; `CONTINUE_ON_FAIL=1` permits collecting subsequent phases.

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

The single-cycle analyzer prints `healthy=0|1` and a `verdict`; exit 0 means parsing completed,
not that playback passed. Invalid evidence/configuration exits 2. The device
drivers consume `healthy` and distinguish evidence-only failures from playback
faults. Sparse or missing presentation observations remain `healthy=0` with
`INSUFFICIENT_EVIDENCE`, never PASS. Some routes expose only throttled legacy
playhead records roughly two seconds apart, which cannot establish continuity
at the default 500ms limit. Frozen counters, counter resets and independent
playback faults still fail; neither the threshold nor native query rate changes.

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
Resume-boundary attribution handles both delayed renderer records and renderer
records that overtake the buffered resume message. The latter are revisited once
the pause start and applied duration establish the monotonic resume boundary.
No adjustment is inferred from deadline spacing, and pending records are cleared
on seek/open/stop so a later boundary cannot repair another playback context.

`stress_pause.sh`, `stress_seek.sh` and `stress_speed.sh` remain unvalidated
chaos drivers; successful completion is not a playback verdict.

The separate native host harnesses are indexed in `AGENT.md`. `make test`
builds `ff`, `comp`, and `stream_url`; it does not run the Python or shell suites
or automatically execute those binaries. The subtitle engine's host tests live in
`test/host/` and are run separately; see
[Native subtitle engine host tests](#native-subtitle-engine-host-tests).

Run all discoverable host regression tests (no Android device required):

```bash
python3 -m unittest discover -s test -p 'test_*.py'
```

The former monolithic `pause_resume_sync.py` replay used removed functions and
obsolete preload/timeout assumptions. It is now a compatibility runner:

```bash
python3 test/pause_resume_sync.py
```

It runs these discoverable production-code replays:

- `test_pause_resume_policy.py`: manual delay, preserved versus destructive timing
  resets, paused checkpoint timeout accounting, backend preload policy, retained
  PCM on capacity/zero-write/positive-partial-write pause races, abort and failed
  restart. Sink and wall-clock behavior are mocked; this does not reproduce JNI.
- `test_slew_offset.py`: all bounded step policies in both directions, variable
  frame-duration caps, per-frame retry guards, target deadbands, the existing
  unflagged fallback completion branch, retained resume versus cold-start pause
  behavior, renderer wall-reference shifts, and seek cancellation. It extracts
  current arithmetic and lifecycle functions and compares the complete step path
  against `fixtures/slew_before_refactor.c` from `0ab00c6`, including overlapping
  flags, variable durations, completion cleanup and repeated-frame retries.
- `test_audiotrack_speed_epoch.py`: checkpoint continuity and concurrent publication.

The tests compile source excerpts with warnings treated as errors. Missing source
markers fail rather than silently skipping coverage. A host pass checks these
contracts, not device scheduling, receiver relock or physical lipsync.

## Native subtitle engine host tests

`test/host/test_host_native.py` compiles small C programs together with the **real**
subtitle engine sources, links them on the host and runs them. No NDK, device or
Makefile change is involved. Android-only headers (`jni.h`, `android/*`, `EGL`,
`GLES2`) are replaced by `host_stubs.h`; the few symbols that live elsewhere in the tree
are weak stand-ins in `stubs.c`; `fake_backend.c` replaces the SSA/SRT/GFX decoders
where a test must control exactly when a backend answers, how slowly, and what it was
fed. The directory is not a Python package, so
`python3 -m unittest discover -s test` does **not** run it; invoke it explicitly.

### Environment

- Linux with a C11 compiler (`cc`, or set `CC`), `pkg-config` and pthreads.
- libass development files (`pkg-config --cflags libass` must work, or set
  `PKG_CONFIG_PATH`). Tests that need libass are **skipped**, not failed, when it
  is missing.
- fontconfig with the **DejaVu Sans** family installed: `ssa_geometry`,
  `ssa_style_cache` and `ssa_late_script` render real text with it.
- macOS is **not yet supported**: the full suite does not currently run there (a
  timed-wait function and the font setup are known problems, tracked separately).
  Use Linux until that is fixed.

### Running

```bash
python3 test/host/test_host_native.py                 # all, UBSan (the default)
python3 test/host/test_host_native.py -v -k registry  # one test, every check listed

# Concurrency tests under ThreadSanitizer
SANITIZERS=thread python3 test/host/test_host_native.py -v \
  -k race -k registry -k switch -k seek -k close_open -k style_resize -k jni

# Lifetime / use-after-free checks under AddressSanitizer
SANITIZERS=address,undefined python3 test/host/test_host_native.py -v \
  -k registry -k close_open -k jni

HOST_TEST_OUTPUT=1 python3 test/host/test_host_native.py   # print passing output too
```

Output is one row per test (PASS / FAIL / SKIP, duration, number of checks). A failing
test lists only its failing checks, `-v` lists every check, and `HOST_TEST_OUTPUT=1` adds the
raw program output. Compiler warnings from the code under test are collected into one
de-duplicated section at the end. The header records the compiler, libass version and
sanitizer, so a pasted run carries its own environment. Colour is used only on a terminal
(`NO_COLOR=1` turns it off). Exit status is 0 when nothing failed.

`SANITIZERS=none` disables instrumentation; `CC` may carry arguments (for example
`ccache gcc`). A test that runs longer than 120 s is reported as a probable
deadlock, and the concurrency tests also arm their own watchdog that prints
`FAIL: watchdog expired` instead of hanging.

Last recorded full run: 12 of 12 tests passed with `SANITIZERS=undefined`, and the six
concurrency tests (`registry`, `switch`, `seek`, `close_open`, `style_resize`, `jni`) passed
under `SANITIZERS=thread` (2026-10-23, Linux). Record the compiler, libass version and
sanitizer with any later baseline.

### What is covered

| Test | Contract checked |
|---|---|
| `ssa_geometry` | `ssa_geom_compute()` / `ssa_geom_apply()` against a box-only libass reference render, including degenerate and transient inputs |
| `ssa_style_cache` | Forced FontSize is recomputed when canvas or content height changes |
| `ssa_late_script` | An external ASS script fed **after** the first render still receives Custom (forced) settings without the user touching a setting; compared with the same script fed before the first render, and with an unforced run so the comparison can fail |
| `race_harness` | Nothing is visible once `close_track()` has returned |
| `fill_contract` | UNCHANGED / CLEAR / FRAME / ERROR and when the frame generation moves |
| `push_policy` | The 3D "content changed" push: paused only, 3D only, once per generation, no locks held |
| `jni_listener` | Listener global-ref and thread attach/detach lifetime against a mock JavaVM, including the deferred destroy: `nativeDestroy()` while a stream still holds the engine frees nothing, and the last release frees the engine and listener ref, attaching a native releasing thread itself |
| `registry_lifetime` | `sub_engine_registry`: an engine is destroyed exactly once and never while held; immediately if unheld, otherwise by the last release; retract never blocks; a newer publish keeps older references; the destroy callback runs outside the registry lock; stray releases are ignored; concurrent acquire/release against publish/retract |
| `track_switch_loading` | Rapid switches between two large SRT/VTT feeds (and `close_track()`) with generation tokens: stale cues dropped, none reach another track's backend, every accepted cue reaches exactly one backend, the last track loads completely, no backend leaks |
| `seek_pause_feed` | Pause, resume and seek racing the initial bulk feed (flush, then raise the re-feed flag): no deadlock, pause state exact, one full copy of the list afterwards |
| `close_open_parsing` | Closing a video and immediately opening another while its parse worker is active, with the Java side rebuilding the engine: each engine destroyed exactly once and never while held, no cross-video cues, no leaked backend; both an orderly and a hostile close order |
| `style_resize_paused` | Style changes and surface resizes while paused render once at the frozen clock, reach the backend as a coalesced canvas/box pair ending on the last request, survive concurrent style and resize storms, and leave the pause state untouched |

### Limits

- The `track_switch_loading`, `seek_pause_feed` and `close_open_parsing` scenarios
  exercise the real engine, generation-token and registry code, with the STREAM-level
  workers modelled by small threads in the tests: `stream_sub_ext.c` needs the whole
  STREAM to link and is **not executed** by these tests. They check the engine-side
  contract those workers rely on.
- The mock JavaVM validates our use of the JNI contract, not a real ART.
- The render thread never gets a real EGL surface (the same as production 3D mode
  without a window), so the GPU blend path is link-only here.
- `track_switch_loading` asserts that the race was actually exercised (some feeders
  were cut off mid-list). That depends on feeding being slower than a switch interval;
  raise `BIG_CUES` if it ever fails on a much faster machine.
- Smoke-test on a device as well: pause in 3D, switch or disable the subtitle track,
  and confirm the old cue clears and the new one appears without resuming; load an
  external `.ass` after playback has started with "Custom" settings active; seek inside
  a PGS/VobSub cue.

## Renderer extraction validation

The arithmetic-only extraction was authorized on 2026-10-15 before device
validation of `fdf81dd`, `f386291`, `8d4ece1` and `0ab00c6`. Its host checks
do not close that hardware-validation gap. Save the build revision, device, route, codec,
audio mode and capture options with each baseline; do not compare different
binaries or routes as though they were one experiment.

- Run separate pause/seek campaigns for PCM and passthrough modes 1/2 where
  supported. Keep speed testing on PCM.
- For PCM, run atempo, Sonic and AudioTrack speed sessions, including FLAC and a
  non-1x settling transition such as 1.8x to 1.2x. The current speed-session runner
  automates 1x to 1.6x to 1x only; use a separately annotated manual capture for
  the non-1x endpoint until the driver and analyzer support it explicitly.
- Compare SurfaceFlinger capture enabled/disabled on the same configuration,
  keeping PCM presentation capture and all other options constant. Query duration
  is overhead evidence, not proof of a playback stall. Missing compositor or
  audio coverage remains insufficient evidence.
- Host replay establishes arithmetic equivalence; device campaigns check playback
  integration. Physical lipsync needs a calibrated camera/photodiode plus
  microphone measurement at the panel and speaker. That rig is not a prerequisite
  for the first two checks.

No connected device was available during the 2026-10-14 host-test repair. That
work supplies a test baseline, not completion of the device-validation gate.

The transition inventory and coverage boundaries are in
[sync_transition_invariants.md](sync_transition_invariants.md). The slew replay
also checks the deliberately different PCM/Mode 1 resume-boundary limits,
including short backward corrections and PCM speed scaling. The Mode 2 seek
replay covers named frontier reasons surviving resets, one-time consumption and
seek-specific refill protection. These checks do not replace device campaigns.

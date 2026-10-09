# Analyzing playback stutter and A/V sync logs

Use this procedure to investigate seek, pause/play, resume and audio-speed
transitions. The objective is to establish what changed, how long recovery took,
and whether playback returned to its original timing. Analyze the complete
recording, including startup, short interrupted transitions and the final tail.

[TEST.md](TEST.md) describes capture commands, harness settings and report
formats. This document describes how to interpret the evidence and what needs
manual investigation beyond the automated report. Source code and the captured
build's diagnostic definitions remain authoritative.

## 1. Establish the playback context

Record the build revision, device/Android version, audio route, media file and
frame rate, audio codec, requested and effective output mode, speed backend,
manual A/V delay, and actions performed. State whether the symptom was actually
seen/heard or inferred from logs. A useful reproduction includes several seconds
of normal playback before the first action and after the final action.

Identify the effective path from output configuration and runtime records:

| Path | Timing considerations |
|---|---|
| Decoded PCM at 1x | Check accepted samples, presentation progress and one-shot startup/resume corrections. |
| PCM with atempo or Sonic | Distinguish requested speed, filter output and the speed committed to presentation. Buffered output can still belong to the preceding speed. |
| PCM with AudioTrack PlaybackParams | Check platform speed application and checkpoint/timeline continuity. Do not apply the software-filter byte-duration assumptions. |
| Passthrough Mode 1 | AVOS creates IEC bursts; compare against the established relative phase of the same output. Partial nonblocking writes can be normal. |
| Passthrough Mode 2 | Trace the selected heard clock, observer validity and any static/dynamic clock transitions. Its latency policy differs from Mode 1. |
| AC3 recoding | Identify the resolved output mode and account for the recode/pacer path; the UI setting alone does not describe the sink clock. |

Do not carry a phase reference across a different file/session, output mode,
format, manual-delay configuration or audio route without establishing that
the reference is still applicable. The recording analyzer detects some context
changes and invalidates later phase coverage. Unlogged route changes still need
manual identification. Split mixed captures and analyze each context with its
own configuration.

Distinguish an ordinary non-flushing pause from background/surface restoration
or reopening a saved playback position. The latter may create a new AudioTrack,
perform a seek and establish new clocks; it is not merely a wall-time shift of
the previous session.

## 2. Collect and preserve the necessary evidence

Use a build containing the timing diagnostics and the debug settings in
[TEST.md](TEST.md). Prefer a harness capture, which preserves configuration and
ends observations with device-timestamped poll markers. For manual collection,
retain an original `logcat -v epoch` recording and collector errors. Do not keep
only lines containing `error`, `diff` or `wrote`.

For rapid remote-OK reproduction, use `BURST_PAIRS=10
test/stress_resume_validate.sh 5 8 2000` (on one shell line). The
[burst procedure](TEST.md#rapid-pauseplay-bursts) sends each sequence through
one adb invocation and verifies the actual native pause/resume pairs. Analyze
the saved `burst-N.log` and `burst-N-review.json`, including interrupted recovery
windows; compare final settled phase against the saved preflight reference.

| Records to process | Information to extract |
|---|---|
| Output configuration, mode, filter selection, PID | Playback context and changes that invalidate a reference. |
| Seek request, parser result, `_stream_play_n_frames`, `VIDEO_SEEK_TARGET_READY`, `AUDIO_SEEK_TARGET_READY` | Requested position, actual landing, preview boundaries, admitted frame and audio readiness. |
| `android_sync: pause start`, `WALLCLOCK_RESET`, `audio_resume_route`, renderer resume shift/correction | Pause lifecycle, whether output survives, and when the renderer applies the resume update. |
| `startup_anchor_commit`, `mode1 published anchor`, phase baseline and seek/resume slew messages | Which sample established the anchor, correction magnitude/direction and interruption/completion. |
| `put_time_calc` | Published audio-clock samples, wall reference, committed speed, epoch-related resets and reanchor decisions. |
| `video_sched_diag`, `stream_av_diff` | Admission-time progress and gross internal A/V differences. These are not final presentation measurements. |
| `video_render_diag` | Frame/epoch, scheduled deadline, submission time, effective interval, projected phase and anchor age. |
| `audiotrack_write`, compressed short-write/continuation/failure records | Actual accepted byte counts, partial writes, retries and errors. |
| `audio_present_diag`, Mode 1/2 occupancy observers | Observed counter progress, source, freshness, generation/epoch, trust and underruns. |
| `AUDIO_STARVED`, late-frame drops, runtime failures and log-loss notices | Explicit faults and evidence gaps. |
| Speed request/filter/commit, timeline, FIFO and ledger diagnostics when available | Ordering between control, audio processing, accepted output and presentation. |

The standard filtered harness captures retain the common timing and Mode 1
transition records. Detailed speed ledger/filter records may require a broader
capture and the backend-specific debug settings. Their absence cannot prove
that commits or filter buffering behaved correctly.

Check for missing diagnostics, stale anchors, `chatty`/dropped lines, truncated
captures and process restarts before interpreting zero error counts. The absence
of underrun messages is not proof that underrun monitoring was active.

## 3. Run the appropriate analyzer

From the repository root, analyze a saved recording, including logcat brief:

```bash
python3 test/analyze_stress_recording.py avos-21.log --format markdown > /tmp/avos-21-report.md
python3 test/analyze_stress_recording.py avos-21.log > /tmp/avos-21-report.json
```

For a harness capture, preserve its thresholds and phase reference:

```bash
python3 test/analyze_stress_recording.py path/to/logcat.log \
  --config path/to/analyzer-config.json --format markdown > /tmp/playback-report.md
```

Exit 0 means no issues observed within the measured scope; exit 1 means issues
or insufficient evidence. Both produce a report. Exit 2 means invalid input or
configuration. Do not discard a report simply because the command returned 1.

For a single epoch-timestamped harness observation, use the live analyzer with
the threshold environment used during capture:

```bash
python3 test/analyze_stress.py seek path/to/current-seek.log
python3 test/analyze_stress.py resume path/to/current-resume.log
python3 test/analyze_stress.py speed path/to/current-cycle.log
```

Here, inspect `healthy` and `reason`: exit 0 means analysis completed, not that
playback passed. Use the recording analyzer for an entire session containing
multiple seeks and pauses. The live analyzer expects a single controlled test
observation and can reject intentional boundaries elsewhere in a whole session.

The recording analyzer evaluates scheduled video and internal phase. It does
not perform the complete live audio-presentation, write-gap or action-to-resume
latency checks. A clean offline verdict is not a device-campaign PASS.
Its segment durations end at the last included render record. A silent tail
after that record needs separate inspection; use the live analyzer's timestamped
observation barrier to establish whether playback stopped progressing.

## 4. Keep clock domains and segments separate

Use integer nanoseconds for `deadline_ns` and `submit_ns`, converting their
differences to milliseconds. Their monotonic domain is not logcat's epoch clock.
`atime64` values may have a different origin from absolute monotonic time.
Compare intervals within the same domain unless a paired clock observation
explicitly supplies the mapping.

For brief logs, embedded render timestamps support cadence and phase analysis.
They do not timestamp neighboring writes, button events or slew-completion
messages. Do not assign an unclocked message the timestamp of the preceding
`put_time_calc` and then describe the result as an exact duration. Thread output
can interleave; log order alone does not establish execution timing.

Separate startup, seeks/preview, each ordinary playing interval, pauses and
output/session changes. Do not compare frame progression across seek epochs or
audio counter deltas across incompatible sources/generations. Speed changes
also alter the media-to-playback mapping: RST/media timestamps and time-scaled
TS values must not be subtracted as though they represented the same clock.

Within each transition, retain three observations:

1. The last stable pre-action timing reference.
2. The full transient, including gaps, corrections, drops and interrupted work.
3. The post-recovery interval, or an explicit statement that recovery was not
   observed before the next action or the end of capture.

## 5. Identify scheduled stutter

First check submission continuity within a compatible epoch and playback
segment. `render_seq` increments after each successful timed MediaCodec release,
outside the logging guard, and persists across pause/seek/flush for that decoder.
Consecutive numbers establish adjacent submissions. A jump establishes missing
diagnostics, not their unseen timing; report `missing_render_records` and keep
the verdict inconclusive. Do not normalize a three-frame gap into three assumed
regular intervals. Duplicated, reversed or partially missing sequence fields
also prevent a clean continuity verdict.

For legacy logs without sequence numbers, a media-timestamp jump greater than
1.5 times the larger adjacent frame interval is `missing_render_continuity`:
the recording cannot distinguish skipped frames from omitted logs. The analyzer
still detects ordinary deadline errors between records with adjacent media times.
Known late submissions and explicit drops remain independently reportable.

For consecutive submissions in a compatible epoch, calculate:

```text
actual_interval_ms = (next.deadline_ns - previous.deadline_ns) / 1e6
submission_lateness_ms = (submit_ns - deadline_ns) / 1e6
```

Compare the interval with the logged effective `interval_ms`, rather than
arrival spacing of log messages. At a speed boundary either adjacent effective
interval can be appropriate. For example, a 23.976fps stream has approximately
41.708ms intervals at 1x and 20.854ms at 2x. Variable-frame-rate content needs a
suitable model/tolerance; a fixed-rate assumption can otherwise create false
findings.

Look for unexplained gaps, duplicate/reversed deadlines, repeated cadence
oscillation and submissions arriving after their deadlines. Negative submission
lateness is normal when the renderer queues a frame ahead of presentation.
Zero late-frame drops does not prove smooth playback: the analyzer can detect
late submissions well below the engine's 200ms drop threshold.

At resume, account for the explicitly logged renderer update before judging
the interval spanning it. In `avos-21.log`, an interval of 126.708ms comprised
a normal 41.708ms interval plus a 123ms pause shift and a -38ms correction.
The resulting 85ms adjustment is explained by that boundary. An unexplained
residual still counts as judder; a resume marker alone is not an exemption.

Render logs are emitted after releasing the scheduler lock and can arrive after
a newer resume message. When both pause start and shift duration are recorded,
the analyzer adds them to obtain the embedded monotonic resume time. A render
whose `submit_ns` predates that time retains the earlier boundary, regardless
of log delivery order. Logged before/after renderer offsets give the exact net
shift; otherwise the analyzer uses the rounded shift and correction messages.
Without clock evidence it retains log-order accounting. Submissions during the
resume millisecond or after the lock was released can remain ambiguous; the logs
do not contain the frame's scheduler-generation snapshot. Do not exempt an
unexplained interval simply because it is close to a pause.

Check audio starvation, observed presentation and decode/feed activity around
the same event. A write gap may be covered by queued audio. Advancing writes do
not prove advancing presentation. Scheduled deadline irregularity establishes a
software timing disturbance, not exactly what Android displayed.

## 6. Identify persistent lipsync drift and recovery

Prefer fresh `video_render_diag.phase_ms` evidence for scheduled phase. It is
video media time minus projected heard-audio time at the presentation deadline,
with the requested manual delay removed by the diagnostic. Negative means the
scheduled picture is behind that audio estimate. The analyzers use anchors aged
0–100ms; stale anchors cannot establish current phase.

Do not equate this with `stream_av_diff.diff` or
`video_sched_diag.frame_minus_heard`. Those describe earlier pipeline points.
A stable admission-time difference near zero can coexist with a substantial
scheduled phase. Compare like measurements before and after actions.

Mode 1 intentionally retains a relative static-clock phase. Establish a stable
reference before the actions and retain it across seeks and repeated pauses on
the same output. Do not redefine the reference after every transition: that
would hide cumulative drift. PCM and Mode 2 default to zero in the analyzer;
inspect the applicable clock policy and use an explicit known reference when
justified by the capture context.

Read these report fields together:

| Field | Interpretation |
|---|---|
| Raw phase extrema and median | Absolute internal phase within the selected samples, not acoustic calibration. |
| `phase_max_ms`, `phase_bad_ms` | Maximum deviation and longest sustained bad run under the error policy. |
| `initial_phase_ms`, `initial_deviation_ms` | First fresh sample and its deviation at this segment's start. |
| `recovery_ms` | Start of the first confirmed close-to-reference run, measured from the first render submission. |
| `final_stable_run_ms` | Start of the final qualifying run; later excursions can make this much later than first recovery. |
| `settled_phase_median_ms`, recovery `status` | Timing of the final confirmed run, or why settlement was not observed. |

Defaults distinguish sustained error from close recovery: deviation greater
than 80ms for 250ms after the initial 500ms settling interval is an error;
recovery requires at least three fresh samples within 8ms for 250ms. These are
analysis policies, not universal perceptual limits. The report includes resolved
settings. Preserve historical configurations when comparing builds.

A correction-complete message says the renderer reached its target. It does
not prove that every queued frame has already displayed or that physical lipsync
is correct. Confirm subsequent fresh phase and cadence. A later good interval
does not erase a sustained error earlier in the transition.

## 7. Trace each transition type

### Seek and reopening at a saved position

Follow the request through parser landing, video preview/admission, audio
readiness, first accepted output, clock publication and phase recovery. Record
both requested and achieved positions: a coarse keyframe landing seconds away
from the request is distinct from spending those seconds recovering A/V sync.

Exclude deliberate preview output from normal-playing cadence measurements.
Submission after resume does not prove a frame belongs to ordinary playback:
match its timestamp and epoch against `SINK_REF_DEFERRED` emitted during an
explicit `_stream_play_n_frames` preview interval. Both analyzers carry that
identity across resume and report `queued_seek_preview` separately, including its
lateness. Preview lateness exceeding `SEEK_STARTUP_MAX_MS` still reports an issue.
Preview output cannot supply healthy playback coverage or satisfy first-render
recovery, and render sequence gaps remain evidence gaps. Without matching records,
do not infer preview status from a late deadline or widen the normal-frame limit.
Reject stale-epoch frames when checking progress. Verify that the final seek in
a chained sequence establishes the active clock and that old absolute anchors
or observations do not survive into it. Real audio gaps require their own
hold/progress analysis; do not assume the first audio packet must coincide with
the first video frame.

For Mode 1, inspect `startup_anchor_commit`, `mode1 published anchor`, the phase
reference and the subsequent seek correction. The renderer should anchor to the
clock publication, not whichever refill burst the renderer thread later sees.
A small remaining refill correction can be normal. Report its magnitude and
recovery interval separately from any lasting phase shift.

The live analyzer measures target-ready to first write, scheduled frame and
render log against `SEEK_STARTUP_MAX_MS` (1000ms by default). It uses the ordinary
feed-gap limits only after each producer starts, including silent tails. A
target-ready message alone does not mean audio is ready or a frame is displayed.
For PCM, inspect `pcm_startup_correction`, `android_sync anchor_diag` and
`at_ledger` together: an early correction target may precede presentation-clock
calibration. The phase capture retains these messages for that comparison.

Startup consistency requires multiple comparable opens. One recording with one
startup cannot prove cross-run consistency. Compare equivalent cold/warm starts,
the publication pair and the resulting stable phase; do not reuse a phase from
a different soundbar route as an absolute calibration.

### Ordinary pause/play and long pauses

Check `audio_resume_route` for the actual queue-preservation/preload decision.
Trace pause start, audio restart, renderer wall/target adjustment, first accepted
post-resume output and any correction. `WALLCLOCK_RESET` marks the start of the
resume work; it can precede the renderer's completed adjustment.

Paused wall time must not count as audio progress, refill evidence or clock
drift. For Mode 1, unfinished measurements/corrections should survive an ordinary
non-flushing pause when their output context remains valid. Look for repeated
measurement restarts, lost correction targets or a fresh full timeout after
each pause.

For rapid sequences, keep the original pre-sequence reference and report every
short window, then assess the final uninterrupted recovery. Compare settled
medians across the sequence; a steadily growing difference is stronger evidence
of cumulative drift than a large transient during one interrupted correction.
Do not mark a short window healthy merely because it ended before an error's
duration threshold could be reached.

For long pauses, verify the renderer offset, pending target and drift reference
shift consistently. If background restoration reopened the player, switch to
the startup/seek procedure instead of expecting preserved pause state.

### Audio-speed ramp up, hold, ramp down and return to 1x

First confirm decoded PCM and the actual backend. The speed harness does not
select it; passthrough is not a valid speed-campaign input. Examine every ramp,
every high-speed window and the return-to-1x tail, including the last window in
the log. An earlier successful interval must not conceal a later failure.

Separate requested speed from applied/committed speed. For software filtering,
queued output and its presentation boundary can legitimately delay the video
commit. Inspect filter selection, accepted output, ledger/checkpoint state,
commit ordering and the timeline adopted by the renderer. For PlaybackParams,
inspect platform speed application and the corresponding presentation mapping.
Do not infer the backend from a preference flag alone.

Use the effective render interval at each committed speed. Look for unexpected
hard reanchors, discontinuous TS/RST mapping, duplicated/lost media progress,
prolonged correction or cadence oscillation after the ramp ends. Confirm that
the final committed speed is 1x and that ordinary cadence and phase recover.

When a seek or pause overlaps a ramp, follow epoch/reset ownership before
comparing ledger positions. Old speed boundaries must not commit into a new
seek timeline; an ordinary preserving pause has different rules. A commit
timeout or fallback is a diagnostic lead, not proof of desync by itself.

The common analyzers detect the resulting timing symptoms. They do not fully
validate ledger ownership, sample continuity or the audible moment of a speed
commit; these require the backend records and source-level tracing described in
[atempo](audio_speed_atempo_architecture.md),
[Sonic](audio_speed_sonic_architecture.md) and
[AudioTrack](audio_speed_audiotrack_architecture.md).

## 8. Correlate audio flow without overclaiming

Classify `wrote N out of M` numerically. `0 < N < M` is a partial write; `N == M`
is a full write. A positive count proves accepted bytes, not a completed access
unit or heard audio. For compressed output, inspect continuation offsets and
failures around each suspicious burst. Use the format's actual IEC carrier
geometry; a 16-byte alignment seen in one run is not a universal codec rule.

Check fresh observed counters within one source/generation/epoch and verify
the gaps between samples. Two advancing endpoints several seconds apart do not
prove continuity between them. Count underrun increments, not an old cumulative
total at the start of capture, and do not double-count writer/observer reports.

Mode 1 occupancy observations are diagnostic; do not silently replace its
selected clock with a candidate estimate. For Mode 2, inspect whether the
dynamic clock was actually active and which evidence supported entry/exit.
An untrusted timestamp or frozen vendor playhead is different from proven loss
of accepted audio. Keep clock-estimation problems separate from no-sound or
write-stall failures.

## 9. Report a bounded conclusion

Include a table per transition with source lines/epoch, initial deviation,
correction records, first recovery, final stable phase, cadence/late-submission
findings and audio evidence. State interruptions and unmeasured intervals.
Use consistent timing origins: first write, first render and button-to-recovery
are different durations.

Summarize separately:

- Persistent phase change versus temporary recovery.
- Steady-playback cadence versus accounted transition adjustments.
- Observed failures versus missing evidence.
- Internal timing versus the user's seen/heard symptom.

Attach the original log, JSON/Markdown reports, configuration and playback
context. Keep actionable source-line references. Avoid conclusions based only
on a final good sample, zero drops or positive writes.

### Optional SurfaceFlinger comparison

Capture with `SURFACEFLINGER_CAPTURE=1` as described in [TEST.md](TEST.md).
Read each phase's `surfaceflinger-report.md` beside its ordinary analyzer report.
The JSON includes exact per-frame matches, source log lines, query duration,
snapshot overlap and per-transition summaries. The wrapper's `run-report.json`
also compares phase segments against the initial session compositor baseline.

Separate three observations: AVOS deadline spacing, actual compositor spacing,
and AVOS's estimated audio/video phase. Smooth deadlines with irregular actual
spacing point downstream of scheduling. A consistent increase in actual-minus-
desired time with unchanged scheduled phase suggests additional video delivery
delay. Neither observation identifies downstream audio latency. Do not simply
subtract presentation delay from `phase_ms` during speed changes: the media/wall
clock relationship and freshness of the audio estimate must first be established.

Allow normal refresh quantization (such as 33/50 ms alternation for 23.976 fps
at 59.94 Hz). Review cadence candidates against adjacent submissions within one
epoch/transition; do not join across missing records, paused intervals or layer
recreation. Constant presentation delay can leave cadence smooth, so inspect
delay extrema and changes from the fixed initial baseline as well as cadence.
No late replacement baseline is used when initial calibration evidence is absent.
Exact matches validate correspondence only for those frames, not the whole run.

Physical lipsync and panel cadence remain unmeasured by these scripts.
AudioTrack timing can omit downstream TV/ARC/eARC/AVR/soundbar buffering and DSP.
Use a synchronized recording of known flash/beep events to establish physical
offset, accounting for capture delay. A stable physical offset on one route
does not by itself justify another scheduler correction. See
[REMEMBER.md](../REMEMBER.md) and
[sync anchoring rules](sync_anchoring_rules.md) before changing playback policy.

Delay Estimation (AudioTrack)
=============================

Overview
--------
AudioTrack delay is estimated from three sources:
- Dynamic timestamp (AudioTrack.getTimestamp).
- Playback head position (getPlaybackHeadPosition).
- Static latency (AudioTrack.getLatency).

The goal is to track real output latency, avoid reinjecting static
latency once a valid dynamic delay has been observed, and keep anchors
stable when timing is noisy (especially on Android devices with unreliable
AudioTrack timestamps).

Equations
---------
All times are in milliseconds (TS domain) unless noted.

Core A/V diff (control):
  diff = (video_time - audio_time) + sync_delay

Audio pipeline delay (ms):
  sync_delay = codec_delay + filter_delay + sink_delay - video_delay

Heard time (estimated):
  heard_ts = audio_time - sync_delay

Latency Terms
-------------
The code and logs distinguish several latency values. They must not be
treated as interchangeable:

- `app_latency` / geometry latency: the local AudioTrack buffer geometry,
  computed from the buffer size, frame size, sample rate, and playback speed.
  This is scheduler-local queue depth. It does not attempt to include Android
  HAL, HDMI, AVR, soundbar, or codec decode latency.
- Track/platform latency: the raw value reported by AudioTrack `getLatency()`.
  On some passthrough routes this can include large platform or HDMI pipeline
  estimates; on other routes it may be stale, rounded, or codec-insensitive.
- System/output latency: the value reported by the output-latency path when
  available.
- Raw `pipeline_latency`: the conservative platform estimate used at startup
  and for diagnostics. It is the maximum of track/platform latency and
  `system_latency + app_latency`.
- Normalized mode-2 latency: after at least 250ms of paired accepted compressed
  bytes and logical samples, AVOS derives the compressed duration represented by
  the AudioTrack buffer. It combines that capacity with the residual platform
  latency and freezes the result. It is not capped: low-bitrate compressed
  streams can legitimately place more than one second of media in the configured
  buffer.
- Static latency: a fallback selected delay used when stable dynamic evidence
  is unavailable. Static latency is not always the raw AudioTrack
  `getLatency()` value. Mode2 uses the normalized latency once its evidence
  window completes for AC3/EAC3/JOC, AC3 recode, TrueHD, and DTS formats.
  Existing codec-aware app/pipeline selection applies only before normalization
  or when valid paired evidence is unavailable. See
  [audio_passthrough.md](audio_passthrough.md).
- `selected_delay`: the delay actually subtracted from `audio_time` to derive
  the raw heard frontier. For PCM it may come from dynamic AudioTrack evidence,
  last-good cache, or static geometry. Mode 1 uses its static passthrough delay;
  Mode 2 uses its normalized latency after the evidence window as the static
  fallback. A separately validated presentation clock may subsequently bound
  heard time without rewriting `selected_delay`.
- `mode2_playhead_audit`: the older synchronous comparison between mode2
  `fakeSize` logical writes and AudioTrack playhead/timestamp counters. It is
  diagnostic only. Production presentation evidence comes from the asynchronous
  generation-scoped observer described below.
- `mode2_normalized_latency`: production record emitted when the estimate is
  calculated, containing the raw platform values, paired evidence, calculated
  compressed-buffer capacity, residual, and selected normalized latency.

The core scheduler rule remains:

  heard_ts = audio_time - selected_delay

For direct Mode 2 this expression is the raw submitted frontier. A monotonic
wall-clock interpolator advances heard time between coarse compressed write
batches and clamps it to the physical buffer envelope. It does not redefine
`audio_time` and it is not a measured occupancy clock.

For Mode 2, a trusted `AudioTimestamp` can provide a dynamic
submitted-minus-presented delay. The centralized heard clock adopts that
evidence monotonically while keeping the normalized capacity clock alive as
fallback. The broad `mode2_dynamic_all` test switch is currently enabled;
disabling it restores the raw AC3/44.1 kHz production allowlist. Mode 1 IEC
timestamps are collected only as shadow evidence and cannot alter heard time.

Exception: during plain PCM AudioTrack PlaybackParams speed epochs, the
AudioTrack playhead is used as a temporary checkpoint clock. At the speed
change, AVOS stores the current heard anchor and a fresh
`getPlaybackHeadPosition()` sample. While the epoch is active:

  heard_ts = epoch_heard_ts + RST_TO_TS_DELTA(frames_delta * 1000 / rate)

This avoids discontinuities caused by combining write-quantized `audio_time`
with stale `last_good_delay_ms` across hardware speed changes. The epoch is
cleared on seek, flush, or stop, not simply when speed returns to 1.0x.

Notes:
- playhead_ms is the output position derived from getPlaybackHeadPosition and is used only
  to compute delay in AudioTrack during normal playback. The PlaybackParams
  speed-epoch checkpoint above is the explicit exception.
- video_delay is only included when timestamps are sampled before the video sink.

Mode 2 Interpolator and Epochs
------------------------------
For direct codec-specific AudioTrack output (`passthrough >= 2`, excluding AC3
recode), accepted compressed writes update a logical submitted endpoint:

  raw_heard = audio_time - selected_delay

`stream_get_heard_audio_ts()` advances `mode2_heard_interp_ts` from monotonic
wall time between write batches. It snaps forward when `raw_heard` overtakes it
and limits free-running lead to:

  max(selected_delay - fixed_latency, STREAM_MODE2_HEARD_INTERP_MAX_LEAD_MS)

The selected delay is decomposed into compressed capacity plus `fixed_latency`,
the downstream platform/route component. This lets an empty replacement track
seed at `audio_time - fixed_latency` while its buffer refills.

`stream_sync_restart_after_pause()` preserves the interpolated phase but resets
the wall epoch. Full sync restart clears it. Seek and mid-playback track changes
carry explicit empty-track ownership through `mode2_heard_frontier_seed_pending`;
the condition is not inferred from timestamps because `audio_time` can remain
continuous across a track recreation.

Complete compressed units are also recorded in an epoch-owned logical-sample and
encoded-byte ledger. A low-rate AudioTrack worker polls `AudioTimestamp`, playback
head, and underrun state outside the writer and scheduler threads, then publishes
generation-scoped snapshots. Mode 2 records media samples; Mode 1 records IEC
carrier frames at the AudioTrack container rate. Both counters are published only
after a complete compressed unit is accepted. The sync layer rejects stale, reset,
implausible, or non-advancing counters before comparing them with the submitted
ledger.

Mode 2 direct logical-frame evidence must prove the configured rate over an
advancing streak and then provide three stable delay samples. On entry, heard
time never moves backward: it holds until physical presentation catches the
existing phase, then follows the measured frontier. A non-flushing pause retains
the ledger and grants a 750ms remapping grace period. Any other evidence loss
slews back to the continuously maintained static clock. Playback-head, encoded-
byte, and frame-size interpretations remain diagnostic-only.

State Machine Summary
---------------------
1) Startup (no valid dynamic yet)
   - Try getTimestamp (if enabled).
   - If invalid, try playback head.
   - If unavailable, fall back to static latency.

2) Dynamic becomes valid
   - last_good_dynamic_delay_ms is set and reused when timestamps are
     invalid or during throttle windows.
   - Static latency is no longer injected once last_good is present.
   - Validity is gated by a short streak of advancing samples to avoid
     false positives after resume/seek (Sabrina/Kirkwood). The current
     playhead-based threshold is 3 consecutive advancing queries.
   - During `startup_hold`, if `getTimestamp()` keeps returning a
     non-advancing frame position after seek/startup, the estimator can
     escape the hold early by promoting a recent sane playback-head
     fallback delay to valid. A bounded timeout provides the same escape
     hatch if the timestamp path never converges.

3) Throttle window
   - Use cached delay if valid.
   - Else use last_good_dynamic_delay_ms (if available).
   - Else fall back to static latency.

4) Prefer playback head over static
   - If playback head delay is valid and clearly below static latency
     (by >= 50ms), prefer it to avoid persistent bias from static latency.

5) Silent lead-in
   - While audio_start_pending is true, sync decisions avoid using
     non-audible audio PTS and defer smoothing until audio advances.

PCM Delay Memory
----------------
- `last_good_delay_ms` stores **HW-only delay** (atempo delay stripped).
  Atempo delay is re-added live at consumption sites
  (`_stream_pcm_reanchor_select_delay`, `_get_anchor_delay_ms`,
  `_stream_get_delay_status`). This prevents stale pipeline bias when
  speed is restored after the cache was written.
- `smoothed_av_delay` tracks HW-only delay (same reason). Atempo is
  added live inside `stream_sync_av_delay()`.
- `last_good_delay_ms` update gate: guarded by `sensitive_phase`
  (true when `startup_hold_active`, `audio_start_pending`,
  `audio_resume_pending`, or seek in progress). During sensitive phases,
  `last_good` only updates when the delay streak reaches
  `STREAM_PCM_DELAY_STABLE_STREAK` (3). This prevents a transient
  startup/fallback value from polluting the cache before it is stable.
- On seek, `_stream_pcm_delay_memory_reset(reset_smoothed=0)` wipes
  `last_good` and LWMA history but preserves `smoothed_av_delay` as a
  warm start for the new position.

PCM heard_ts Interpolation
--------------------------
In `put_time` mode with valid delay, `heard_ts` is wall-clock
interpolated between audio writes:
  `heard_ts = _stream_interpolate_heard_ts(...)`
This eliminates the staircase artifact where `heard_ts` stayed flat
for 10-20ms between write chunks then jumped. The interpolation is
capped at the latest `audio_time - heard_delay` frontier (not an
unbounded predictor). Video scheduling in sfdec2 receives a
continuously-advancing value instead of steps.

AudioTrack PlaybackParams Speed Epochs
--------------------------------------
For plain PCM speed changes driven by AudioTrack PlaybackParams, `last_good`
is not used as the authoritative heard clock during the speed epoch.

Why:
- `audio_time` advances from committed writes and therefore moves in write
  quanta.
- `last_good_delay_ms` is intentionally conservative and can be stale across a
  speed change.
- Switching `last_good_delay_ms` after a write has already advanced
  `audio_time` can create a put_time discontinuity.

The speed-epoch clock is armed before `audio_interface_change_audio_speed()`:

  epoch_heard_ts = anchor_ts
  epoch_frames = fresh getPlaybackHeadPosition()
  epoch_rate = AudioTrack sample rate

Then heard time is derived from presented-frame progression:

  frames_delta = getPlaybackHeadPosition() - epoch_frames
  delta_media_ms = frames_delta * 1000 / epoch_rate
  heard_ts = epoch_heard_ts + RST_TO_TS_DELTA(delta_media_ms)

Current production policy reads playback head fresh on every epoch query.
Caching without interpolation was tested and caused perceptible stair-step
jitter during speed ramps. If this is optimized later, use linear interpolation
between real playhead samples and keep the unthrottled implementation as the
correctness baseline.

The dynamic AudioTrack delay estimator still matters outside this epoch and as
diagnostic/fallback evidence. It must report wall/output milliseconds when
PlaybackParams speed is active:

  delay_wall_ms = frames_pending * 1000 / (rate * speed)

not nominal media-frame duration:

  delay_media_ms = frames_pending * 1000 / rate

heard_ts < 0 clamp
------------------
For PCM in `put_time` mode, `heard_ts < 0` is only clamped to 0 when
`sink_ref_time > 0` (an anchor has already been established). When
`sink_ref_time <= 0` (buffer-fill phase), negative `heard_ts` is
propagated to the sfdec2 scheduler so early frames are paced relative
to when audio will actually be heard, producing smoother startup.

Notes
-----
- Outlier delays are rejected in favor of cached/last-good values.
- Once a real delay is observed, the estimator does not revert to static
  unless no dynamic value has ever been available.
- The diff metrics are control signals, not a direct lipsync meter.
- In put_time mode, sync uses heard_ts for anchoring and smoothed_av_delay
  for diff alignment (no heard_ts substitution in the diff path).
- Manual A/V delay is a user offset, not part of the core delay-estimation
  equations.
  - Positive delay is applied at final presentation scheduling in
    `codec_sfdec2.c` when building `render_ts_ns` for MediaCodec. The user target
    is slewed through an effective delay state to avoid fast-render bursts.
  - Negative delay is implemented as an audio-side PCM hold. It is unsupported
    for compressed passthrough because AVOS cannot insert decoded silence there.
- When timing is invalid and atempo is actively changing speed, heard_ts uses
  the atempo chain delay to keep speed-change anchoring latency-aware. This is
  separate from the hot-filter topology rule: atempo may remain in the PCM path
  at neutral 1.0x for seamless speed changes without forcing its synthetic
  neutral-speed delay into every heard-time estimate.
- On the current platform-timed path, stale delay is not used as a fresh
  reanchor source because doing so can cause visible catch-up bursts. PCM may
  retain last-good delay as a heard-time fallback under the state rules below.

Full State Machine (Delay + Anchoring)
--------------------------------------
Definitions:
- `delay_valid`: AudioTrack timing is trusted (timestamp/playhead passes validity checks).
- `last_good_delay`: most recent trusted dynamic delay (cached).
- `static_latency`: AudioTrack.getLatency().
- `anchor_delay`: delay used for anchoring (put_time / render_ts alignment).
- `heard_delay`: delay used for heard_ts only (audible-time estimate).

Rules:
0) PCM policy:
   - PCM should prefer dynamic AudioTrack timing when stable. Static latency is
     only a warmup/fallback anchor because PCM queued delay changes with buffer
     fill, resume, speed filtering, and device timing behavior.

1) Choose delay candidate (raw):
   - If `delay_valid`, use dynamic delay (timestamp or playhead).
   - Else if `last_good_delay` exists, use it as a candidate.
   - Else if `static_latency` > 0, use static.

2) Anchor delay selection:
   - If `delay_valid`, anchor_delay = current dynamic delay (or smoothed).
   - If dynamic evidence is unavailable, use the path-specific static/last-good
     fallback for heard-time estimation, but do not present it as fresh evidence.
   - In sfdec2 reanchor windows, prefer fresh sink `put_time` (`venc_put_time`)
     as the authoritative heard anchor; recompute heard time when it is stale.

3) Heard delay selection (heard_ts):
   - If `delay_valid`, heard_delay = anchor_delay (smoothed/dynamic).
   - If `delay_valid` is false, heard_delay = raw delay from AudioTrack
     (playhead/static). If atempo is active, include atempo chain delay so
     speed changes remain latency-aware.
   - Keep heard_ts calculation pure: do not suppress static delay inside
     `stream_get_heard_audio_ts()` to handle late audio startup.

3b) PCM late-audio startup guard:
   - If the delay source is static fallback and audio starts significantly ahead
     of early video, suppress video anchoring briefly in `stream_sync_video()`.
   - This keeps the heuristic as a video-release decision instead of mutating the
     audible-time estimate used by all anchors.

4) Mapping on speed change:
   - Default: anchor at `heard_audio_ts`.
   - With invalid delay, map using `current_time_ts` instead and
     defer sink re-anchoring (avoid fast catch-up).

5) Resume:
   - PCM free-runs while delay is invalid; when delay becomes valid
     (streak), a one-time rebase aligns to measured delay.
   - On the first resumed PCM write, AVOS may perform an invalid-delay rebase
     using static latency to avoid a large offset while AudioTrack timing warms
     up. At normal speed, if that rebase fired, the later measured-delay rebase
     is disarmed because it has caused visible snaps when sync is already near
     zero. Non-1x PCM keeps the measured-delay rebase armed.
   - Passthrough / AC3 recoding: static passthrough delay is considered valid
     immediately for mode 1 and as the mode 2 baseline. Video resume hold is
     released only after the first resumed audio write commits, avoiding anchors
     before post-resume compressed output has actually restarted.
- Playback-head availability:
  - PCM and passthrough mode 1 (IEC): playhead is used when valid.
  - Passthrough mode 2 (raw): playhead/timestamp evidence is treated
    conservatively. The scheduler uses normalized compressed-buffer latency as
    its baseline after the 250ms evidence window, with the platform/app policy
    retained for startup fallback. Current playhead/timestamp comparisons are
    diagnostic-only and do not change the selected Mode 2 delay or heard clock.
- Cached/throttled AudioTrack delay reads preserve validity when the last
  trusted source was playhead-based (`last_good_dynamic_valid`), so
  `cached(throttle)` does not immediately invalidate a newly trusted delay.
- `startup_hold` is not allowed to remain permanent on devices with
  frozen-but-successful `getTimestamp()` reporting. If timestamp-based
  convergence cannot occur, a recent playback-head fallback delay can be
  promoted to valid, and a timeout acts as a safety net.

Observability limits:
- AudioTrack delay APIs stop at the Android output boundary. They do not report
  downstream soundbar/AVR decode, DSP, ARC, or eARC latency.
- Internal diff convergence is therefore a scheduler consistency signal, not a
  physical lipsync proof. Route- or format-specific downstream delay must be
  represented as a user/route offset outside the core delay estimator.

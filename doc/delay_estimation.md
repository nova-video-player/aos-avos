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
- `pipeline_latency`: the conservative platform estimate used for diagnostics
  and selected passthrough policies. It is the maximum of track/platform
  latency and `system_latency + app_latency`.
- Static latency: a fallback selected delay used when stable dynamic evidence
  is unavailable. Static latency is not always the raw AudioTrack
  `getLatency()` value. For mode2 passthrough it is currently selected by
  codec policy: plain AC3/EAC3 uses `pipeline_latency`, while DTS/DTS-HD,
  TrueHD, and DDP/JOC use geometry/app latency based on Nvidia Shield and
  Google Streamer 4K testing.
- `selected_delay`: the delay actually subtracted from `audio_time` to derive
  heard time. It may come from dynamic AudioTrack evidence, last-good cache,
  geometry latency, or pipeline latency depending on path and stability.
- `mode2_playhead_audit`: diagnostic comparison between mode2 `fakeSize`
  logical writes and AudioTrack playhead/timestamp counters. The result is
  evidence only, not a live delay provider.

The core scheduler rule remains:

  heard_ts = audio_time - selected_delay

Measured evidence may update `selected_delay`, but must not become a separate
clock that continuously redefines `audio_time`.

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
  - `android_sync=1`: applied at final presentation scheduling in
    `codec_sfdec2.c` when building `render_ts_ns` for MediaCodec. The user
    target delay is slewed through an effective delay state (bounded per-frame
    step) to avoid fast-render bursts on large UI changes.
  - `android_sync=0`: keep sink anchors physical (`put_time` unchanged).
    The sync diff includes `s->av_delay`; negative delay (video earlier)
    is implemented as audio-side hold (silence insertion) in
    `stream_audio.c`.
- When timing is invalid and atempo is actively changing speed, heard_ts uses
  the atempo chain delay to keep speed-change anchoring latency-aware. This is
  separate from the hot-filter topology rule: atempo may remain in the PCM path
  at neutral 1.0x for seamless speed changes without forcing its synthetic
  neutral-speed delay into every heard-time estimate.
- For android_sync=0, if timing becomes invalid during steady playback,
  last-good delay is held for anchoring to avoid dropping latency
  compensation. For android_sync=1, stale delay is not used for anchoring
  to avoid visible catch-up bursts.

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
   - android_sync=0:
     - If `delay_valid`, anchor_delay = current dynamic delay (or smoothed).
     - If `delay_valid` becomes false during steady playback and last_good exists,
       hold `last_good_delay` as anchor_delay (prevents latency drop).
     - If no usable delay exists, anchor_delay = 0.
   - android_sync=1:
     - If `delay_valid`, anchor_delay = current dynamic delay (or smoothed).
     - If `delay_valid` is false, do NOT anchor on last_good/static (avoid catch-up bursts).
     - In sfdec2 reanchor windows, prefer fresh sink `put_time` (`venc_put_time`)
       as authoritative heard anchor; fallback to recomputed heard-time when stale.

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
   - android_sync=1 + invalid delay: map using `current_time_ts` instead and
     defer sink re-anchoring (avoid fast catch-up).

5) Resume:
   - android_sync=0: if delay invalid on first audio after resume, rebase to
     static latency; when delay becomes valid (streak), rebase to measured delay.
   - android_sync=1 PCM: free-run while delay invalid; when delay becomes valid
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
    conservatively. The scheduler uses platform static latency as baseline.
    When `enable_dynamic_audio_delay` and `stream_mode2_dynamic_delay` are
    enabled, stable AudioTrack evidence may add a capped, slewed, positive-only
    residual above static latency. Stream-level last-good fallback is not
    considered fresh sink evidence for this residual.
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

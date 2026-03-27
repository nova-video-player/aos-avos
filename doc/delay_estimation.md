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

Notes:
- playhead_ms is the output position derived from getPlaybackHeadPosition and is used only
  to compute delay in AudioTrack, not as a global A/V metric.
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
- When timing is invalid and atempo is active, heard_ts uses the atempo
  chain delay to keep speed-change anchoring latency-aware.
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

4) Mapping on speed change:
   - Default: anchor at `heard_audio_ts`.
   - android_sync=1 + invalid delay: map using `current_time_ts` instead and
     defer sink re-anchoring (avoid fast catch-up).

5) Resume:
   - android_sync=0: if delay invalid on first audio after resume, rebase to
     static latency; when delay becomes valid (streak), rebase to measured delay.
   - android_sync=1 PCM: free-run while delay invalid; when delay becomes valid
     (streak), a one-time rebase aligns to measured delay.
   - android_sync=1 passthrough / AC3 recoding: static passthrough delay is
     considered valid immediately, but video resume hold is released only after
     the first resumed audio write commits. This avoids anchoring before
     post-resume compressed output has actually restarted.
- Playback-head availability:
  - PCM and passthrough mode 1 (IEC): playhead is used when valid.
  - Passthrough mode 2 (raw): playhead/timestamp are unreliable; static only.
  - Cached/throttled AudioTrack delay reads preserve validity when the last
    trusted source was playhead-based (`last_good_dynamic_valid`), so
    `cached(throttle)` does not immediately invalidate a newly trusted delay.

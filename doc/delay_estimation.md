Delay Estimation (AudioTrack)
=============================

Overview
--------
AudioTrack delay is estimated from three sources:
- Dynamic timestamp (AudioTrack.getTimestamp).
- Playback head position (getPlaybackHeadPosition).
- Static latency (AudioTrack.getLatency).

The goal is to track real output latency and avoid reinjecting static
latency once a valid dynamic delay has been observed.

Equations
---------
All times are in milliseconds (TS domain) unless noted.

Core A/V diff (control):
  diff = (video_time - audio_time) + sync_delay + av_delay

Audio pipeline delay (ms):
  sync_delay = codec_delay + filter_delay + sink_delay - video_delay

Heard time (estimated):
  heard_ts = audio_time - sync_delay - av_delay

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
- Playback-head availability:
  - PCM and passthrough mode 1 (IEC): playhead is used when valid.
  - Passthrough mode 2 (raw): playhead/timestamp are unreliable; static only.

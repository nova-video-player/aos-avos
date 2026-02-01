Stutter Investigation Notes
===========================

Goal
----
Reduce visible stutter on Android devices where AudioTrack timing is unreliable
(e.g., Google Streamer 4K returning `framePosition=0` or sparse timestamps).
Cover both android_sync=0 (sfdec2 pacing) and android_sync=1 (MediaCodec pacing).

Key Symptoms Observed
---------------------
- Repeated "timing unavailable" messages from audio timing.
- Large wait spikes (e.g., 60-100ms) in video sink pacing despite 60fps content.
- Re-anchor spam and pacing resets when AudioTrack timing jumps or is missing.

Changes Implemented
-------------------
1) Centralized anchor delay with last-good/static fallback
   - Files: `Source/stream_sync.c`, `Include/stream.h`,
     `Include/audio_interface.h`, `Source/audio_interface.c`,
     `Source/audio_interface_audiotrack_java.c`
   - `stream_get_heard_audio_ts()` uses `_get_anchor_delay_ms()` so anchors are
     derived from smoothed/last-good/static delay consistently.
   - When dynamic timing is unstable, reuse last-good delay; otherwise fall back
     to playback-head or static latency on Android.

2) android_sync=1 single-control anchoring (MediaCodec pacing)
   - File: `Source/codec_sfdec2.c`
   - Always provide `render_ts_ns` to MediaCodec; no local wait/drop pacing.
   - Initialize render offset from static latency at startup, then slew toward
     the best-delay anchor once timing stabilizes.
   - `stream_sync_video()` bypasses waits under android_sync=1; the sink owns pacing.
   - `sfdec2_android_sync_on_pause()` shifts the render offset by the paused gap
     so resume does not fast‑forward to catch the wall clock.

3) android_sync=0 reanchor/pacing guards (sfdec2 pacing)
   - File: `Source/codec_sfdec2.c`
   - Drift-based reanchor requires a streak; grace windows suppress resets
     during warmup/speed changes.
   - Drop thresholds are relaxed for steady 1.0x playback.
   - `put_time_mode` uses heard‑audio anchors but leaves pacing to the sink.

4) Startup delay strategy (android_sync=1)
   - File: `Source/codec_sfdec2.c`
   - Default: use a static-latency render offset for first frames; avoid hold/drop loops.
   - Passthrough=2: hold video until audio_time is valid, then apply residual static
     latency (avoid double‑counting the hold). Slew is event-driven only.
   - Transition to the best-delay anchor via a slow slew when timing stabilizes.

5) Speed-change anchoring when timing is invalid
   - File: `Source/stream.c`
   - For android_sync=1, if AudioTrack delay is invalid, fall back to
     `current_time_ts` for `timeline_map_apply` and defer sink re-anchoring.
     This avoids “fast catch-up” bursts caused by stale `heard_audio_ts`.

6) Delay-aware heard-time when timing is invalid
   - File: `Source/stream_sync.c`
   - When atempo is active and delay is invalid, include the atempo chain delay
     in heard-time so speed changes remain latency-aware.

7) Hold last-good delay during steady playback (android_sync=0)
   - File: `Source/stream_sync.c`
   - If delay validity drops mid-playback, keep last-good delay for anchoring to
     avoid losing latency compensation.

Notes / Potential Follow-ups
----------------------------
- If stutter persists, log render_ts deltas and the dynamic-delay ramp.

How This Helps Stutter
----------------------
These changes remove two core stutter triggers:
- Unstable anchors (TS constant but WC anchor refreshed) no longer cause
  MediaCodec wait stalls under android_sync=1.
- Drift and jitter no longer trigger repeated re-anchors or aggressive drops
  under android_sync=0.

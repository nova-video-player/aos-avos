# Android Frame Timing Mode

## Purpose

Enabling `android_sync` hands video pacing to the Android `MediaCodec` renderer instead of the AVOS video sink. The goal is to let the platform handle frame queuing/dropping while the player stays in the time‑scaled (`ts`) domain used for audio speed changes.

## How the Sink Delegates Pacing (`Source/codec_sfdec2.c`)

- The sink always calls `sfdec_buf_render` with a non‑zero `render_ts_ns`.
- `render_ts_ns` is derived from a single render offset: `render_ts_ns = f->time * 1e6 + render_offset_ns`.
- The render offset is initialized once at startup:
  - Prefer `smoothed_av_delay` when available.
  - Otherwise use the unified anchor delay (playback‑head or static latency).
- The offset is **slewed** toward a new target only on explicit events
  (seek/resume/speed) to avoid jitter‑driven reanchors.

In short, the sink never blocks or drops; MediaCodec schedules frames using the provided timestamps.

## Frame Snapping and Wall-Clock Mapping (`external/android/libsfdec/sfdec_ndkmediacodec.cpp`)

When `render_ts_ns` is provided, MediaCodec uses it directly for presentation.
`sfdec_ndkmediacodec.cpp` still snaps timestamps for consistency across speed
changes, but the internal `(start_off, start_monotonic)` anchoring is bypassed
because `render_ts_ns > 0` is always supplied.

## Audio Speed Interaction

- The parser feeds MediaCodec timestamps that are already scaled by the active audio speed (`ts` domain). Because `Δts = Δwc`, the wall-clock projection remains valid at any speed.
- When the app changes audio speed (or resumes playback with a remembered non-1.0x speed), `stream_set_av_speed` caches the requested ratio on the `STREAM` object and ensures the active decoder receives it via `sfdec_set_playback_speed` (`Source/stream.c:534-566`, `Source/stream_video.c:608-615`, `Source/codec_sfdec2.c:980-984`). The MediaCodec helper stores the new numerator/denominator and the snapping logic starts using the updated effective frame rate on the very next frame.
- After the notification, the player performs a seek so all subsequent frames adopt the new timestamps. No additional MediaCodec reset is required.

## Operational Notes and Caveats

- The render offset is initialized from a stable fallback when timing is
  unreliable; this provides a consistent A/V alignment at startup.
- Subsequent corrections are event‑driven (seek/resume/speed) and applied via
  slow slew to avoid visible acceleration or stutter.
- Accurate `video->frame_rate_{num,den}` metadata is important. Bad values yield
  incorrect snapping after a speed change, causing jitter in scheduled timestamps.

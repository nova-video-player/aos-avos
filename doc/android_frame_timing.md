# Android Frame Timing Mode

## Purpose

The current `sfdec2` path always uses the restored platform-timed release engine
(historically named `android_sync=1`). AVOS computes a monotonic presentation
deadline for every decoded frame and passes it to Android `MediaCodec`, while all
media timestamps remain in the time-scaled (`ts`) domain used for audio speed
changes. There is no current runtime `android_sync=0` branch in `sfdec2`.

## How the Sink Delegates Pacing (`Source/codec_sfdec2.c`)

- The sink always calls `sfdec_buf_render` with a non‑zero `render_ts_ns`.
- `render_ts_ns` is derived from a single render offset plus user delay:
  `render_ts_ns = f->time * 1e6 + render_offset_ns + effective_av_delay_ns`.
- The render offset is initialized from `_get_render_heard_ts()` when audio time
  exists. That helper prefers an audio-thread `put_time` sample no older than
  100ms and otherwise recomputes `stream_get_heard_audio_ts()`.
- If audio has not started, initialization uses the static anchor delay once and
  reanchors to heard audio when it becomes available.
- For reanchor windows, the sink prefers a **fresh** audio-thread
  `put_time` anchor (`venc_put_time`) and falls back to recomputed
  `stream_get_heard_audio_ts()` only when `put_time` is stale. This avoids
  cross-thread heard-time skew at seek/resume boundaries.
- For PCM and mode 1, the offset is **slewed** toward a new target only on
  explicit events (seek/resume/speed/hard discontinuity). Direct mode 2 normally
  keeps its render offset stable after initialization. On entry to or exit from
  the validated dynamic presentation clock, it slews toward the centralized
  heard-time target by at most 5ms per frame.
- Manual A/V delay (`s->av_delay`) is also slewed in the render path through
  `effective_av_delay` (bounded per-frame step) so large UI jumps do not create
  a burst of ASAP renders ("fast video" transient).

The video thread limits submission to a 200ms lookahead window and releases a
frame without rendering when it is already more than 200ms late. Frames inside
that window are submitted to MediaCodec with the computed deadline.

## Frame Snapping and Wall-Clock Mapping (`Source/codec_sfdec2.c`)

`_snap_timestamp_ns()` rounds the frame TS to the nearest frame interval using
the current frame rate and playback speed. The sink then adds `render_offset_ns`
and the effective user video delay. `sfdec_buf_render()` forwards the resulting
non-zero `render_ts_ns` to MediaCodec for timed release.

## Audio Speed Interaction

- The parser feeds MediaCodec timestamps that are already scaled by the active audio speed (`ts` domain). Because `Δts = Δwc`, the wall-clock projection remains valid at any speed.
- When the app changes audio speed (or resumes playback with a remembered non-1.0x speed), `stream_set_av_speed` caches the requested ratio on the `STREAM` object and ensures the active decoder receives it via `sfdec_set_playback_speed` (`Source/stream.c:534-566`, `Source/stream_video.c:608-615`, `Source/codec_sfdec2.c:980-984`). The MediaCodec helper stores the new numerator/denominator and the snapping logic starts using the updated effective frame rate on the very next frame.
- After the notification, the player performs a seek so all subsequent frames adopt the new timestamps. No additional MediaCodec reset is required.

## Operational Notes and Caveats

- The render offset is initialized from a stable fallback when timing is
  unreliable; this provides a consistent A/V alignment at startup.
- Subsequent corrections are event‑driven (seek/resume/speed) and applied via
  slow slew to avoid visible acceleration or stutter.
- For `passthrough=2`, the renderer uses the centralized Mode 2 interpolator,
  preferably through a fresh `put_time`. Initial and seek reanchors clamp an
  implausible forward lead and prevent a backward seek from anchoring behind the
  current video frame.
- On the validated raw AC3/44.1 kHz route, trusted asynchronous `AudioTimestamp`
  evidence dynamically bounds that interpolator. The renderer observes clock
  entry/exit and slews its existing offset; it does not create a second audio
  clock.
- Pause preserves the Mode 2 audio phase and shifts an established render offset
  by the paused wall duration. It retains the compressed ledger while resetting
  the presentation-observation epoch. Seek and mid-playback track recreation
  explicitly seed an empty compressed track at the submitted frontier minus
  fixed downstream latency.
- Audio seek preroll waits for the video decoder to reach the epoch-tagged seek
  target. Renderer reanchoring and this video-target handshake are separate from
  audio latency estimation.
- Accurate `video->frame_rate_{num,den}` metadata is important. Bad values yield
  incorrect snapping after a speed change, causing jitter in scheduled timestamps.

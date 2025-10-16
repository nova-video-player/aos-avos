# Android Frame Timing Mode

## Purpose

Enabling `android_sync` hands video pacing to the Android `MediaCodec` renderer instead of the AVOS video sink. The goal is to let the platform handle frame queuing/dropping while the player stays in the time‑scaled (`ts`) domain used for audio speed changes.

## How the Sink Bypass Works (`Source/codec_sfdec2.c`)

- `videosink_thread` normally decides whether to sleep, render, or drop based on `blit_duration = frame->blit_time - venc_time`.
- When `android_sync` is set, the code shifts `blit_duration` by `-200ms` (`Source/codec_sfdec2.c:404-410`). The negative bias keeps the `blit_duration > 0` branch from running, so the sink never blocks waiting for wall clock alignment.
- The late-frame drop logic is also skipped because it is guarded by `!android_sync` (`Source/codec_sfdec2.c:437-448`). Every decoded frame is forwarded directly to `sfdec_buf_render`.
- The actual render call is made with `asap = 0` (`Source/codec_sfdec2.c:459-466`), signalling that MediaCodec should schedule presentation based on the timestamps it already knows.

In short, the sink’s tests for “wait” and “drop” are bypassed; MediaCodec becomes responsible for pacing.

## Frame Snapping and Wall-Clock Mapping (`external/android/libsfdec/sfdec_ndkmediacodec.cpp`)

When the sink delegates, `sfdec_buf_render` performs two key tasks before posting a buffer with `AMediaCodec_releaseOutputBufferAtTime`:

1. **Frame snapping** – The MediaCodec output timestamp (`timestamp_us`) is in the time-scaled domain because the parser already applied `RST_TO_TS`. The helper multiplies the stream frame rate by the current playback speed (`sfdec->playback_speed_*`) and snaps `timestamp_us` onto that grid (`external/android/libsfdec/sfdec_ndkmediacodec.cpp:347-365`). This keeps per-frame spacing consistent after a speed change.
2. **Wall-clock projection** – The code maintains a `(start_off, start_monotonic)` pair. It projects the snapped timestamp into `CLOCK_MONOTONIC` by `ts = timestamp_us*1000 - start_off + start_monotonic` (`external/android/libsfdec/sfdec_ndkmediacodec.cpp:376-414`). Large gaps or drift (>500 ms) reset the anchors, and late frames push `start_monotonic` back by 100 ms to re-align to audio.

The release path chooses `releaseOutputBufferAtTime` unless a reset was just triggered, in which case it falls back to immediate rendering.

## Audio Speed Interaction

- The parser feeds MediaCodec timestamps that are already scaled by the active audio speed (`ts` domain). Because `Δts = Δwc`, the wall-clock projection remains valid at any speed.
- When the app changes audio speed (or resumes playback with a remembered non-1.0x speed), `stream_set_av_speed` caches the requested ratio on the `STREAM` object and ensures the active decoder receives it via `sfdec_set_playback_speed` (`Source/stream.c:534-566`, `Source/stream_video.c:608-615`, `Source/codec_sfdec2.c:980-984`). The MediaCodec helper stores the new numerator/denominator and the snapping logic starts using the updated effective frame rate on the very next frame.
- After the notification, the player performs a seek so all subsequent frames adopt the new timestamps. No additional MediaCodec reset is required.

## Operational Notes and Caveats

- The `-200 ms` bias assumes MediaCodec keeps roughly 100 ms of internal lead; if that assumption drifts, the sink will still refuse to wait, so any underrun must be handled by MediaCodec’s own drop heuristics.
- Because the drop path is disabled in the sink, catastrophic decoder slowness can only be mitigated by the `start_monotonic += 100 ms` back-off inside MediaCodec, which may introduce visible judder before playback recovers.
- Accurate `video->frame_rate_{num,den}` metadata is important. Bad values yield incorrect snapping after a speed change, causing jitter in the scheduled timestamps.

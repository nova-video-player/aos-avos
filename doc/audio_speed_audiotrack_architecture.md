# Audio Speed Change Architecture

## Overview

This document details the architecture for audio speed changes in the AVOS player. The implementation relies on a time-scaled (`ts`) internal clock, anchored conversions between the real-stream and time-scaled domains, and a sink-side synchronization mechanism. In the AudioTrack PlaybackParams path, speed changes are applied seamlessly by retargeting the timeline mapping without a self-seek. (The atempo path may optionally use a frame-accurate seek; see the atempo architecture doc.)

## Time Domains

There are three fundamental time domains in the implementation:

-   **`wc` (Wall Clock):** The system's monotonic clock (`clock_gettime()`). It progresses linearly and is independent of playback speed. It is the ground truth for real-world time.

-   **`rst` (Real Stream Time):** The original media stream's own timeline, representing the actual position within the media file (e.g., 0 to 10 minutes). It is primarily used for UI display and for seeking.

-   **`ts` (Time-Scaled):** This is the **primary internal time domain** for the core player logic. The parser creates a "compressed" or "expanded" timeline where all timestamps are scaled by the `audio_speed`. At 2.0x speed, a frame at 60s (`rst`) will have a timestamp of 30s (`ts`). All core variables like `s->video_time` and `frame->time` are in this domain.

It is important to understand that wall clock progresses at ts rate: `delta_wc = delta_ts` thus adding ts domain increments to wc domain is intended and does not result into a mismatch.

### Key Relationship

`ts = rst / audio_speed` and `rst = ts * audio_speed`. Absolute conversions use the `RST_TO_TS_TIME` / `TS_TO_RST_TIME` helpers, while durations use the `RST_TO_TS_DELTA` / `TS_TO_RST_DELTA` helpers so anchors are only applied when needed.

## Core Architecture

### Parser Interaction (Creating the `ts` Domain)

The `ts` domain is created at the earliest possible stage: the parser. In `stream_parser_ffmpeg.c`, every packet's timestamp (`pts`, `dts`) and `duration` is immediately scaled after being read from the source file:

```c
// Simplified from the parser
packet.pts = RST_TO_TS_TIME(packet.pts, int64_t);
packet.dts = RST_TO_TS_TIME(packet.dts, int64_t);
packet.duration = RST_TO_TS_DELTA(packet.duration, int64_t);
```

This means that any component that receives data from the parser (e.g., `cdata->time`, `frame->time`, `s->video_time`) operates in the `ts` domain.

### Timeline Mapping (Anchors)

Speed changes no longer flush the pipeline. Instead, the player maintains an anchored mapping between `rst` and `ts`:

- `timeline_map_apply(rst_anchor, ts_anchor, speed)` installs a new piecewise-linear mapping that preserves continuity at the current playback position.
- `rst_to_ts_time` / `ts_to_rst_time` convert absolute timestamps using the anchors.
- `rst_to_ts_delta` / `ts_to_rst_delta` rescale pure durations without touching the anchors.

Whenever `stream_set_av_speed` succeeds (or the audio hardware reports a quantised ratio), the current playback position is captured and used as the new anchor so in-flight buffers keep their ordering.
This AudioTrack path intentionally avoids seek-based realignment; the optional frame‑accurate seek is restricted to the atempo path when explicitly enabled.

### AudioTrack PlaybackParams Speed-Epoch Clock

Plain PCM AudioTrack PlaybackParams speed changes have one extra clock rule.
During an in-flight speed ramp, the normal PCM heard clock:

```text
heard_ts = audio_time - last_good_delay_ms
```

is not authoritative enough. `audio_time` advances in write quanta, while
`last_good_delay_ms` is a stability cache that can lag the hardware state
across speed epochs. Updating `last_good_delay_ms` after a write has already
advanced `audio_time` can introduce a discontinuity instead of removing one.

For the AudioTrack PlaybackParams path, AVOS therefore arms a speed-epoch
checkpoint at every speed change, including a return to 1.0x:

1. Compute `anchor_ts`, the same continuity anchor used for the video sink.
2. Before calling `audio_interface_change_audio_speed()`, read a fresh
   `getPlaybackHeadPosition()` sample.
3. Store:
   - `at_speed_epoch_heard_ts = anchor_ts`
   - `at_speed_epoch_presented_frames = playback_head`
   - `at_speed_epoch_rate = AudioTrack sample rate`
   - `at_speed_epoch_speed = requested speed`, then patch it to the hardware
     read-back speed after the PlaybackParams call returns.
4. While the epoch is active, derive heard time from the presented-frame delta:

```text
frames_delta = current_playback_head - at_speed_epoch_presented_frames
delta_media_ms = frames_delta * 1000 / at_speed_epoch_rate
heard_ts = at_speed_epoch_heard_ts + RST_TO_TS_DELTA(delta_media_ms)
```

This is the same checkpoint idea used by players that derive the audible media
position from the hardware playhead. It makes speed changes continuous in the
clock that matters to the video sink, instead of relying on write timing or a
cached delay value.

The epoch is cleared on seek, flush, or stop. It is not cleared merely because
the requested speed returns to 1.0x; the return to 1.0x is itself a speed epoch
and must preserve continuity from the previous hardware-rate segment.

The current implementation reads `getPlaybackHeadPosition()` fresh on every
epoch heard-clock query. A stale 10ms cache was observed to reintroduce
perceptible stair-step jitter during speed ramps. A future optimization may
interpolate between less frequent playhead samples, but that should be a
separate correctness-neutral change.

### A/V Synchronization and Video Pacing

Audio is the master clock. Video synchronization is achieved through a clever pacing mechanism inside the video sink (`stream_sink_video_android.c`).

-   The video sink's rendering thread maintains its own wall-clock timer, `venc_time`, which tracks elapsed `wc` time since the last flush.
-   When a video frame is ready to be displayed, its `blit_time` (which is a `ts` value) is compared against the sink's `venc_time` (`wc` value).
-   `blit_duration = frame->blit_time - venc_time;`
-   This subtraction between two different time domains is intentional. The resulting `blit_duration` is not a true duration, but a pacing value used in a feedback loop to adjust the sleep time between frames, ensuring the rate of `venc_time` (`wc`) matches the rate of `blit_time` (`ts`).

### Seeking (`_stream_seek_real`)

Seeking uses the `rst` domain as its input.
1. The UI provides a seek target in **RST**.
2. `_stream_seek_real` receives this `rst` value and passes it to the parser.
3. The parser seeks to the requested `rst` keyframe in the media file.
4. When `_parse_once` reads the new packets from this position, it immediately scales their timestamps to the **`ts`** domain, and playback resumes correctly.

### Metadata

Container-level metadata like `duration` and `start_time` are read in their original **`rst`** domain. They are converted to `ts` on-the-fly when needed for calculations against the internal `ts` clocks.

## Time Domain Variable Reference

| Variable | Domain | Use and Explanation |
|---|---|---|
| `s->video_time` | `ts` | The current video playback time in the time-scaled domain. |
| `s->audio_time` | `ts` | The current audio playback time in the time-scaled domain. |
| `frame->time` | `ts` | The timestamp of a video frame in the time-scaled domain. |
| `cdata->time` | `ts` | Timestamp of a data chunk from the parser in the time-scaled domain. |
| `s->cdata_now.time`| `ts` | The timestamp of the current data chunk being processed. |
| `sc.time` | `rst` | Seek result time from the parser; used to reset stream state before TS scaling resumes. |
| `frame->duration` | `ts` | The scaled duration of a single video frame. |
| `frame->blit_time`| `ts` | The target presentation time for a video frame, numerically comparable to WC. |
| `venc_time` | `wc` | The video sink's internal wall-clock timer, used for pacing against `blit_time`. |
| `s->delay` | `ts` | The smoothed A/V difference, calculated and stored as a `ts` duration. |
| `s->av_delay` | `rst` | A user-configured A/V offset, in real-world milliseconds. |
| `s->video->msPerFrame` | `rst` | The unscaled, real-world duration of a single video frame. |
| `s->audio->bytesPerSec` | N/A | Unscaled property: The data rate of the audio stream in bytes per second. |
| `s->video->bytesPerSec` | N/A | Unscaled property: The data rate of the video stream in bytes per second. |
| `s->audio->bytesPerFrame`| N/A | Unscaled property: The size of a single audio frame in bytes. |
| `s->sink_ref_time`| `wc` | A wall-clock anchor time, used to align the `ts` and `wc` timelines. |
| `s->vid_ref_time` | `ts` | A time-scaled anchor timestamp, used to align the `ts` and `wc` timelines. |
| `stream_get_current_time()` | `rst` | **Returns** the current playback position in `rst` for UI purposes (converts from `s->video_time`). |
| `stream_seek_time()` | `rst` | **Accepts** a seek position in `rst` from the UI. |
| `s->duration` | `rst` | The total duration of the media, stored in `rst`. |
| `_get_audio_time` | `ts` | Stream function performs `rst` to `ts` domain conversion. |
| `_get_video_time` | `ts` | Stream function performs `rst` to `ts` domain conversion. |
| `at_speed_epoch_heard_ts` | `ts` | Heard-audio checkpoint used during AudioTrack PlaybackParams speed epochs. |
| `at_speed_epoch_presented_frames` | sample frames | AudioTrack playback-head position at the speed-epoch checkpoint. |
| `at_speed_epoch_rate` | Hz | AudioTrack sample rate used to convert presented-frame deltas to media milliseconds. |

**Notes:**
*   Functions that run once at startup, like metadata parsing (`_parse_format`), operate in the `rst` domain before any speed scaling is applied.
*   `stream_parser_guess_msPerFrame` is a fallback called during initialization when speed is 1.0, so it calculates `msPerFrame` in the `rst` domain.
*   Dynamic AudioTrack delay must be expressed as wall/output milliseconds. When
    PlaybackParams speed is active, a pending frame count converted as
    `frames_pending * 1000 / rate` is media duration, not wall time; the
    wall-delay form is `frames_pending * 1000 / (rate * speed)`.
*   During AudioTrack PlaybackParams speed epochs, the playhead checkpoint
    replaces delay-cache-derived heard time. Outside those epochs, delay
    evidence remains a delay provider, not a second audio clock.

## Design Philosophy

The seamless approach for applying speed changes prioritizes:

1. **Continuity of buffered data:** Anchors guarantee that timestamps never jump backwards, so decoder and renderer queues stay valid without flushing.
2. **Simplicity of call sites:** Every conversion goes through the same helpers, eliminating special cases for “speed == 1.0”.
3. **Hardware compatibility:** The actual ratio reported by the audio hardware (even if quantised) is fed back into the mapping so MediaCodec pacing continues to match.

This architecture delivers seamless speed changes while maintaining clear separation between time domains.

### Time Domain Equivalence: `ts` vs `wc`

A key aspect of the architecture is the numerical equivalence between `ts` (Time-Scaled) and `wc` (Wall-Clock) **durations**. This is proven as follows:

1.  By definition of playback speed, the Wall-Clock time elapsed for a given Real Stream Time duration is: `Δwc = Δrst / audio_speed`.
2.  By definition of the parser's scaling, the Time-Scaled duration for a given Real Stream Time duration is: `Δts = Δrst / audio_speed`.
3.  Therefore, it is unequivocally true that **`Δts = Δwc`**.

This equivalence is crucial. It means that a duration of 100ms in `ts` is numerically equal to a duration of 100ms in `wc`. This confirms that the video sink's clock estimator logic is mathematically sound:

`venc_time = venc_put_time + (atime() - venc_ref_time)`

This correctly estimates the current `ts` by adding the elapsed `wc` duration to the last reference `ts` timestamp. The error calculation `blit_duration = frame->blit_time - venc_time` is also sound, as it compares two values in the same, correct `ts` domain.

#### The `android_sync = 1` Strategy

When the `android_sync` flag is enabled, the synchronization strategy changes completely, bypassing the sink's internal wait/drop logic and delegating frame pacing directly to the Android `MediaCodec` framework.

1.  **Delegation:** The `videosink_thread` bypasses local wait/drop pacing and delegates scheduling to `sfdec`/`MediaCodec`.

2.  **`sfdec` Timestamp Calculation:** The `sfdec` layer computes `render_ts_ns` for `AMediaCodec_releaseOutputBufferAtTime()`. The render time is derived from the current TS anchor and a wall‑clock reference so MediaCodec can pace frames in wall clock while respecting the TS timeline (including audio speed).

3.  **Irrelevant `blit_time`:** In this mode, the sink’s `blit_duration` pacing is intentionally bypassed; the MediaCodec render timestamps are the authoritative schedule.

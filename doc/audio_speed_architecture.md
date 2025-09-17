# Audio Speed Change Architecture

## Overview

This document details the architecture for audio speed changes in the AVOS player. The implementation relies on a time-scaled (`ts`) internal clock and a unique video synchronization mechanism in the sink. For speed changes themselves, a seek-based approach is used to ensure clean state transitions by flushing buffers.

## Time Domains

There are three fundamental time domains in the implementation:

-   **`wc` (Wall Clock):** The system's monotonic clock (`clock_gettime()`). It progresses linearly and is independent of playback speed. It is the ground truth for real-world time.

-   **`rst` (Real Stream Time):** The original media stream's own timeline, representing the actual position within the media file (e.g., 0 to 10 minutes). It is primarily used for UI display and for seeking.

-   **`ts` (Time-Scaled):** This is the **primary internal time domain** for the core player logic. The parser creates a "compressed" or "expanded" timeline where all timestamps are scaled by the `audio_speed`. At 2.0x speed, a frame at 60s (`rst`) will have a timestamp of 30s (`ts`). All core variables like `s->video_time` and `frame->time` are in this domain.

It is important to understand that wall clock progresses at ts rate: `delta_wc = delta_ts` thus adding ts domain increments to wc domain is intended and does not result into a mismatch.

### Key Relationship

`ts = rst / audio_speed` and `rst = ts * audio_speed`. The `RST_TO_TS` and `TS_TO_RST` macros are used for these conversions.

## Core Architecture

### Parser Interaction (Creating the `ts` Domain)

The `ts` domain is created at the earliest possible stage: the parser. In `stream_parser_ffmpeg.c`, every packet's timestamp (`pts`, `dts`) and `duration` is immediately scaled after being read from the source file:

```c
// Simplified from the parser
packet.pts = RST_TO_TS(packet.pts, int64_t);
packet.dts = RST_TO_TS(packet.dts, int64_t);
packet.duration = RST_TO_TS(packet.duration, int64_t);
```

This means that any component that receives data from the parser (e.g., `cdata->time`, `frame->time`, `s->video_time`) operates in the `ts` domain.

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
| `sc.time` | `ts` | Timestamp of a `STREAM_CHUNK`, typically set during parsing or seeking. |
| `frame->duration` | `ts` | The scaled duration of a single video frame. |
| `frame->blit_time`| `wc` | The target wall-clock presentation time for a video frame, sent to the sink. |
| `venc_time` | `wc` | The video sink's internal wall-clock timer, used for pacing against `blit_time`. |
| `s->delay` | `ts` | The smoothed A/V difference, calculated and stored as a `ts` duration. |
| `s->av_delay` | `rst` | A user-configured A/V offset, in real-world milliseconds. |
| `s->video->msPerFrame` | `rst` | The unscaled, real-world duration of a single video frame. |
| `s->audio->bytesPerSec` | N/A | Unscaled property: The data rate of the audio stream in bytes per second. |
| `s->video->bytesPerSec` | N/A | Unscaled property: The data rate of the video stream in bytes per second. |
| `s->audio->bytesPerFrame`| N/A | Unscaled property: The size of a single audio frame in bytes. |
| `s->sink_ref_time`| `wc` | A wall-clock anchor time, used to align the `ts` and `wc` timelines. Note: usage is inconsistent in the code. |
| `s->vid_ref_time` | `ts` | A time-scaled anchor timestamp, used to align the `ts` and `wc` timelines. |
| `stream_get_current_time()` | `rst` | **Returns** the current playback position in `rst` for UI purposes (converts from `s->video_time`). |
| `stream_seek_time()` | `rst` | **Accepts** a seek position in `rst` from the UI. |
| `s->duration` | `rst` | The total duration of the media, stored in `rst`. |
| `_get_audio_time` | `ts` | Stream function performs `rst` to `ts` domain conversion. |
| `_get_video_time` | `ts` | Stream function performs `rst` to `ts` domain conversion. |

**Notes:**
*   Functions that run once at startup, like metadata parsing (`_parse_format`), operate in the `rst` domain before any speed scaling is applied.
*   `stream_parser_guess_msPerFrame` is a fallback called during initialization when speed is 1.0, so it calculates `msPerFrame` in the `rst` domain.
*   Sync delays like `codec_delay` or audiotrack `system_delay` are constant hardware chain delays (`rst`) that are invariant with audio speed.

## Design Philosophy

The seek-based approach for applying speed changes prioritizes:

1. **Reliability over Seamlessness:** Clean state transitions prevent timing artifacts.
2. **Simplicity over Complexity:** Reduces maintenance burden.
3. **Hardware Compatibility:** Works reliably by using standard seek/flush mechanisms.

This architecture ensures robust audio speed changes while maintaining clear separation between time domains.

# Audio Speed Change Architecture

## Overview

This document details the architecture for audio speed changes in the AVOS player. The implementation uses a seek-based approach that ensures clean state transitions by flushing buffers and reinitializing the media pipeline when speed changes occur.

The core of the architecture relies on three distinct time domains and a clear set of rules for converting between them at the boundaries of different components.

## Time Domains

There are three fundamental time domains in the implementation:

-   **`wc` (Wall Clock):** This is the system's monotonic clock (`atime()` or `clock_gettime()`). It progresses linearly and is independent of playback speed. It is the ground truth for all real-world timing, such as scheduling a frame for display.

-   **`rst` (Real Stream Time):** This is the playback time as perceived by the user, representing the actual position within the media file. For example, if a video is 10 minutes long, the `rst` will go from 0 to 10 minutes, regardless of the playback speed. It is primarily used for UI display and seeking.

-   **`ts` (Time-Scaled):** This is the primary internal time domain for the core player logic. The parser creates a "compressed" or "expanded" timeline where timestamps are scaled by the playback speed. At 2.0x speed, a frame at 60s (`rst`) will have a timestamp of 30s (`ts`). This allows the sync and decoding logic to operate without needing to know the current speed, but requires careful conversions at the boundaries.

### Key Relationship

`ts = rst / audio_speed` and `rst = ts * audio_speed`. The `RST_TO_TS` and `TS_TO_RST` macros are used for these conversions.

## Core Architecture

### Parser Interaction (Creating the `ts` Domain)

The `ts` domain is created at the earliest possible stage: the parser. In `_parse_once()` in `Source/stream_parser_ffmpeg.c`, every packet's timestamp (`pts`, `dts`) and `duration` is immediately scaled after being read from the source file:

```c
// Simplified from _parse_once()
packet.pts = RST_TO_TS(packet.pts, int64_t);
packet.dts = RST_TO_TS(packet.dts, int64_t);
packet.duration = RST_TO_TS(packet.duration, int64_t);
```

This means that any component that receives data from the parser (e.g., `cdata->time`, `frame->time`, `s->video_time`) operates in the `ts` domain.

### A/V Synchronization (`stream_sync.c`)

The A/V sync logic operates almost entirely in the `ts` domain.

-   The core clock variables, `s->video_time` and `s->audio_time`, are both `ts` timestamps.
-   The calculated difference, `s->delay`, is a `ts` duration.
-   **Boundary Conversions:**
    -   When comparing the internal `ts` delay against a fixed, real-world threshold (e.g., to decide if a frame should be dropped), the `ts` delay is converted to `rst`: `if (TS_TO_RST(s->delay, int) > threshold_rst)`.
    -   When accounting for physical buffer delays (like the audio sink's buffer depth, which is an `rst` value), the `rst` delay is converted to `ts` before being used in calculations with other `ts` variables: `delta_ts + RST_TO_TS(delay_rst, int)`.

### Seeking (`_stream_seek_real`)

Seeking is straightforward with the corrected time domain handling:
1. The UI provides a seek target in **RST**.
2. `_stream_seek_real` in `stream_video.c` receives this RST value.
3. It passes the RST value directly to the parser's `seek_time` function without any scaling.
4. The parser seeks to the requested RST keyframe.
5. When `_parse_once` reads the packets from the new position, it scales their timestamps to the **TS** domain, and playback resumes correctly.

### Initial Metadata Parsing (`_parse_format`)

A critical part of the corrected architecture is how initial stream metadata is handled. Values like `duration` and `start_time` are now kept in their original **RST** domain and are **not** scaled to TS when the file is first opened. This prevents them from becoming stale when the playback speed changes. They are only converted to TS on-the-fly when needed for calculations.

## Time Domain Variable Reference

| Variable                  | Domain | Use and Explanation                                                                                                                            |
| ------------------------- | ------ | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| `s->video_time`           | `ts` | The current video playback time in the time-scaled domain. |
| `s->audio_time`           | `ts` | The current audio playback time in the time-scaled domain. |
| `frame->time`             | `ts` | The timestamp of a video frame in the time-scaled domain. |
| `cdata->time`             | `ts` | Timestamp of a data chunk from the parser in the time-scaled domain. |
| `sc->time` | `ts` | Timestamp of the target seek chunk in the parser, used to drop audio packets before the new video frame. |
| `frame->blit_time`        | `ts` | The frame's `ts` timestamp, passed to the video sink for pacing. **Not a `wc` value.** |
| `s->delay` | `ts` | The smoothed A/V difference, calculated and stored as a `ts` duration. |
| `s->av_delay`             | `rst` | A user-configured A/V offset, in real-world milliseconds. |
| `stream_get_current_time()` | `rst` | **Returns** the current playback position in `rst` for UI purposes (converts from `s->video_time`). |
| `stream_seek_time()` | `rst` | **Accepts** a seek position in `rst` from the UI. |
| `s->stop_time` | `rst` | The user-defined stop time in `rst`. Must be converted to `ts` for comparison with `s->video_time`. |
| `s->duration` | `rst` | The total duration of the media, stored in RST. |
| `s->audio_ref_time` | `ts` | A `ts` reference time for sample-based audio sync, set from `cdata->time`. |
| `p->venc_ref_time` | `wc` | The video sink's private wall-clock anchor, set on flush. Used to calculate `venc_time`. |
| `venc_time` | `wc` | The video sink's internal clock; a wall-clock duration since the last flush/seek. |

## Design Philosophy

The seek-based approach prioritizes:

1. **Reliability over Seamlessness:** Clean state transitions prevent timing artifacts and ensure consistent behavior
2. **Simplicity over Complexity:** Straightforward implementation reduces bugs and maintenance burden  
3. **Hardware Compatibility:** Works reliably with all hardware decoders by using standard seek/flush mechanisms
4. **User Expectation Management:** Brief interruption during speed change is acceptable for reliable playback

This architecture ensures robust audio speed changes while maintaining clear separation between time domains and predictable system behavior.

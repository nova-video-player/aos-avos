# Audio Speed Change Architecture

## Overview

This document details the architecture for audio speed changes in the AVOS player. The implementation uses a seek-based approach that ensures clean state transitions by flushing buffers and reinitializing the media pipeline when speed changes occur.

## Time Domains

There are two fundamental time domains in the current implementation:

-   **`wc` (wall clock domain):** This is the system's monotonic clock (`atime()`). It progresses linearly and is independent of playback speed. It's the ground truth for display timing and hardware synchronization.
-   **`rst` (real stream time domain):** This is the playback time as perceived by the user. It represents the actual position within the media file. For example, if a video is 10 minutes long, the `rst` will go from 0 to 10 minutes, regardless of the playback speed.

### Key Relationships:

-   `wc` progresses linearly at system clock rate
-   `rst` progresses at rate determined by audio speed
-   Both domains are kept separate and only converted when necessary

## Core Architecture: Seek-Based Speed Changes

The core of the speed change mechanism uses a simple but effective seek-based approach that ensures clean state by reinitializing the pipeline. This approach prioritizes reliability over seamless transitions.

The process is orchestrated by `stream_set_av_speed` in `Source/stream.c`.

### Time-Scaled (TS) Domain and Parser Interaction

A key architectural choice is to have the internal pipeline operate in a **Time-Scaled (TS) domain**. This avoids complex timestamp conversions throughout the player logic. Instead of passing the speed factor down through all layers, the parser itself creates a "compressed" or "expanded" timeline by scaling timestamps as they are read from the source file.

This is implemented in `_parse_once()` in `Source/stream_parser_ffmpeg.c` by scaling every packet's timestamp (`pts`, `dts`) and `duration` immediately after it is read by `av_read_frame`:

```c
// Simplified from _parse_once()
packet.pts = RST_TO_TS(packet.pts, int64_t);
packet.dts = RST_TO_TS(packet.dts, int64_t);
packet.duration = RST_TO_TS(packet.duration, int64_t);
```
The `RST_TO_TS` macro divides the timestamp by the playback speed.

**Key Implications:**

1.  **Internal Timestamps are Time-Scaled:** All timestamps processed by the decoders and used in the core sync logic (`frame->time`, `s->video_time`, etc.) are in the TS domain, not RST. At 2x speed, a frame at 60s (RST) will have a timestamp of 30s (TS).

2.  **RST is for UI/User-Facing Time:** The RST domain is used for displaying time to the user or accepting seek commands from the UI. A conversion from the internal TS time to RST (`TS_TO_RST`) is required before displaying the current time.

3.  **Conversion at the Boundary:** Any interaction with the parser/streamer that involves time must be converted at the boundary.
    *   **Seeking:** This is the most subtle part of the architecture. When a user seeks to a time `t0` (in RST), the goal is for the internal `s->video_time` (in TS) to become numerically equal to `t0`. To achieve this, the seek target passed to `ffmpeg` must be `t0 * speed`. `ffmpeg` seeks to this higher RST value, and when the parser reads the resulting packet, it scales the timestamp down by `speed`, resulting in a TS value that matches the original `t0`. This is done in `_stream_seek_real()`: `int seek_target_rst = TS_TO_RST(rst_ms, int);`
    *   **Metadata:** Metadata from the container like total `duration` or `chapter` times are converted from RST to TS when the file is first opened in `ffmpeg_parse_header`.

4.  **`_real_time` Calculation:** The `_real_time` function takes a `frame->time` (TS) and correctly maps it to a Wall Clock (WC) value for the video sink, using the `vid_ref_time` (TS) and `sink_ref_time` (WC) anchors.

This compressed timeline approach enables speed changes without requiring complex domain conversions throughout the codebase, but requires awareness when interfacing with FFmpeg directly.

### Seek-Based Speed Change Process

When `stream_set_av_speed` is called, the following sequence of operations occurs:

1.  **Read Current Position:** Store current playback position before speed change using `stream_get_current_time()` (returns RST domain)
2.  **Change Hardware Audio Speed:** Call `audio_interface_change_audio_speed()` to adjust hardware playback rate  
3.  **Notify Video Decoder:** If present, notify video decoder of speed change via `set_playback_speed()` callback
4.  **Seek to Current Position:** Execute `stream_seek_time()` to current position, which:
    - Flushes all internal buffers
    - Resets parser state
    - Reinitializes video/audio decoders
    - Starts fresh with new speed scaling

```c
int stream_set_av_speed( STREAM *s, float av_speed )
{
    // Read current time before speed change affects calculations
    int stream_current_time = stream_get_current_time( s, NULL ); // RST domain
    
    // Change audio hardware speed
    audio_interface_change_audio_speed( s->audio_ctx, av_speed );
    
    // Notify video decoder of speed change
    if( s->video_dec && s->video_dec->set_playback_speed ) {
        s->video_dec->set_playback_speed(s->video_dec, 100, (int)(av_speed * 100 + 0.5));
    }
    
    // Seek to current position to flush and reset state
    if( stream_current_time > 0 && s->parser->seekable && s->parser->seekable( s ) ) {
        stream_seek_time( s, stream_current_time, STREAM_SEEK_BACKWARD, 0 );
    }
    
    return 0;
}
```

**Advantages of Seek-Based Approach:**
- **Guaranteed Clean State:** All buffers are flushed, eliminating timing inconsistencies
- **Simple Implementation:** No complex timestamp rescaling or buffer management required  
- **Reliable:** Hardware decoder internal buffers are cleared, preventing stale frame artifacts
- **Deterministic:** Each speed change results in known, consistent pipeline state

**Trade-offs:**
- **Brief Interruption:** Visible pause during speed change due to buffer flush and seek operation
- **Seek Requirement:** Only works with seekable streams

## Hardware Decoder Integration

### Video Decoder Speed Notification

The video decoder is explicitly notified of speed changes through the `set_playback_speed` callback:

```c
if( s->video_dec && s->video_dec->set_playback_speed ) {
    s->video_dec->set_playback_speed(s->video_dec, 100, (int)(av_speed * 100 + 0.5));
}
```

This allows hardware decoders (MediaCodec) to:
- Adjust internal timing calculations
- Prepare for timeline changes
- Optimize buffer management for new playback rate

### Buffer Management

The seek-based approach naturally handles hardware decoder buffer artifacts by:

1. **Complete Buffer Flush:** Seeking flushes all decoder internal buffers
2. **Clean Restart:** Decoder restarts with fresh state and new speed context
3. **No Stale Frames:** Eliminates mixing of old/new speed frames that cause visual artifacts

## Time Domain Variable Reference

| Variable                  | Domain | Use and Explanation                                                                                                                            |
| ------------------------- | ------ | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| `s->video_time`           | `rst`  | The current playback time in real stream time domain. Primary reference for current position in the stream.                                   |
| `s->audio_time`           | `rst`  | The current audio playback time in real stream time domain. Used for A/V synchronization.                                                     |
| `frame->time`             | `rst`  | The timestamp of a video frame in real stream time. Scaled by FFmpeg parser according to current audio speed.                                 |
| `frame->blit_time`        | `wc`   | The wall clock time at which a video frame should be displayed. Calculated from frame timing + wall clock references.                         |
| `cdata->time`             | `rst`  | Timestamp of a subtitle chunk in real stream time.                                                                                             |
| `s->sink_ref_time`        | `wc`   | Wall clock reference time for display timing calculations. Set during initialization and seek operations.                                      |
| `s->vid_ref_time`         | `rst`  | RST reference timestamp used for delta calculations. Must be in same domain as `frame->time` for proper timing.                               |
| `s->audio_ref_time`       | `wc`   | A wall clock reference time for audio. Used to map audio `rst` to `wc` for hardware audio timing.                                            |
| `s->sync_delay`           | `wc`   | The measured delay in the video sink, in wall clock time. Hardware presentation latency measurement.                                           |
| `s->av_delay`             | `rst`  | The calculated delay between audio and video in real stream time domain. Used by the sync logic.                                              |
| `s->wc_base_time`         | `wc`   | The wall clock time at the start of playback.                                                                                                  |
| `s->wc_current_time`      | `wc`   | The current wall clock time.                                                                                                                   |
| `stream_get_time_default` | `rst`  | Returns `s->video_time`, which is in the real stream time domain.                                                                              |
| `_get_audio_time`         | `rst`  | Returns the time of the last played audio sample, in real stream time domain.                                                                  |
| `_get_video_time`         | `rst`  | Returns the time of the last displayed video frame, in real stream time domain.                                                                |

## Design Philosophy

The seek-based approach prioritizes:

1. **Reliability over Seamlessness:** Clean state transitions prevent timing artifacts and ensure consistent behavior
2. **Simplicity over Complexity:** Straightforward implementation reduces bugs and maintenance burden  
3. **Hardware Compatibility:** Works reliably with all hardware decoders by using standard seek/flush mechanisms
4. **User Expectation Management:** Brief interruption during speed change is acceptable for reliable playback

This architecture ensures robust audio speed changes while maintaining clear separation between time domains and predictable system behavior.
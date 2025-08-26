# Seamless Audio Speed Change Architecture

## Overview

This document details the architecture for seamless audio speed changes in the AVOS player. It builds upon the principles of timestamp-based scaling, where the internal time representation (`ts`) is adjusted based on the desired audio speed, while maintaining synchronization with the real-world wall clock (`wc`) and the user-perceived real stream time (`rst`).

## Time Domains

There are three fundamental time domains:

-   **`wc` (wall clock domain):** This is the system's monotonic clock (`atime()`). It progresses linearly and is independent of playback speed. It's the ground truth for display timing and hardware synchronization.
-   **`rst` (real stream time domain):** This is the playback time as perceived by the user. It represents the actual position within the media file. For example, if a video is 10 minutes long, the `rst` will go from 0 to 10 minutes, regardless of the playback speed.
-   **`ts` (timestamp domain):** This is the internal, scaled time used for A/V synchronization. Timestamps from the media file are scaled by the audio speed.

### Key Relationships:

-   `rst = ts * audio_speed`
-   `ts = rst / audio_speed`
-   `wc` progresses linearly.

## Core Architecture: Timestamp-Based Scaling

The core of the seamless speed change mechanism lies in rescaling all in-flight timestamps when the audio speed is changed. This avoids costly operations like flushing buffers or seeking the stream, which would cause a noticeable interruption.

The process is orchestrated by `stream_set_av_speed` in `Source/stream.c`.

### FFmpeg Parser Timeline Compression

**Important architectural detail:** The FFmpeg parser implements packet-level scaling that creates a "compressed timeline" within FFmpeg's domain. This is implemented in `Source/stream_parser_ffmpeg.c` in the `_parse_once()` function:

```c
if (audio_interface_is_audio_speed_enabled()) {
    float speed = audio_interface_get_audio_speed();
    if (speed > 0 && speed != 1.0f) {
        if (packet.pts != AV_NOPTS_VALUE)
            packet.pts /= speed;        // Timeline compression
        if (packet.dts != AV_NOPTS_VALUE)
            packet.dts /= speed;
        if (packet.duration > 0)
            packet.duration /= speed;
    }
}
```

**Key implications:**

1. **FFmpeg operates on compressed time:** When audio speed is 1.5x, FFmpeg's internal timeline runs 1.5x slower than RST. A 1-hour video appears as ~40 minutes in FFmpeg's domain.

2. **Compensation required for FFmpeg interface:** Operations that interact directly with FFmpeg's timeline (seeking, chapters, duration) require compensation by multiplying by audio_speed to account for the compression.

3. **Automatic conversion to RST:** The packet-level scaling automatically converts timestamps back to RST domain via the `GET_*_TS` macros, so most AVOS components work in proper RST time.

4. **Seek compensation:** In `stream_video.c`, seeking requires `seek_rst = (int)(rst_ms * speed)` to compensate for FFmpeg's compressed timeline when providing seek targets.

This compressed timeline approach enables seamless speed changes without requiring complex domain conversions throughout the codebase, but requires awareness when interfacing with FFmpeg directly.

### Reset Strategy on Speed Change

When `stream_set_av_speed` is called, the following sequence of operations occurs:

1.  **Set New Speed:** The new audio speed is stored globally.
2.  **Rescale Parser Packets:** `ffmpeg_rescale_buffered_packets` is called. This function iterates through the packet queues in the FFmpeg parser (`aq`, `vq`, `sq`) and rescales the PTS, DTS, and duration of each packet. The rescaling is done relative to the current playback time (`s->video_time`) to maintain correct timing.
3.  **Rescale Video Frames:** `video_rescale_frames` is called. This function iterates through all video frame queues (`disp_q`, `decode_q`, `locked_q`, `codec_q`) and rescales the `time` of each frame. This ensures that frames already in the pipeline are adjusted to the new speed.
4.  **Notify Video Decoder:** The video decoder is informed of the new speed.
5.  **Change Audio Hardware Speed:** The `audio_interface_change_audio_speed` function is called to adjust the hardware playback speed (e.g., via Android's `PlaybackParams`).
6.  **Reset Synchronization:**
    *   `s->sink_ref_time` is set to -1. This invalidates the wall clock reference time, forcing the synchronization mechanism to establish a new reference point on the next frame.
    *   `stream_sync_restart(s)` is called to reset the A/V sync logic.

This reset strategy ensures that all components of the pipeline are operating with the new, consistent timestamps, and the A/V sync can re-establish itself without a full pipeline flush.

## Time Domain Variable Reference

| Variable                  | Domain | Use and Explanation                                                                                                                            |
| ------------------------- | ------ | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| `s->video_time`           | `ts`   | The current playback time in the timestamp domain. It is the primary reference for the current position in the stream.                           |
| `s->audio_time`           | `ts`   | The current audio playback time in the timestamp domain. Used for A/V synchronization.                                                         |
| `frame->time`             | `ts`   | The timestamp of a video frame. This value is rescaled during an audio speed change.                                                           |
| `frame->blit_time`        | `wc`   | The wall clock time at which a video frame should be displayed. It's calculated based on `frame->time` and `s->sink_ref_time`.                   |
| `cdata->time`             | `ts`   | Timestamp of a subtitle chunk.                                                                                                                 |
| `s->sink_ref_time`        | `wc`   | A wall clock reference time. It's the `wc` time when a specific `ts` was presented. It's used to map `ts` to `wc`. Reset on speed change.        |
| `s->vid_ref_time`         | `ts`   | The timestamp (`ts`) of the video frame that was used to establish the current `sink_ref_time`.                                                  |
| `s->audio_ref_time`       | `wc`   | A wall clock reference time for audio. Similar to `s->sink_ref_time` but for audio, used to map audio `ts` to `wc`. Reset on speed change.      |
| `s->sync_delay`           | `wc`   | The measured delay in the video sink, in wall clock time. It represents how long it takes for a frame to be displayed after it's submitted.    |
| `s->av_delay`             | `ts`   | The calculated delay between audio and video in the timestamp domain. Used by the sync logic.                                                  |
| `s->wc_base_time`         | `wc`   | The wall clock time at the start of playback.                                                                                                  |
| `s->wc_current_time`      | `wc`   | The current wall clock time.                                                                                                                   |
| `stream_get_time_default` | `ts`   | Returns `s->video_time`, which is in the timestamp domain.                                                                                     |
| `_get_audio_time`         | `ts`   | Returns the time of the last played audio sample, in the timestamp domain.                                                                     |
| `_get_video_time`         | `ts`   | Returns the time of the last displayed video frame, in the timestamp domain.                                                                   |
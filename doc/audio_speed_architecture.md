# Audio Speed Architecture Documentation

## Overview

The AVOS media player implements variable audio speed playback that maintains audio/video synchronization while allowing users to play content at different speeds (0.5x, 1.5x, 2x, etc.). This document explains the core principles and implementation across the codebase.

## Core Principles

### Time Units and Conversions

The audio speed system operates with two fundamental time representations:

**Timestamp (ts)**: Internal time units used by parsers and the media pipeline
- When `audio_speed > 1`, parsers output smaller timestamps 
- Example: At 2x speed, a 1000ms real duration becomes 500ms timestamp
- Used for: stream timestamps, buffer timing, sync calculations

**Real Stream Time (rst)**: Actual playback time as experienced by the user
- Always represents wall-clock time regardless of speed
- Used for: display timing, user interface, audio output timing

### Conversion Formulas

The relationship between these time units is governed by audio speed (`as`):

```
rst = ts * as    (Real Stream Time = Timestamp × Audio Speed)
ts = rst / as    (Timestamp = Real Stream Time ÷ Audio Speed)
```

**Examples:**
- At 2x speed (`as = 2.0`):
  - 500ms timestamp → 1000ms real time
  - 1000ms real time → 500ms timestamp
- At 0.5x speed (`as = 0.5`):
  - 1000ms timestamp → 500ms real time
  - 1000ms real time → 2000ms timestamp

## Implementation Architecture

### 1. JNI Interface (`jni/libavosjni/libavos.c`)

**Entry Point**: `Java_com_archos_medialib_LibAvos_nativeEnableAudioSpeed`
- Called from Java layer to enable/disable audio speed feature
- Activates the audio speed system via `libavos_enable_audio_speed(enable)`

### 2. Audio Interface (`Source/audio_interface_audiotrack_java.c`)

**Key Function**: `audiotrack_set_output_params`
- Configures Android AudioTrack with PlaybackParams for hardware-accelerated speed
- Scales buffer latency: `at->latency = (at->latency + min_buffer_latency_ms) / as`
- Reduces buffer scale due to improved timing accuracy
- **Time Domain**: Works in real stream time (rst) for audio output

### 3. Stream Parser (`Source/stream_parser_ffmpeg.c`)

**Core Logic**: FFmpeg parser with audio speed awareness
- **Principle**: "audio_speed > 1 means parsers output audio/video quicker with smaller time units yielding smaller timestamps"
- **Time Conversion Macros**:
  ```c
  #define GET_AUDIO_TS(ts) (ts == AV_NOPTS_VALUE ? STREAM_NO_PTS_VALUE : (INT64)ts * 1000 * (INT64)s->audio->scale / s->audio->rate)
  #define GET_VIDEO_TS(ts) (ts == AV_NOPTS_VALUE ? -1 : (INT64)ts * 1000 * (INT64)ff_p->time_base_num / ff_p->time_base_den)
  ```
- **Audio/Video Time Functions**: `_get_audio_time()`, `_get_video_time()` scale by audio speed
- **Time Domain**: Outputs timestamps (ts) that are scaled by audio speed

### 4. Video Stream Management (`Source/stream_video.c`)

**Frame Timing**: 
- `msPerFrame`: Intrinsic frame duration (real time, not scaled)
  ```c
  s->video->msPerFrame = 1000 * (UINT64)s->video->scale / (UINT64)s->video->rate;
  ```
- `framesPerSec`: Intrinsic frame rate
- **Frame Duration Assignment**: Converts to timestamp units
  ```c
  frame->duration = (int)(s->video->msPerFrame / as); // ts
  ```

**Frame Dropping/Doubling**: Handles speed compensation
- Uses `(s->video->msPerFrame / as)` for timestamp adjustments
- **Time Domain**: Converts from real time to timestamps for sync calculations

### 5. Synchronization (`Source/stream_sync.c`)

**A/V Sync Calculation**: `_stream_av_diff`
```c
return video_time - audio_time + stream_sync_av_delay(s) + (int)((s->av_delay + stream_dbg_delay) / as);
```

**Delay Thresholds**: 
```c
delay_s = (int)(stream_max_delay * s->video->msPerFrame / as); // ts
```
- Converts frame duration thresholds from real time to timestamps
- **Time Domain**: Operates in timestamp units for sync decisions

### 6. Subtitle Handling (`Source/stream_subtitle.c`)

**External Subtitles**: 
```c
time = (int)(audio_interface_get_audio_speed() * s->video_time);
```
- Converts video timestamps to real time for subtitle display

**Subtitle Duration**: 
```c
int duration = (int)(GET_SUB_TS(packet->duration) / audio_interface_get_audio_speed());
```
- Scales subtitle timing from real time to timestamps

### 7. Video Sinks (`Source/stream_sink_video_android*.c`)

**Time Scaling**: Both Android2 and Android3 sinks implement `_get_time()` functions
- Scale timestamps by audio speed for display timing
- Handle timestamp rescaling during speed changes via `rescale_timestamps()`
- **Time Domain**: Converts timestamps to real time for display

## Time Domain Summary

| Component | Input Domain | Output Domain | Conversion |
|-----------|--------------|---------------|------------|
| Parser | Media timestamps | Scaled timestamps (ts) | Built into parsing |
| Audio Interface | Real time (rst) | Hardware timing | Direct rst usage |
| Video Stream | Intrinsic timing | Timestamps (ts) | `/ as` |
| Sync Logic | Timestamps (ts) | Sync decisions | All in ts domain |
| Subtitles | Mixed | Display timing | `* as` for display |
| Video Sinks | Timestamps (ts) | Display timing | `* as` for display |

## Key Design Decisions

1. **Parser Scaling**: Parsers output pre-scaled timestamps to maintain efficiency
2. **Audio Hardware**: AudioTrack handles speed natively, no software scaling needed
3. **Intrinsic Frame Duration**: `msPerFrame` represents real frame duration for consistency
4. **Explicit Conversions**: All time domain conversions are explicit (`/ as` or `* as`)
5. **Timestamp-Based Sync**: Internal synchronization operates in timestamp domain

## Common Patterns

**Real Time → Timestamp**: `timestamp = real_time / audio_speed`
**Timestamp → Real Time**: `real_time = timestamp * audio_speed`

**Buffer/Latency Scaling**: Always scale by `/ as` to maintain timing accuracy
**Display Timing**: Always scale by `* as` to show correct user time
**Sync Calculations**: Keep in timestamp domain for consistency

## Seamless Audio Speed Changes

### Architecture Overview

The system supports seamless audio speed changes through `stream_set_av_speed()` in `Source/stream.c`. This allows real-time speed adjustments without seeks/flushes by coordinating timestamp rescaling across multiple components.

### Component Roles in Speed Changes

**Stream Controller** (`Source/stream.c`):
- **Role**: Orchestrates seamless speed changes across all components
- **Time Domain**: Manages core stream timestamps (ts)
- **Scaling**: Rescales `video_time`, `audio_time`, `sync_v_time`, `sync_a_time` using `old_speed/new_speed`
- **Coordination**: Calls audio interface, video sink rescaling, and video decoder updates

**Audio Interface** (`Source/audio_interface_audiotrack_java.c`):
- **Role**: Handles hardware audio speed changes via Android AudioTrack
- **Time Domain**: Works in real time (rst) for audio output
- **Scaling**: Updates latency calculations with new speed for timing accuracy
- **Limitation**: May fail due to hardware/format constraints, triggering fallback behavior

**Video Sinks** (`Source/stream_sink_video_android2.c`, `stream_sink_video_android3.c`):
- **Role**: Rescale timestamps of frames already in the rendering pipeline
- **Time Domain**: Converts between timestamps (ts) and display timing (rst)
- **Scaling**: 
  - `venc_put_time`: Reference time rescaled with `new_speed/old_speed`
  - Frame queue: Calls `frame_q_rescale_timestamps()` for queued frames
- **Purpose**: Maintains sync for frames already queued when speed changes

**Frame Queue** (`Source/frame_q.c`):
- **Role**: Rescales timestamps of video frames waiting in the pipeline
- **Time Domain**: Frame `blit_time` values in timestamp units (ts)
- **Scaling**: Uses `new_speed/old_speed` to adjust frame display timing
- **Critical**: Must use same scaling direction as video sink reference time

**Video Decoder** (`Source/stream_video.c`):
- **Role**: Informed of speed changes for internal timing adjustments
- **Time Domain**: Works with intrinsic frame rates and durations
- **Scaling**: Only updated when audio speed change succeeds
- **Purpose**: Maintains decoder internal state consistency

**Stream Parser** (`Source/stream_parser_ffmpeg.c`):
- **Role**: Primary entry point for media parsing and timestamp generation
- **Time Domain**: Core conversion point between media timestamps and system timestamps (ts)
- **Scaling**: 
  - `_get_audio_time()`, `_get_video_time()`, `_get_subtitle_time()`: Convert real time to timestamps using `/ as`
  - `GET_AUDIO_TS()`, `GET_VIDEO_TS()`, `GET_SUB_TS()`: Extract real stream time from media packets
  - Comments lines 914-917: Document core conversion principles
- **Architecture**: Implements the fundamental `ts = rst / as` conversion for all media streams

**Audio Stream Processing** (`Source/stream_audio.c`):
- **Role**: Handles audio decoding and timing in non-sample sync mode
- **Time Domain**: Operates with timestamps (ts) for stream coordination
- **Scaling**: Line 339 - converts decoded byte duration to timestamp units using `/ as`
- **Purpose**: Maintains audio timing consistency when not using sample-based synchronization

**Subtitle Processing** (`Source/stream_subtitle.c`):
- **Role**: Manages subtitle display timing across different subtitle types
- **Time Domain**: Mixed - converts between timestamps (ts) and real time (rst) based on subtitle type
- **Scaling**:
  - External subtitles: Line 276 - `time = (int)(audio_interface_get_audio_speed() * s->video_time)` (ts → rst)
  - Internal subtitles: Use timestamp domain directly
  - Duration scaling: Line 1488 - `/as` for proper display duration
- **Purpose**: Ensures subtitle timing matches video playback at any speed

**Synchronization** (`Source/stream_sync.c`):
- **Role**: Continues operating with rescaled timestamps
- **Time Domain**: All sync calculations remain in timestamp domain (ts)
- **Scaling**: Existing `/as` conversions handle the new speed automatically
- **Benefit**: No additional changes needed due to consistent timestamp domain

### Time Domain Flow During Speed Changes

1. **Audio Hardware**: Attempts speed change in real time domain (rst)
2. **Stream Times**: Rescaled from old timestamp domain to new timestamp domain
3. **Video Pipeline**: Frame timestamps rescaled to match new time base
4. **Video Decoder**: Updated to maintain internal consistency (if audio succeeded)
5. **Sync Logic**: Operates normally with new timestamp base

### Scaling Consistency Requirements

All timestamp rescaling during speed changes must use consistent formulas:
- **Stream timestamps**: `old_speed / new_speed` (converts from old ts domain to new ts domain)
- **Video sink reference**: `new_speed / old_speed` (compensates for stream rescaling)
- **Frame queue timestamps**: `new_speed / old_speed` (matches video sink scaling)

This ensures that all components maintain temporal relationships when transitioning between audio speeds.

## Migration Considerations

When modifying audio speed code:
1. Identify the time domain of your variables (ts vs rst)
2. Use explicit conversions between domains
3. Maintain consistency within each component
4. Understand each component's role in seamless speed changes
5. Test with various speed values (0.5x, 1.5x, 2x)
6. Verify scaling formulas are consistent across related components
7. Test both seamless changes and fallback scenarios
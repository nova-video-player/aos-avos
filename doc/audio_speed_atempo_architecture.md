# Audio Speed Architecture with atempo Filter

## Overview

This document describes the audio speed control implementation using FFmpeg's `atempo` filter. The architecture maintains timeline mapping for timestamp synchronization while using software-based audio resampling instead of AudioTrack PlaybackParams API.

## Design Principles

### Core Philosophy

The implementation replaces AudioTrack PlaybackParams with FFmpeg atempo filter while preserving the original time domain architecture:

1. **Timeline mapping enabled:** Parser scales all timestamps from RST → TS domain
2. **Software audio resampling:** atempo physically changes audio duration to match playback speed
3. **AudioTrack plays at 1.0x:** No PlaybackParams, no buffer scaling
4. **Domain equivalence:** atempo output duration equals TS domain time

### Why atempo with Timeline Mapping (Option B)

**The fundamental relationship:** `Δts = Δwc` (time-scaled duration equals wall clock duration)

With timeline mapping at speed S:
- Parser scales timestamps: `ts = rst / S`
- atempo changes physical duration: `physical_duration = rst / S`
- AudioTrack @ 1.0x plays for `physical_duration` wall clock time
- `Δwc = physical_duration = rst / S = ts`
- **`Δts = Δwc`** → synchronization maintained ✓

## Time Domains

There are three fundamental time domains:

### `wc` (Wall Clock)
The system's monotonic clock (`clock_gettime()`). Ground truth for real-world time progression.

### `rst` (Real Stream Time)
Original media timeline (e.g., 0 to 10 minutes). Used for:
- UI display and seeking
- File metadata (duration, start_time)
- User-facing position information

### `ts` (Time-Scaled)
**Primary internal time domain** for player logic. Created by parser scaling:
- `ts = rst / audio_speed`
- At 2.0x speed: 60s RST → 30s TS
- All core variables (`s->video_time`, `frame->time`) use this domain

### Key Relationship

**Absolute time conversions** (maintain continuity via anchors):
- `RST_TO_TS_TIME(time)` - Convert timestamp using current anchor
- `TS_TO_RST_TIME(time)` - Convert back to RST

**Duration conversions** (pure scaling, no anchors):
- `RST_TO_TS_DELTA(duration)` - Scale duration by `1 / speed`
- `TS_TO_RST_DELTA(duration)` - Scale duration by `speed`

### Domain Equivalence: ts ≡ wc

**Mathematical proof:**
1. By definition: `Δwc = Δrst / audio_speed` (wall clock elapsed)
2. By definition: `Δts = Δrst / audio_speed` (parser scaling)
3. Therefore: **`Δts = Δwc`** (numerically equal)

This equivalence means:
- 100ms in TS domain = 100ms in wall clock
- Video sink can mix TS and WC in calculations
- `venc_time` (WC) can be compared to `blit_time` (TS)

## atempo Filter Architecture

### Method Comparison

| Aspect | AudioTrack PlaybackParams (Old) | atempo Filter (Current) |
|--------|--------------------------------|-------------------------|
| Audio duration | Unchanged samples | Changed by atempo |
| AudioTrack rate | S× | 1.0× |
| Timeline mapping | Enabled | **Enabled** |
| Buffer scaling | 2× for 2.0x support | 1× (normal) |
| Android API | 23+ (Marshmallow) | All versions |
| Quality | Hardware-dependent | Consistent (WSOLA) |

### Data Flow at 1.5x Speed

**Audio Path:**
```
File: 1000ms RST
  ↓
Parser: RST_TO_TS_TIME → 667ms TS
  ↓
Decoder: outputs 1000ms of PCM samples (timestamp: 667ms TS)
  ↓
atempo filter: 1000ms samples → 667ms samples (physical)
  ↓
AudioTrack @ 1.0x: plays for 667ms wall clock
  ↓
audio_time += 667ms TS (NO RST_TO_TS_DELTA scaling!)
```

**Video Path:**
```
File: 1000ms RST
  ↓
Parser: RST_TO_TS_TIME → 667ms TS
  ↓
Decoder: frame timestamp = 667ms TS
  ↓
Video sink: blit_time = 667ms TS
  ↓
Display: waits for 667ms wall clock
```

**Result:** Audio 667ms TS, video 667ms TS → **perfect sync** ✓

### Critical Implementation Detail: Audio Time Accounting

The key to synchronization is avoiding **double-scaling**:

**WRONG (without atempo awareness):**
```c
// Decoded 1000ms of samples
// atempo outputs 667ms of samples
int output_time_ms = 667;  // physical duration
audio_time += RST_TO_TS_DELTA(667);  // = 667 / 1.5 = 445ms ❌ WRONG!
```

**CORRECT (with atempo awareness):**
```c
// atempo output samples are already in TS domain (physical time)
int output_time_ms = 667;  // physical duration = TS domain
audio_time += output_time_ms;  // = 667ms ✓ CORRECT!
```

## Implementation Details

### 1. Timeline Mapping (`stream.c:570-590`)

When speed changes:

```c
int using_atempo = (s->audio_filter_atempo != NULL);
audio_interface_set_using_atempo(using_atempo);

if (using_atempo) {
    float clamped_speed = clamp(av_speed, 0.25f, 2.0f);
    audio_interface_set_audio_speed(clamped_speed);

    // ENABLE timeline mapping with atempo speed
    timeline_map_apply((double)stream_current_time_rst,
                      (double)current_time_ts,
                      clamped_speed);
}
```

**Key:** Timeline mapping is enabled, NOT disabled. This creates the TS domain that matches physical playback time.

### 2. Parser Timestamp Scaling (`stream_parser_ffmpeg.c`)

Parser applies timeline mapping to all timestamps:

```c
// Video timestamps
int video_time = GET_VIDEO_TS(packet->pts) - ff_p->start_time;
return RST_TO_TS_TIME(video_time, int);

// Audio timestamps
int audio_time = GET_AUDIO_TS(packet->pts) - ff_p->start_time;
return RST_TO_TS_TIME(audio_time, int);
```

### 3. Audio Time Accounting - Two Paths

#### Path A: Non-SAMPLES Sync Mode (`stream_audio.c:488-527`)

For timestamp-based sync:

```c
if (s->sync_mode != STREAM_SYNC_SAMPLES) {
    if (!s->audio->vbr && audio_frame.size > 0) {
        // Calculate time from FILTERED output (after atempo)
        int bytes_per_sample = audio_frame.bits / 8;
        int channels = audio_frame.channels;
        int sample_rate = audio_frame.samplesPerSec;

        int output_samples = audio_frame.size / (bytes_per_sample * channels);
        int output_time_ms = (output_samples * 1000) / sample_rate;

        // Check if atempo is active
        int using_atempo = (s->audio_filter_atempo != NULL);
        if (using_atempo) {
            // atempo output = physical samples @ 1.0x = TS domain
            _add_audio_time(s, output_time_ms);
        } else {
            // Normal: samples in RST domain need scaling
            _add_audio_time(s, RST_TO_TS_DELTA(output_time_ms, int));
        }
    }
}
```

#### Path B: SAMPLES Sync Mode (`stream_audio.c:541-563`)

For sample-count-based sync (used with EAC3/AC3 passthrough):

```c
if (s->sync_mode == STREAM_SYNC_SAMPLES && s->audio_ref_time != -1) {
    if (s->audio->samplesPerSec) {
        // Accumulate samples written to AudioTrack
        s->audio_samples += size_written / s->audio->bytesPerFrame;
        int delta = (1000 * s->audio_samples) / s->audio->samplesPerSec;

        // Check if atempo is active
        int using_atempo = (s->audio_filter_atempo != NULL);
        if (using_atempo) {
            // atempo output = physical samples @ 1.0x = TS domain, no scaling
            _set_audio_time(s, s->audio_ref_time + delta);
        } else {
            // Normal: samples in RST domain need RST→TS conversion
            _set_audio_time(s, s->audio_ref_time + RST_TO_TS_DELTA(delta, int));
        }
    }
}
```

**Critical fix:** This second path (SAMPLES mode) was the source of A/V desync. The `size_written` value is the FILTERED output size (after atempo), which represents physical playback time. Applying `RST_TO_TS_DELTA` caused **double-scaling**.

### 4. Filter Processing Order (`stream_audio.c:378-383`)

Filters applied in sequence:

```c
// 1. atempo - FIRST (changes audio duration)
if (s->audio_filter_atempo && audio_frame.size > 0) {
    s->audio_filter_atempo->filter(s->audio_filter_atempo, &audio_frame);
}

// 2. compress - dynamic range compression (preserves duration)
if (run_filter && s->audio_filter_compress) {
    s->audio_filter_compress->filter(s->audio_filter_compress, &audio_frame);
}

// 3. ac3 - AC3 encoding if needed (preserves duration)
// 4. jni - JNI passthrough (preserves duration)
```

**Rationale:** atempo MUST be first because it changes `audio_frame.size`. All subsequent time calculations depend on this filtered size.

### 5. AudioTrack Configuration (`audio_interface_audiotrack_java.c`)

#### Buffer Size (`lines 561-568`):

```c
int using_atempo = audio_interface_is_using_atempo();
if (is_audio_speed_enabled && !using_atempo && ...) {
    buffer_scale = 2;  // Only for PlaybackParams method
} else {
    buffer_scale = 1;  // atempo always uses normal buffers
}
```

#### PlaybackParams (`lines 740-742`):

```c
// Skip PlaybackParams when using atempo (speed handled by PCM resampling)
if (!failed && is_audio_speed_enabled && !using_atempo && ...) {
    // Set AudioTrack playback rate only for legacy method
}
```

### 6. A/V Sync Delay Compensation (`stream_sync.c:138-141`)

atempo introduces processing delay that must be added to A/V sync calculation:

```c
if (s->audio_filter_atempo && s->audio_filter_atempo->delay) {
    filter_delay += s->audio_filter_atempo->delay(s->audio_filter_atempo);
}
```

Delay calculation (in `stream_filter_audio_atempo.c`):

```c
// Fragment size (power of 2, closest to sample_rate/24)
int fragment_size = 1 << log2_ceil(sample_rate / 24);

// atempo uses ~2.5 fragments for WSOLA overlap
int atempo_delay_samples = fragment_size * 2.5;
int atempo_delay_ms = (atempo_delay_samples * 1000) / sample_rate;

// Scale by speed for real-world time
return (int)(atempo_delay_ms / current_speed);
```

Example at 48 kHz, 1.5x speed:
- fragment_size = 2048 samples
- atempo_delay = 5120 samples = 107ms
- real_delay = 107 / 1.5 = ~71ms

### 7. A/V Diff Uses "Heard Audio" (TS)

When atempo is active, `audio_time` is already in TS (wall-clock) and
`video_time` is in RST. The sync diff must compare **video_ts** against the
audio timeline **as heard by the user**, not the audio write-head.

Implementation uses:

```c
video_ts = rst_to_ts_time(video_time);
diff = video_ts - audio_time + used_delay;
```

Where `used_delay` is the larger of:
- `stream_sync_av_delay()` (raw sink + filter delay, TS), and
- `smoothed_av_delay` (stabilized estimate of heard audio delay).

This avoids a persistent negative bias (~200ms on low-latency devices) that
appears when raw delay is too small after a speed change.

## atempo Filter Implementation

### FFmpeg Filter Graph

```
abuffer → atempo → abuffersink
```

**abuffer (input):**
- Receives decoded PCM frames
- Config: sample_rate, channel_layout, sample_format

**atempo:**
- Config: `tempo=<speed>` (chained for speeds < 0.5x)
- Range per filter: 0.5 to 2.0
- Overall support: 0.25x to 2.0x
- Algorithm: WSOLA (Waveform Similarity Overlap-Add)

**abuffersink (output):**
- Extracts filtered frames
- Output goes to AVAudioFifo buffer

### Speed Range

**Supported:** 0.25x to 2.0x

**Implementation:**
- Single atempo filter: 0.5x - 2.0x (native FFmpeg constraint)
- Automatic chaining: for speeds < 0.5x (e.g., 0.25x = two 0.5x filters)
- Covers typical use cases with high quality

### Bypass at 1.0x

```c
// In stream_filter_audio_atempo.c:_filter()
float speed = audio_interface_get_audio_speed();

if (fabsf(speed - 1.0f) < 0.001f || !ctx->filter_initialized) {
    return 0;  // Bypass - no processing
}
```

At 1.0x speed:
- Filter object exists but does nothing
- No CPU overhead
- Samples pass through unchanged

## Video Synchronization

### Video Sink Pacing (`stream_sink_video_android.c`)

Audio is the master clock. Video sync uses feedback control:

```c
// venc_time tracks wall clock elapsed since last flush
venc_time = venc_put_time + (atime() - venc_ref_time);

// blit_time is the frame's TS timestamp
// This comparison is valid because Δts = Δwc
blit_duration = frame->blit_time - venc_time;

if (blit_duration > 0) {
    // Frame is early, wait
    stream_sync_sleep_ms(blit_duration);
} else if (blit_duration < -MAX_DROP_THRESHOLD) {
    // Frame is late, drop
    return DROP_FRAME;
}
```

**Why this works:**
- `venc_time` is in WC domain
- `blit_time` is in TS domain
- But `Δts = Δwc`, so numerical comparison is valid
- Result: video pacing matches audio timeline

## Seeking and Speed Changes

### Seeking (`_stream_seek_real`)

1. UI provides seek target in **RST**
2. Parser seeks to RST position in file
3. Parser scales new timestamps to **TS** domain
4. Playback resumes with correct TS values

Timeline mapping anchors are re-established after seek completes.

### Speed Changes (`stream_set_av_speed`)

1. Capture current position (TS and RST)
2. Apply new timeline mapping with new anchor:
   ```c
   timeline_map_apply(stream_current_time_rst,
                     current_time_ts,
                     new_speed);
   ```
3. Update atempo filter speed (rebuilds filter graph)
4. No pipeline flush required
5. Continuity maintained via anchors

## Time Domain Variable Reference

| Variable | Domain | Description |
|----------|--------|-------------|
| `s->video_time` | TS | Current video playback time |
| `s->audio_time` | TS | Current audio playback time |
| `frame->time` | TS | Video frame timestamp |
| `cdata->time` | TS | Parser chunk timestamp |
| `frame->blit_time` | TS | Video frame presentation time (≡ WC) |
| `venc_time` | WC | Video sink's wall-clock timer (≡ TS) |
| `s->delay` | TS | Smoothed A/V difference |
| `s->av_delay` | RST | User-configured A/V offset |
| `s->duration` | RST | Total media duration |
| `s->video->msPerFrame` | RST | Unscaled frame duration |
| `stream_get_current_time()` | RST | Returns UI position (converts from TS) |
| `stream_seek_time()` | RST | Accepts UI seek position |

## Performance Characteristics

### CPU Usage
- atempo filter: ~20-22% CPU
- Comparable to AudioTrack Sonic algorithm
- No significant overhead vs PlaybackParams

### Latency
- Additional latency: ~107ms at 48kHz (scaled by speed)
- Properly compensated in A/V sync
- Not perceptible in practice

### Memory
- FFmpeg filter graph: minimal
- AVAudioFifo buffer: configurable
- **No 2× AudioTrack buffer** (saves memory vs old method)

## Advantages of atempo Implementation

1. **Universal compatibility:** Works on all Android API levels
2. **Consistent quality:** Software-based, not hardware-dependent
3. **Memory efficient:** No buffer scaling required
4. **Maintains architecture:** All existing timestamp logic remains valid
5. **Clear domain separation:** RST → TS in parser, TS throughout pipeline
6. **Correct math:** `Δts = Δwc` relationship preserved
7. **Feature compatible:** Works with all sync modes, subtitle sync, etc.

## Files Modified

### New Files
- `Source/stream_filter_audio_atempo.c` (~590 lines) - Complete atempo implementation

### Modified Files
- `Include/stream.h` - Added `audio_filter_atempo` member
- `Include/audio_interface.h` - Added atempo detection functions
- `Source/stream.c` - Timeline mapping with atempo
- `Source/stream_audio.c` - Audio time accounting (both sync modes)
- `Source/stream_video.c` - Initialize atempo filter
- `Source/stream_sync.c` - Add atempo delay
- `Source/audio_interface.c` - Atempo flag setter/getter
- `Source/audio_interface_audiotrack_java.c` - Skip buffer scaling and PlaybackParams
- `codecs.mk` - Added to build system

## Debugging

### Enable Debug Output

In `Source/debug.c`:
```c
int Debug[DBG_MAX_ENTRIES] = {
    [DBG_STREAM] = 2,  // Timeline mapping
    [DBG_AUD] = 2,     // Audio time accounting
    [DBG_SINK] = 2,    // Video sink pacing
};
```

### Expected Log Output at 1.5x

```
stream_open_audio_filter: opened [atempo]
stream:stream_set_av_speed av_speed=1.500000, audio_interface_get_audio_speed=1.000000
stream:stream_set_av_speed current_time_ts=917583, current_time_rst=917583
stream:stream_set_av_speed using atempo filter WITH timeline mapping, anchor_rst=917583 anchor_ts=917583, speed=1.500
stream_audio: applying atempo filter
stream_audio SAMPLES: atempo active, audio_time = 917583 + 21 (no scaling)
```

## Troubleshooting

### A/V Desync
1. Check both audio time accounting paths (non-SAMPLES and SAMPLES modes)
2. Verify `using_atempo` check exists in both paths
3. Confirm no `RST_TO_TS_DELTA` applied when `using_atempo` is true
4. Check atempo delay calculation and compensation

### Audio Quality Issues
1. Verify filter graph rebuilds correctly on speed changes
2. Check sample format (S16, S32, or FLT)
3. Ensure FIFO buffer is properly sized

### No Speed Change
1. Verify `audio_interface_is_audio_speed_enabled()` returns true
2. Check `stream_set_av_speed()` is called
3. Confirm atempo filter is initialized

---

**Document Version:** 2.0
**Last Updated:** 2025-01-06
**Implementation:** Option B (Timeline Mapping Enabled)
**FFmpeg Version:** N7.1
**Android API:** All versions supported

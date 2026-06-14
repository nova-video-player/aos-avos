# Audio Speed Architecture with atempo Filter

## Overview

This document describes the audio speed control implementation using FFmpeg's `atempo` filter. The architecture maintains timeline mapping for timestamp synchronization while using software-based audio resampling instead of AudioTrack PlaybackParams API.

When the audio-speed feature is enabled and the selected speed backend is not
AudioTrack PlaybackParams, the atempo filter is part of the steady PCM audio
pipeline even at exactly 1.0x. Keeping the neutral filter hot avoids a pipeline
discontinuity when the user changes speed while playback is running.

## Current State (2026-06-14)

The current implementation uses the atempo path as a software speed backend with
three separate clocks/anchors:

- **TS clock:** atempo output samples are already time-scaled, so the live
  `audio_time` / heard clock advances 1:1 with emitted output duration.
- **Output ledger:** every atempo PCM block written to AudioTrack is recorded in
  an output-frame ledger keyed by cumulative AudioTrack written frames. The
  ledger lets the sync path map the current AudioTrack playhead back to the TS
  sample currently audible.
- **Media/RST anchor:** speed commits need an RST anchor for
  `timeline_map_apply()`. The old `TS_TO_RST_TIME(anchor_ts)` projection used the
  pre-step committed speed and accumulated error across ramps. The current fix
  stores a media/RST span per ledger block and uses that ledger RST for the
  first `timeline_map_apply()` argument when the playhead resolves strictly
  inside a ledger block.

Speed changes are no longer committed to the video side immediately. The atempo
tempo command is applied to the audio filter immediately, but
`timeline_map_apply()`, `set_playback_speed()`, and the video-sink re-anchor are
deferred until the AudioTrack playhead crosses the output-frame boundary where
the new-speed content is audible. Pending speed commits are queued and promoted
in order.

### Option B Current, Option A Fallback

**Option A (fallback):** media/RST spans are sampled at AudioTrack-write time
from the patched atempo state (`ns_in - ring`). This fixed the accumulating ramp
desync in stress logs, including seek/pause reset cases, by preventing the
stale-speed RST projection from being used at commit time.

Option A's caveat is that write-time sampling includes variable wrapper-FIFO
lead between filter output production and AudioTrack write. This is why it is no
longer the primary source when the production map can resolve a block.

**Option B (current, validated):** media/RST assignment happens in
`stream_filter_audio_atempo.c` at output-production time. The atempo wrapper
keeps an output-position to media/RST map keyed by cumulative atempo output
frames. Each drained output burst records the media span produced from the
patched atempo state (`ns_in - ring`). The AVOS ledger queries this map by the
output frame range just read from the wrapper FIFO. If the map misses, the code
falls back to Option A; if Option A cannot read state, it falls back to the 1:1
TS slope.

Both Option A and Option B depend on the local FFmpeg patch
`native/ffmpeg-android-builder/atempo.patch`, which exposes atempo's internal
WSOLA state (`ns_in`, `ns_out`, and ring occupancy). A stock-FFmpeg strategy
would require a less precise tempo-schedule estimate or proper filter PTS
ownership and is a separate design goal.

## Design Principles

### Core Philosophy

The implementation replaces AudioTrack PlaybackParams with FFmpeg atempo filter while preserving the original time domain architecture:

1. **Timeline mapping enabled:** Parser scales all timestamps from RST → TS domain
2. **Software audio resampling:** atempo physically changes audio duration to match playback speed
3. **AudioTrack plays at 1.0x:** No PlaybackParams, no buffer scaling
4. **Domain equivalence:** atempo output duration equals TS domain time

### Hot atempo at 1.0x

There are two separate questions:

- **Filter topology:** if software speed is active, keep atempo in the PCM
  chain at 1.0x so speed changes up or down can be applied without rebuilding
  the audio path or creating an audible discontinuity.
- **Delay accounting:** exact 1.0x may still be treated as neutral for
  synthetic atempo-delay compensation. A hot neutral filter is not the same as
  an active speed transform, and the sync model must not invent extra heard
  latency when the physical output clock is already represented by committed
  samples.

AudioTrack PlaybackParams is the exception. If the device path is explicitly
using AudioTrack-based speed, atempo is not the active speed backend. Passthrough
and AC3 recoding are also excluded from atempo sample filtering because they do
not send ordinary PCM samples through the software tempo chain.

### Why atempo with Timeline Mapping

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

### 1. Timeline Mapping and Deferred Video Commit (`stream.c`)

When speed changes:

```c
int using_atempo = (s->audio_filter_atempo != NULL);
audio_interface_set_using_atempo(using_atempo);

if (using_atempo) {
    float clamped_speed = clamp(av_speed, 0.25f, 2.0f);
    audio_interface_set_audio_speed(clamped_speed);

    // Queue the video-side speed commit. The filter sees the target now,
    // but timeline/video speed are promoted only when AudioTrack playhead
    // reaches the output-frame boundary where new-speed content is audible.
    atempo_commit_q.push({
        speed = clamped_speed,
        boundary = atempo_ledger_output_frames + wrapper_fifo_samples
    });
}
```

When the playhead crosses a queued boundary, `stream_atempo_commit_poll()`:

1. computes `anchor_ts` from the current heard-audio clock;
2. resolves the playhead in the atempo ledger;
3. uses the ledger media/RST value as the first `timeline_map_apply()` argument
   when the lookup is strictly inside a ledger block (`state == 0`);
4. falls back to `TS_TO_RST_TIME(anchor_ts)` if the ledger is unavailable,
   stale, or extrapolated;
5. updates `set_playback_speed()` and re-anchors the video sink to `anchor_ts`.

**Key:** Timeline mapping is enabled, but the video-side commit is
playhead-gated. This avoids committing video at the moment old-speed PCM is
written to AudioTrack, which is earlier than when the user hears that content.

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

### 6. Atempo Output Ledger and Heard Clock (`stream_audio.c`, `stream_sync.c`)

The atempo path does not rely on `last_good_delay_ms` as the primary authority
during software speed changes. Instead:

- `stream_audio.c` reserves a ledger entry before each atempo PCM write and
  finalizes/cancels it after `AudioTrack.write()` returns.
- Each entry records:
  - output-frame start,
  - TS block start,
  - block frame count/rate,
  - media/RST start and span.
- The media/RST span normally comes from the Option B production map in
  `stream_filter_audio_atempo.c`, keyed by the wrapper output-sample index. The
  stream ledger queries the range `[output_cursor - nframes, output_cursor)` for
  the block just read from the wrapper FIFO.
- If the production map does not cover the full range, the ledger falls back to
  the Option A live `ns_in - ring` delta sampled at reserve time; a final 1:1
  TS-slope fallback keeps the clock progressing if the patched state is
  unavailable.
- `stream_sync.c` looks up the current AudioTrack presented frame in this ledger
  to compute the heard TS. Timestamp-based presented frames are treated as
  DAC-position evidence; playback-head evidence uses a calibrated post-playhead
  latency.
- The same ledger exposes media/RST interpolation for deferred speed commits.

The TS side of the ledger advances 1:1 with emitted atempo output. Do not
multiply the live heard clock by tempo; `ns_in/ns_out ~= tempo` is expected
because `ns_in` is RST/media and the heard clock is TS.

### 7. A/V Sync Delay Compensation (`stream_sync.c`)

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

### Hot Filter at 1.0x

```c
// In stream_filter_audio_atempo.c:_filter()
float speed = audio_interface_get_audio_speed();
int speed_enabled = audio_interface_is_audio_speed_enabled();

if (!speed_enabled || !ctx->filter_initialized) {
    return 0;  // Bypass when audio-speed feature is disabled
}
```

At 1.0x speed:
- If audio-speed is enabled and software atempo is the selected backend, the
  filter remains active at neutral tempo. This keeps the PCM filter topology
  stable for seamless runtime speed changes.
- If audio-speed is disabled, or AudioTrack PlaybackParams is the selected
  backend, the filter is bypassed.
- Passthrough and AC3-recoding routes do not use atempo filtering.

Important: keeping atempo hot at 1.0x is a topology rule, not a requirement to
count the full synthetic WSOLA delay in every 1.0x heard-time calculation. Delay
selection may ignore atempo's neutral-speed internal delay while still counting
atempo delay when `speed != 1.0x`.

## Video Synchronization

### Video Sink Pacing (`codec_sfdec2.c`)

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

## Seeking, Reset, and Speed Changes

### Seeking (`_stream_seek_real`)

1. UI provides seek target in **RST**
2. Parser seeks to RST position in file
3. Parser scales new timestamps to **TS** domain
4. Playback resumes with correct TS values

Timeline mapping anchors are re-established after seek completes.

### Speed Changes (`stream_set_av_speed` + `stream_atempo_commit_poll`)

1. Apply the new atempo tempo command immediately.
2. Queue a video-side commit at the current atempo output boundary.
3. Keep the video timeline and decoder playback speed at the previous committed
   speed while old-speed content is still queued in AudioTrack.
4. Promote queued commits in order when the playhead crosses each boundary.
5. Anchor `timeline_map_apply()` with:
   - `anchor_ts`: the current heard TS;
   - `anchor_rst`: the ledger media/RST at the same audible playhead when
     available (`state == 0`), otherwise a projection fallback.
6. On seek/flush/reset, clear the ledger and collapse pending commits to the
   latest target speed using a deferred sentinel. The collapsed commit waits for
   the new ledger to become active before applying, avoiding stale-map drains.

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
| `stream_seek_time_frame_accurate()` | RST+TS | Seek to RST keyframe, then drop to TS target |
| `atempo_ledger_output_frames` | output frames | Cumulative AudioTrack-written frame cursor for atempo ledger |
| `STREAM_ATEMPO_LEDGER_ENTRY.block_ts_start` | TS | TS timestamp of the output block |
| `STREAM_ATEMPO_LEDGER_ENTRY.block_rst_start` | RST | Media/RST timestamp of the output block, used for speed commit anchors |

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
- `Include/audio_interface.h` - Added atempo detection and presented/written frame helpers
- `Source/stream.c` - Playhead-gated atempo video-speed commit queue
- `Source/stream_audio.c` - Audio time accounting and atempo output ledger
- `Source/stream_filter_audio_atempo.c` - Atempo filter, output FIFO, and
  production-time output-to-media map
- `Source/stream_video.c` - Initialize atempo filter
- `Source/stream_sync.c` - Atempo ledger heard clock and ledger RST lookup
- `Source/audio_interface.c` - Atempo flag setter/getter
- `Source/audio_interface_audiotrack_java.c` - Skip buffer scaling/PlaybackParams, expose presented/written frames
- `codecs.mk` - Added to build system
- `native/ffmpeg-android-builder/atempo.patch` - Required local FFmpeg patch exposing atempo internal media/ring state

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
at_ledger_arm: written=... playhead=... queued=... audio=... epoch_rst=...
atempo_commit_arm: prev=1.000 target=1.500 boundary=... out_cursor=... flt_fifo=...
at_ledger_omap: w_start=... nframes=... b_span_us=... a_span_us=... diff_us=...
at_ledger: ledger_heard=... heard=... applied=1 playhead=... state=0 speed=1.500
atempo_commit_apply: prev=1.000 speed=1.500 boundary=... crossed=1 anchor_ts=...
atempo_rst_anchor: speed=1.500 anchor_rst_proj=... anchor_rst_ledger=... state=0 flipped=1
```

## Troubleshooting

### A/V Desync
1. Check both audio time accounting paths (non-SAMPLES and SAMPLES modes)
2. Verify `using_atempo` check exists in both paths
3. Confirm no `RST_TO_TS_DELTA` applied when `using_atempo` is true
4. Check `atempo_commit_arm` / `atempo_commit_apply` ordering and that commits
   usually log `atempo_rst_anchor ... state=0 flipped=1`
5. Check `at_ledger_omap` during dense logging. Large differences from the live
   Option A estimate are expected only around wrapper-FIFO lead; map misses
   should remain rare and fall back cleanly.
6. After seek/flush during a ramp, verify deferred sentinel commits do not drain
   against an empty ledger
7. Confirm the FFmpeg atempo patch is present when building this path

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
**Last Updated:** 2026-06-14
**Implementation:** Timeline mapping + playhead-gated atempo commit + Option B production media map with Option A fallback
**FFmpeg Version:** N7.1
**Android API:** All versions supported

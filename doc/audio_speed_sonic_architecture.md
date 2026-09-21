# Audio Speed Architecture with the Sonic Filter

## Current status

Sonic (`Source/sonic.c`, `Include/sonic.h`) is a selectable software audio-speed
backend wrapped by `Source/stream_filter_audio_sonic.c`. Native backend values
are 0 = FFmpeg atempo, 1 = AudioTrack PlaybackParams, and 2 = Sonic. Java's
`audio_speed_mode` preference additionally includes Disabled and maps Sonic's
preference value 3 to native backend 2.

Sonic shares the `STREAM_FILTER_AUDIO` operations, output ledger, and
playhead-gated video-speed commits with atempo. **Sonic audible-transition
accounting is implemented.** Each returned output burst carries its consumed
media span and effective processing-speed checkpoint. The writer translates
these into accepted AudioTrack frames; video follows the presented checkpoint.
Host tests and Android builds pass; physical lip sync still needs device testing.

Sonic does not require sample realignment. It still requires timing accounting
for retained input, produced output, pending writes, and audio queued in the
sink. An output sample count alone does not identify the audible media position.

The MediaCodec audio-decoder speed restriction also applies to Sonic; see
`mediacodec_audio_decoder.md`. Passthrough and AC3 recoding bypass software speed
filtering. MediaCodec video presentation is a separate path.

## Time domains and accounting

The player uses three time domains:

- **RST:** original media time, used for UI position and seeking.
- **TS:** the internal time-scaled timeline, used by `audio_time`, `video_time`,
  and decoded frame timestamps.
- **Wall time:** monotonic time, used for physical presentation deadlines.

Within a committed speed segment, `delta_ts = delta_rst / speed = delta_wall`.
Absolute conversions use the anchors installed by `timeline_map_apply()`:
`ts = ts_anchor + (rst - rst_anchor) / speed`. Duration conversions use
`RST_TO_TS_DELTA` and `TS_TO_RST_DELTA` without absolute anchors.

The parser already converts source timestamps to TS. Sonic physically changes
PCM duration while AudioTrack plays the transformed samples at its normal rate.
For example, at 1.5x, 1000 ms of source audio produces approximately 667 ms of
output. That output represents 667 ms of TS and 667 ms of wall time.

Consequently, the byte-duration path in `stream_audio.c` must use:

```c
if (use_atempo) { // Either software speed filter, including Sonic.
    _add_audio_time(s, add_ms);
} else {
    _add_audio_time(s, RST_TO_TS_DELTA(add_ms, int));
}
```

Here `add_ms` is calculated from committed output bytes and the output byte
rate, with a fractional-duration remainder. `_add_audio_time()` adds directly
to the TS clock; it does not perform another conversion. Multiplying filtered
output duration by speed adds RST units to a TS clock, while dividing it by
speed scales an already transformed duration twice.

The `STREAM_SYNC_SAMPLES` branch follows the same rule: for filtered PCM,
output sample duration is added directly to the TS reference time. Neither
software backend should receive a second speed conversion in that branch.

Video projection likewise applies no second speed division:

```text
render_ts_ns = snap(frame_ts) + render_offset_ns + effective_av_delay_ns
```

`_snap_timestamp_ns()` uses the effective frame rate to snap TS timestamps.
`render_offset_ns` maps the audio-owned TS anchor to monotonic time. See
`android_frame_timing.md`, `sync_anchoring_rules.md`, and
`audio_speed_atempo_architecture.md` for the shared clock contracts.

## Filter operations

`_filter()` reads the requested global speed on each nonempty input call and
clamps it to 0.5x–2.0x. Supported formats are signed 16-bit PCM and 32-bit float
PCM, following the player's `bits` convention. It writes input through
`sonicWriteShortToStream()` or `sonicWriteFloatToStream()` and reads all ready
output into a wrapper-owned buffer. Consume returned output before the next
filter or drain operation.

Missing frame channel/rate/bit-depth fields inherit the current geometry. Both
`_filter()` and `stream_filter_audio_sonic_needs_format_drain()` resolve these
fields consistently under the context mutex.

### Format changes and end of stream

When geometry changes, `stream_audio.c` retains the incoming frame and calls
`drain(end=1)` before admitting it. The drain flushes Sonic's retained input and
returns old-format output through the ordinary writer. Flush failures propagate
as errors rather than being mistaken for end of output.

After a successful terminal drain, `stream_initialized` becomes false. Repeated
drains return empty. The next nonempty filter call recreates Sonic before
accepting input, even if the geometry is unchanged. A retained frame with new
geometry therefore does not repeatedly drain the old stream.

A seek/reset uses `_flush()`, which destroys and recreates Sonic to discard
buffered state intentionally. Successful recreation sets `stream_initialized`
back to true and preserves the requested speed.

### Concurrency

The context mutex protects live Sonic operations and associated shared state,
including geometry, speed, enabled status, and initialization status. Delay
queries may run on renderer/control threads while the audio thread filters or
recreates the Sonic stream. They take the same mutex before reading that state.
The speed setter computes its result before releasing the mutex.

The mutex does not make the context itself reference counted. Open/publication
and final deletion still require the player's existing lifecycle exclusion.
No filter lock is held across an AudioTrack write or wait.

### Delay estimate

`_delay()` reports ready-but-unread output duration from
`sonicSamplesAvailable()`. Since the wrapper normally reads all available output,
this is usually zero. Existing generic sync code can consult this estimate, but
it is not a complete filter latency or media-position measurement. In particular,
it excludes retained input and does not account for output already in AudioTrack.

## Audible speed transitions

Sonic retains input for pitch-period processing and tracks `inputPlayTime` and
`timeError`. `processStreamInput()` derives an effective speed from the retained
input and its expected play time. Following a request, several output bursts can
therefore have speeds between the old and new requested values. Produced PCM
already queued in the writer or AudioTrack cannot be retimed by a Sonic setter.

### Production observations

The local Sonic additions `sonicInputSamplesBuffered()` and
`sonicProcessingSpeed()` expose retained input and the effective speed of the
last processing call. They do not change the live waveform algorithm. The AVOS
wrapper uses speed-only mode: pitch and rate remain 1, so there is no downstream
Sonic pitch/rate buffer to account for.

The wrapper counts admitted input and returned output in interleaved PCM frames:

```text
processed_media = admitted_input - retained_input
burst_media_span = processed_media - previous_processed_media
burst_output_range = [previous_returned_output, returned_output)
```

It saves the most recently returned burst's span. Its consumer must finish that
buffer and take its checkpoint before calling `filter`/`drain` again. Media
lookups interpolate within the burst using differences of integer endpoints,
so arbitrary split writes sum to the original media-frame span. This is a
burst-level estimate of media position, not an exact attribution of individual
overlap-added samples to source timestamps.

When effective speed changes, the wrapper publishes a checkpoint at the burst's
first output frame. It follows intermediate blended speeds rather than labeling
all output with the latest request. Once effective speed is within 0.001 of the
request, checkpoints settle on the requested value. Initial output and output
after recreation also publish a checkpoint, even at 1x.

### Shared writer and presentation gate

`STREAM_FILTER_AUDIO` exposes optional `get_output_state`, `take_speed_commit`,
and `lookup_output_media` callbacks. Both atempo and Sonic implement them; only
FFmpeg's legacy audit accessor remains specific to atempo. Existing
`atempo_ledger_*` and `atempo_commit_*` names are retained for the shared state.

1. `stream_set_av_speed()` updates the request but defers the software-filter
   video timeline and decoder speed.
2. The writer maps wrapper checkpoint indices to cumulative sink-frame indices.
   It reserves a ledger block and finalizes only the prefix AudioTrack accepted.
   A zero/retry write rolls the block back; it does not advance either clock.
3. The shared presentation gate waits until the playhead reaches the checkpoint
   and at least one frame beyond that boundary has been accepted. It anchors the
   new timeline to the ledger's media position at the same playhead observation.
4. Manual-delay silence adds an output hold with zero media span and shifts any
   pending checkpoints behind the inserted silence.

The commit queue has the same 64-entry capacity as the presentation ledger,
allowing several intermediate checkpoints per Sonic transition. This retains
the existing fallback policy: a terminal presentation drain may finish pending
commits, and an unavailable/stalled playhead has a three-second timeout. Device
presentation timestamps and downstream receiver latency still limit accuracy.

### Lifecycle

An ordinary speed change or pause retains Sonic input, output and timing state.
A seek flush clears wrapper counters, mapping, and pending checkpoints; the
shared reset invalidates old sink-frame boundaries. New output republishes its
speed in the fresh epoch. Sink-only recovery retains wrapper frame indices while
the writer establishes a new sink-frame mapping.

A format change first drains old-format output through the writer. Recreation
resets input/media counters but keeps the cumulative wrapper output index; the
new geometry publishes a fresh map and checkpoint. End-of-stream drain attributes
all remaining real input to the tail, excluding synthetic padding. The padding
receives a matching play-time budget so it does not spuriously accelerate the
tail; no padding or flush is used for live speed changes.

## Corrections and validation

The initial integration had two coupled domain errors: the renderer divided
already-scaled TS differences by speed, while the audio writer multiplied
already-transformed PCM duration by speed. Both operations have been removed.
At 1.5x, the old renderer projected a 40 ms source-frame interval as approximately
17.8 ms instead of 26.7 ms, and the old writer counted 32 ms of output as 48 ms TS.
Agreement between those incorrect internal clocks was not evidence of lipsync;
the earlier `avos-237.log` claim must not be used as validation of physical sync.

Host regressions cover wrapper lifecycle, unchanged live-transition PCM versus
a direct Sonic stream, blended checkpoints, exact split-write media accounting,
short terminal tails, queued playback, retries, pause, manual-delay holds, and
seek/reset. The presentation tests compile the production ledger and commit
functions with a fake sink/playhead.

```sh
python3 test/run-sonic-filter.py
python3 test/run-speed-transitions.py
CFLAGS='-fsanitize=undefined -g' python3 test/run-sonic-filter.py
CFLAGS='-fsanitize=undefined -g' python3 test/run-speed-transitions.py
```

Both suites pass normally and with UndefinedBehaviorSanitizer. Android builds
for armeabi-v7a and arm64-v8a also pass with `LIBAV_CONFIG=full`. The older
`test/pause_resume_sync.py` cannot currently run because it extracts a removed
`_stream_audio_apply_preload` helper; the new presentation suite exercises the
current pause/reset path. No physical lip-sync validation is claimed.

**Document version:** 1.2  
**Last updated:** 2026-09-20

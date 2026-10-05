# Android Frame Timing Mode

## Purpose

The current `sfdec2` path always uses the restored platform-timed release engine
(historically named `android_sync=1`). AVOS computes a monotonic presentation
deadline for every decoded frame and passes it to Android `MediaCodec`, while all
media timestamps remain in the time-scaled (`ts`) domain used for audio speed
changes. There is no current runtime `android_sync=0` branch in `sfdec2`.

## How the Sink Delegates Pacing (`Source/codec_sfdec2.c`)

- The sink always calls `sfdec_buf_render` with a non‑zero `render_ts_ns`.
- `render_ts_ns` is derived from a single render offset plus user delay:
  `render_ts_ns = f->time * 1e6 + render_offset_ns + effective_av_delay_ns`.
- The render offset is initialized from `_get_render_heard_ts()` when audio time
  exists. That helper prefers an audio-thread `put_time` sample no older than
  100ms and otherwise recomputes `stream_get_heard_audio_ts()`.
- If audio has not started, initialization uses the static anchor delay once and
  reanchors to heard audio when it becomes available.
- The renderer may already have adopted negative PCM heard time before the first
  nonnegative `put_time` initializes scheduler bookkeeping. If it has submitted
  a frame on the current audio epoch, a first-publication correction within 100ms
  preserves that render offset and converges at 1ms per distinct frame. This
  avoids a second startup deadline jump merely because the scheduler anchor was
  still unset. Preview-only output, pending seeks/resumes, speed changes, missing
  anchors and larger discontinuities retain the reset path; passthrough is excluded.
- For reanchor windows, the sink prefers a **fresh** audio-thread
  `put_time` anchor (`venc_put_time`) and falls back to recomputed
  `stream_get_heard_audio_ts()` only when `put_time` is stale. This avoids
  cross-thread heard-time skew at seek/resume boundaries.
- For PCM and mode 1, the offset is **slewed** toward a new target only on
  explicit events (seek/resume/speed/hard discontinuity). Decoded PCM has one
  additional epoch-scoped event: after a bounded pipeline-latency startup seed,
  a direct AudioTimestamp success streak of 10 may enable a single correction.
  That correction moves at 1ms per distinct video frame and stops within an 8ms
  deadband; playback-head fallback cannot trigger it below that timestamp trust
  threshold. Direct mode 2 normally keeps its render offset stable after
  initialization. A newly trusted dynamic
  clock first completes any monotonic heard-time hold while the renderer remains
  on its provisional anchor. Once the dynamic phase is ready, an established
  audio-owned renderer anchor within 100ms of the trusted clock uses fast bounded
  recovery, avoiding a deadline jump for a small remaining phase error. Missing
  anchors, a pending seek reanchor, and larger errors retain the explicit
  audio-based reanchor so startup does not acquire a second multi-second slew.
  The smooth handoff preserves the per-frame guard across lookahead retries.
  Later transition and resume corrections remain bounded per frame.
  Mode 2 applies at most 5ms per distinct frame during fast recovery and 0.2ms
  during steady tracking. Any remainder carries into a subsequent frame; the
  final step cannot snap through a second step's worth of correction.
- A PCM resume on a backend that preserves queued output keeps the pause-shifted
  renderer anchor until the first accepted write publishes its clock. With an established audio anchor,
  corrections up to 350ms use a slew with an 8ms initial deadband instead of an
  abrupt scheduling jump. Each distinct video frame adjusts the offset by at
  most 10% of its speed-adjusted duration, capped at 4ms (1ms fallback if the
  duration is unavailable). A 201ms correction at 24fps therefore takes about
  51 frames, rather than 201 frames at the cold-start correction rate. The
  correction is still applied, preventing cumulative pause/resume phase error.
  Missing anchors, seek/speed transitions,
  and larger discontinuities retain the hard reset. Ordinary pauses freeze an
  unfinished resume correction. The next resume applies at most two
  speed-adjusted frame intervals (capped at 100ms) of that measured residual;
  backward correction is also capped by the paused duration so the combined
  pause shift cannot move deadlines backward. The target and renderer offset
  then receive the same pause shift, and the remaining correction keeps its
  per-frame limit until the fresh post-resume clock updates the target. This
  prevents rapid taps from repeatedly cancelling convergence and accumulating
  enough error to trigger the 350ms hard reset. The retained correction is
  invalidated by an output-generation, seek-epoch, speed-mapping or manual-delay
  change. Cold-start corrections still reset on pause. Passthrough resume
  policy is unchanged.
- At 1x, retained-output PCM resume also follows subsequent changes in the
  calibrated mixer-playhead latency used by the atempo/Sonic ledger. Calibration
  and heard time are published as a pair. After the initial resume slew finishes,
  a latency change exceeding 8ms shifts its target by that change alone, with the
  same per-frame limit; ordinary timestamp/write jitter cannot retarget it.
  Seek, pause, output replacement, manual-delay changes, speed transitions,
  uncalibrated samples and explicit reanchors end this tracking. This does not
  change passthrough, PlaybackParams, or the calibration algorithm itself.
- Manual A/V delay (`s->av_delay`) is also slewed in the render path through
  `effective_av_delay` (bounded per-frame step) so large UI jumps do not create
  a burst of ASAP renders ("fast video" transient).

The video thread limits submission to a 200ms lookahead window and releases a
frame without rendering when it is already more than 200ms late. Frames inside
that window are submitted to MediaCodec with the computed deadline.

`video_render_diag` includes `render_seq`, a render-thread-owned counter advanced
after every successful timed submission, even with diagnostic logging disabled.
It persists across pause, seek and flush and resets with a new decoder instance.
This distinguishes omitted timing records from deadline errors between adjacent
submissions. A successful submission does not prove physical display timing.

## Frame Snapping and Wall-Clock Mapping (`Source/codec_sfdec2.c`)

`_snap_timestamp_ns()` rounds the frame TS to the nearest frame interval using
the current frame rate and playback speed. Snapping is relative to the first
epoch-valid video frame rather than absolute timestamp zero, so a legitimate
stream phase offset cannot alias millisecond-quantized timestamps into duplicate
or skipped presentation deadlines. The phase origin is reset for codec open,
flush, seek, and seek-epoch changes. The sink then adds `render_offset_ns` and
the effective user video delay. `sfdec_buf_render()` forwards the resulting
non-zero `render_ts_ns` to MediaCodec for timed release.

## Audio Speed Interaction

- The parser feeds MediaCodec timestamps that are already scaled by the active audio speed (`ts` domain). Because `Δts = Δwc`, the wall-clock projection remains valid at any speed.
- When the app changes audio speed (or resumes playback with a remembered
  non-1.0x speed), `stream_set_av_speed` caches the requested ratio and sends the
  effective ratio to `sfdec_set_playback_speed`. The MediaCodec helper updates
  the snapping rate and invalidates its phase origin under the codec lock. The
  first subsequent frame establishes the phase for the new time mapping; no
  MediaCodec reset is required.
- A continuous PCM speed commit also preserves spacing from the last submitted
  frame. Video can already be scheduled up to 200ms ahead of heard audio, so
  retaining the wall offset alone does not prevent a gap when queued frames are
  remapped. The renderer carries a temporary deadline correction at that boundary,
  then converges to the audio-derived schedule by at most 4ms and 10% of a frame
  per successful submission, including the final step. The earlier 1ms limit
  allowed rapid ramp-down commits to accumulate a correction that took seconds
  to repay at 24fps. Lookahead retries do not spend this correction;
  original media timestamp gaps remain intact. This applies to atempo, Sonic and
  PlaybackParams PCM commits, not passthrough. Open, seek, flush, reanchor and pause
  clear it, including when they overlap an unlocked buffer release.
  `video_render_diag` reports the residual as `speed_correction_us`.
- After an actual PCM speed-ratio change, compositor submission lead is limited
  to 50ms instead of 200ms. This keeps future frames in the engine queue, where
  subsequent commits can still remap them, and reduces the phase correction
  accumulated by rapid ramp-down steps. The short lead remains at the final
  ratio (including 1.2x or 1x) until pause/seek/flush resets speed state; non-1x
  playback also uses the short lead after a reset. Ordinary 1x startup and
  passthrough retain their existing 200ms lead.
- Software PCM publications carry their applied mixer-latency calibration at
  every speed. After a speed change, a change in this calibration moves the raw
  renderer offset, while the opposite change is added to the existing bounded
  speed correction. It therefore converges without a deadline jump or a second
  independent correction. Calibration updates during an unlocked buffer release
  remain pending for the next submission. Missing/DAC evidence, output replacement,
  manual delay changes and lifecycle resets invalidate the calibration baseline.

## Operational Notes and Caveats

- The render offset is initialized from a stable fallback when timing is
  unreliable; this provides a consistent A/V alignment at startup.
- Subsequent corrections are event-driven (seek/resume/speed, plus the one-shot
  decoded-PCM startup correction) and applied via bounded slew to avoid visible
  acceleration or stutter.
- For `passthrough=2`, the renderer uses the centralized Mode 2 interpolator,
  preferably through a fresh `put_time`. Initial and seek reanchors clamp an
  implausible forward lead and prevent a backward seek from anchoring behind the
  current video frame.
- Trusted asynchronous presentation evidence dynamically bounds that
  interpolator. Direct raw routes are broadly eligible by default
  (`stream_mode2_dynamic_all=1`), extending the validated AC3/44.1 kHz profile;
  AC3 recode uses its 48 kHz profile. Freshness and trust checks still gate
  adoption. The renderer observes clock entry/exit and slews its existing
  offset; it does not create a second audio clock.
- Pause preserves the Mode 2 audio phase and shifts an established render offset
  by the paused wall duration. It retains the compressed ledger while resetting
  the presentation-observation epoch. Seek and mid-playback track recreation
  explicitly seed an empty compressed track at the submitted frontier minus
  fixed downstream latency.
- Audio seek preroll waits for the video decoder to reach the epoch-tagged seek
  target. Renderer reanchoring and this video-target handshake are separate from
  audio latency estimation.
- A queued frame is only peeked until presentation is committed. Every
  passthrough startup wait returns to the queue/epoch checks, and unlocked
  latency queries revalidate the frame handle, queue head and renderer
  generation before changing anchors. Seek, decoder flush and clock reanchor
  invalidate outstanding queries even if the entire transition completes while
  the renderer is unlocked. Only output release/render calls hold the rendering
  state that flush waits for; waiting for audio must not block flush.
- Renderer audio-clock references, pause/grace intervals and passthrough hold
  deadlines use 64-bit monotonic milliseconds. Hold activity is explicit, so a
  cancelled deadline cannot become an active wait when device uptime exceeds
  the signed 32-bit millisecond range. Seek preview remains exempt from the
  audio-start hold.
- The same 64-bit monotonic domain is used by AudioTrack observation/cache
  timestamps, Mode 2 interpolation and resume grace, AC3 write pacing, atempo
  commit/pause accounting, and seek-preview/idle deadlines. Producers and
  consumers must both use `atime64()`; widening storage alone still preserves
  an already-wrapped `atime()` value. These wall clocks are distinct from media
  timestamps and bounded durations, which retain their existing units/types.
  The software Android sink also uses 64-bit presentation deadlines; a cleared
  deadline means there is no outstanding presentation to drain.
- Accurate `video->frame_rate_{num,den}` metadata is important. Bad values yield
  incorrect snapping after a speed change, causing jitter in scheduled timestamps.
- MediaCodec output-buffer indices belong to one codec generation. After a
  successful `MediaCodec.flush()`, sfdec2 discards retained native wrappers
  without calling `releaseOutputBuffer()` for their invalidated indices. Buffers
  returned concurrently with the flush receive the same treatment. This applies
  to seek and close teardown and prevents stale output callbacks from crossing a
  codec generation.
- Async seek setup idles the engine before changing parser/CBE state and frame
  queues. Preview target/epoch state is published before restarting it; after
  preview it is idled again until seek drop floors and sync state are ready.
  Pausing alone is insufficient because the async player still fetches packets
  and recycles buffers while paused.
- Seek initialization recycles engine-owned display containers instead of
  dropping their queue head. MediaCodec flush invalidates frame metadata as well
  as native handles, moves unconsumed decoder output back to the decode queue,
  and returns cancelled presentation frames through the sink queue. This keeps
  the small frame pool and sink count intact. Only empty decoder-owned containers
  receive the new epoch; stale output is never admitted as a seek preview.
  Audio flush/anchoring, passthrough preview exemptions and startup/resume holds
  retain their existing policy in all passthrough modes.
- Input submission, output dequeue, and timed output release belong to the same
  flush ownership contract. NDK output dequeue uses a bounded wait, so seek and
  close can wait for all three operations to finish before calling
  `MediaCodec.flush()`. This avoids interrupting an in-flight vendor codec call.

## Decoder output, recovery, and completion

- MediaCodec input is submitted as a complete access unit. An input buffer that
  cannot hold it triggers decoder recovery; the remainder is never submitted as
  an unrelated frame with duplicate timestamp bookkeeping. Input and codec-config
  dequeue waits are bounded, and codec-config submission remains pending until
  accepted. Output timestamp lookup waits for accepted-input bookkeeping.
- Submission, output release, and flush failures propagate to decoder recovery.
  A failed flush cannot establish a new seek generation on the old decoder.
- Natural EOF is separate from flush. FFmpeg returns all buffered frames before
  accepting more input and drains with a single null packet. MediaCodec submits
  input EOS once, processes any final frame carrying EOS, and waits for output
  EOS. Empty EOS buffers do not become video frames. Seek clears drain state.
- Playback completion waits for decoder output, queued presentation, and the
  final scheduled frame deadline/duration. Platform display completion is not
  measured directly; the timed sink uses its scheduled monotonic deadline.
- Software conversion uses the retained AVFrame's geometry and pixel format,
  with checks against destination plane capacities. The public-window software
  sink changes buffer geometry at frame boundaries, after older queued frames.
- MediaCodec retains the existing initial-container-geometry workaround for
  inaccurate vendor startup metadata. Subsequent changes in reported dimensions
  or crop propagate validated visible dimensions, including rotation.

## Subtitle timing and decoder state

- Subtitle packet timestamps enter decoding in the stream TS domain. FFmpeg's
  `AVSubtitle.pts` identifies the composition start; relative display offsets and
  durations are converted from milliseconds into TS units. DVD display offsets
  survive decoding, and a future display event retains its decoder-owned bitmap
  until due. Replayed cues expose only their remaining duration.
- A successful seek resets external cue lookup in both directions. PGS seeks
  recreate the decoder because the bundled decoder has no flush callback;
  palettes, objects, and partial compositions cannot cross the seek boundary.
- Seamless PGS track switching replays from a retained acquisition/epoch PCS.
  Palette/object dependencies do not expire merely because they are older than
  sixty seconds. Future demux lookahead preserves the currently visible epoch.
  The cache remains bounded at 8 MiB/2048 packets; eviction invalidates the whole
  affected track's history, so incomplete history is not replayed. Recovery then
  waits for fresh decoder state rather than seeking audio/video.
- Bitmap coordinates use the subtitle decoder's canvas, including external IDX
  dimensions and palette metadata. PGS stays visible until a subsequent
  composition or clear instead of an arbitrary hundred-second timeout.
- The Android subtitle scheduler must preserve pause state when installing a
  replayed cue and freeze its remaining duration until playback resumes. This is
  a Java-side requirement in `SubtitleManager`, separate from native duration
  clipping and decoder recovery.

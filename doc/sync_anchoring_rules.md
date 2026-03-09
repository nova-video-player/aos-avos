# AVOS Sync & Anchoring Rules

## Introduction

- **`venc_put_time` (TS)**: The last TS value passed into the video sink as the anchor point.
- **`venc_ref_time` (WC)**: The monotonic clock sample taken when `venc_put_time` was set. Together, the sink estimates current TS as `venc_put_time + (now_wc - venc_ref_time)` (android_sync=0).
- **`heard_audio_ts` (TS)**: The audio time that is actually audible at the speakers. Formula: `heard_audio_ts = audio_time - chain_delay_ts`, where `chain_delay_ts` is `smoothed_av_delay` if valid, otherwise a fallback delay. When timing is invalid and atempo is active, the fallback includes the atempo chain delay so speed changes remain latency‑aware.
- **Monotonic clock variables (WC)**: `CLOCK_MONOTONIC` timestamps are used for wall‑clock pacing because they never jump due to system time changes. They provide stable elapsed‑time deltas for TS↔WC anchoring.
- **`timeline_map_apply()`**: Installs a single piecewise‑linear mapping between RST and TS at a given anchor `(rst_anchor, ts_anchor, speed)`. Absolute conversions: `ts = ts_anchor + (rst - rst_anchor) / speed`, `rst = rst_anchor + (ts - ts_anchor) * speed`. Duration conversions use `RST_TO_TS_DELTA` / `TS_TO_RST_DELTA`.
- **`smoothed_av_delay`**: A low‑pass filtered estimate of audio‑video offset (TS) used as a stable proxy for the audible delay. When it is invalid, fall back to `stream_sync_av_delay()`.
- **Best delay provider**: Audio delay is chosen by a single provider: use stable `AudioTrack.getTimestamp()` when available; otherwise fall back to playback‑head latency, then static latency. Timestamp validity is gated by a streak of advancing samples and outlier rejection; fallback is always available when dynamic timing is unstable.
- **Delay validity streak (AudioTrack)**: We require a short run of consecutive advancing samples before marking delay as valid. This avoids anchoring on transient/zero playhead values seen on Sabrina/Kirkwood right after resume or seek. Once the streak is met, we allow a one‑time rebase to measured delay.
- **`put_time_mode`**: Enabled when a sink exposes `put_time()`. In this mode the sync layer keeps audio as the master (via `heard_audio_ts`) but does not disable sync on early frames; the sink owns TS↔WC pacing.

## Time Domain Anchors (Single per Sink)

- **android_sync=0** (`codec_sfdec2.c`): Owns single TS↔WC via `venc_put_time` (TS) / `venc_ref_time` (WC). Anchor from `heard_audio_ts` (audio_time - chain delay) in `stream_set_av_speed()`. `videosink_put_time()` may reanchor only on speed change or when drift exceeds a fixed threshold, with grace/monotonic guards. If delay becomes invalid during steady playback, last‑good delay is held for anchoring so latency compensation does not drop to zero. Manual A/V delay does not shift `put_time` anchors. The sync diff includes `s->av_delay`; for negative delay requests, extra audio hold is applied in `stream_audio.c` (non-passthrough).

- **android_sync=1** (`codec_sfdec2.c` + MediaCodec): Always supplies `render_ts_ns` to MediaCodec. A single render offset (TS↔WC) is initialized at startup. Manual A/V delay is applied at final presentation scheduling (`render_ts_ns = frame_ts + render_offset + av_delay_ts`). For passthrough=2, timing is treated as unreliable and the sink uses a startup hold plus a residual static latency (no slew) to align with audible time. No local wait/drop pacing is used.

**Unification**: `stream_set_av_speed()` normally computes `heard_audio_ts`, calls `timeline_map_apply(rst_from_ts(heard_audio_ts), heard_audio_ts, new_speed)` and `video_sink->put_time(heard_audio_ts)`. When delay is invalid, speed‑change anchoring uses **last‑good delay** (adjusted by atempo delta) if available. If no last‑good delay and **android_sync=1**, it falls back to `current_time_ts` and **defers** sink re‑anchoring to avoid visible catch‑up bursts.

## Heard-Audio Anchor Definition

- `heard_audio_ts = audio_time - stream_sync_av_delay()`
- When timing is **valid**, `heard_audio_ts` uses `s->smoothed_av_delay` (or `_get_anchor_delay_ms()` if no smoothing is available).
- When timing is **invalid**, heard‑time falls back to the raw AudioTrack delay (playhead/static), and if atempo is active it uses the atempo chain delay so speed changes remain latency‑aware.
- Negative anchors are generally avoided, but **android_sync=1** may allow a negative internal anchor at startup to keep MediaCodec timing consistent until audio is ready.
- `heard_audio_ts` is an audio‑side anchor (audible time). Video TS is aligned to it via delay compensation in `stream_sync_av_delay()`.
- `heard_audio_ts` is computed centrally via `stream_get_heard_audio_ts()` and should not be re‑implemented elsewhere.

## Mapping Rules

- Only one RST↔TS mapping exists (`timeline_map_apply`). Every speed change updates it.
- Parser timestamps are always in TS; all sync math uses TS unless explicitly converting with `RST_TO_TS_*` or `TS_TO_RST_*`.
- Heard‑audio anchoring is compatible with both atempo (software) and AudioTrack PlaybackParams (hardware) speed paths; both share the same TS timeline and use the audible clock as the anchor.

## Audio Speed Change Flow

1) Compute `heard_audio_ts` (TS) from current audio time minus chain delay.
2) Convert to RST: `anchor_rst = TS_TO_RST_TIME(heard_audio_ts)`.
3) Apply mapping: `timeline_map_apply(anchor_rst, heard_audio_ts, new_speed)`.
4) Anchor the sink: `video_sink->put_time(heard_audio_ts)` so the TS↔WC anchor matches what is heard.
5) Sinks keep pacing off their own WC reference (`venc_ref_time` or `start_monotonic`) using the new TS anchor.
6) **invalid delay (android_sync=0/1)**: prefer last‑good delay for speed‑change anchoring (adjusted by atempo delta). This avoids anchoring on a stale/zero heard_ts when timing is unstable.
7) **android_sync=1 + no last‑good delay**: mapping falls back to `current_time_ts` (not `heard_audio_ts`) and the sink re‑anchor is deferred to avoid catch‑up bursts.
8) **android_sync=0 + atempo**: optionally issue a frame‑accurate realignment seek (see below) to pull video/audio back to the audible TS without flushing.

## Android Path (sfdec2)

- **Sink selection**: On Android, the active video sink is `sfdec2` (`codec_sfdec2.c`). The sink is created via `stream_get_default_video_sink()` but the name logged is `sfdec2`.
- **android_sync=0**: `codec_sfdec2.c` owns TS↔WC anchoring (`venc_put_time`, `venc_ref_time`) and pacing (blit wait/drop). `stream_sync.c` still computes A/V delay and audio master timing, but the sink uses its own WC anchor to schedule frames.
- **android_sync=1**: The sink bypasses its own wait/drop path and delegates scheduling to MediaCodec. `codec_sfdec2.c` computes `render_ts_ns` from the render offset and always passes it to MediaCodec. The offset starts from static latency and slews toward dynamic delay once timing is valid, except passthrough=2 which uses a startup hold plus residual static latency and skips slew.
- **Manual A/V delay policy**: keep anchors/diff in physical time; apply user delay at presentation scheduling. In `codec_sfdec2.c`, this is:
  - `android_sync=1`: `render_ts_ns`.
  - `android_sync=0`: anchors remain physical in the sink. The sync diff includes `s->av_delay`; negative delay is realized by audio-side hold in `stream_audio.c` (non-passthrough).
- **put_time_mode**: When the sink provides `put_time()`, the sync layer uses `heard_audio_ts` for the diff calculation but leaves pacing to the sink.

## In-Flight Data During Speed Changes

- **No draining**: Queues are not flushed or drained on speed change. In‑flight audio/video retains its original TS.
- **Audio in flight**: Samples already in the audio sink continue at the old tempo. New tempo applies to decoded output after the change. The mapping anchor is set to `heard_audio_ts` so the timeline pins to what is actually audible.
- **Video in flight**: Frames already queued keep their TS and remain ordered. The sink is re‑anchored to `heard_audio_ts`, so pacing stays aligned without flushing.
- **Speed‑change seek (android_sync=0 + atempo)**: Seek to a keyframe (RST), then drop decoded video frames and audio chunks until the shared target TS is reached. This keeps the target monotonic while minimizing visible jumps.

## Time Continuity Guarantee

- **RST↔TS continuity**: `timeline_map_apply()` changes only the slope (speed) while pinning the current `heard_audio_ts`, so TS does not jump.
- **TS↔WC continuity**: The sink anchor is reset with `video_sink->put_time(heard_audio_ts)` so WC pacing advances smoothly from the same TS reference.
- **android_sync=1 stability**: Repeated anchors with the same TS must not update `venc_ref_time`; otherwise the WC mapping jumps forward and MediaCodec will wait, causing stop/go stutter. At startup, a negative anchor may be used internally to preserve continuity.

## atempo Speed Change Behavior

- **No forced rebuild**: `stream_filter_audio_atempo.c::_filter()` updates speed in place via `atempo_update_speed()`; it rebuilds the filter graph only if the runtime update fails.
- **Delay recomputation**: `stream_filter_audio_atempo.c::_delay()` recomputes delay from:
  - FIFO output samples (already time‑scaled), plus
  - WSOLA internal delay based on fragment size, scaled by `1 / current_speed`.
- **Continuity**: FIFO contents are preserved across speed changes, so the audible timeline remains continuous; the mapping anchor (`heard_audio_ts`) absorbs any small internal delay shift.

## Low/High Latency Devices

- There are no special‑case code paths for high‑latency devices; behavior is unified.
- High latency only increases `stream_sync_av_delay()` (and thus `heard_audio_ts`), so anchors shift earlier to match what is actually heard.
- Reanchor thresholds are fixed; **speed‑change** realignment may use a seek‑and‑drop path (android_sync=0 + atempo) to converge faster after large latency ramps.

## Pause/Resume and Seek

- **Pause**: WC continues to advance; TS does not. On resume the sink is re‑anchored to `heard_audio_ts`, so the wall‑clock gap is ignored.
- **Resume (android_sync=0)**:
  - First audio output after resume triggers a **static‑latency rebase** if AudioTrack timing is invalid. This is used on devices like **Sabrina (Chromecast 4K)** where `getTimestamp()` can be invalid for hundreds of ms after resume.
  - When AudioTrack delay later becomes valid (streak threshold), a **second rebase** aligns to the measured delay. This can cause a visible jump but yields accurate alignment.
- **Resume (android_sync=1)**:
  - The sink continues to render via `render_ts_ns`; no local wait/drop pacing is applied.
  - While AudioTrack delay is invalid, video can free‑run against its current anchor (timing unavailable). This can show a small initial A/V offset.
  - When delay becomes valid (streak threshold), a one‑time **delay‑valid rebase** aligns audio_time to `video_time + measured_delay`. This is why A/V appears to converge quickly after start on some devices.
- **Seek**: The UI target is RST. After seek, the parser emits new TS timestamps from the new RST position, and a short convergence window is applied (android_sync=0) to align with `heard_audio_ts`, then the sink is re‑anchored once and gating stops to avoid stutter.
- **Speed‑change realignment seek**: Uses `stream_seek_time_frame_accurate(rst_target, ts_target, BACKWARD)` so the parser seeks to a keyframe, then `_stream_play_n_frames` drops frames until `ts_target`. Audio chunks are dropped until the same `ts_target`.
- **Broken audio PTS (post‑seek)**: Some files emit non‑monotonic audio PTS after seek (e.g., audio restarts near 0 while video is at 26s). To avoid a video freeze, `stream_audio.c` guards against large backward jumps after seek: first audio far behind video is rebased to `video_time`, and later backward PTS (>1s) are ignored. This is a minimal safety net for malformed files, not the nominal path.

## Speed Change Cadence

- No debounce/coalescing is required for correctness: TS/RST continuity is preserved by `timeline_map_apply()` and the sink anchor.
- Rapid changes may increase CPU or audible artifacts, but they do not break monotonic timelines.

## Delay Jitter and Stability

- The biggest stability risk is **noisy delay reporting** (e.g., `stream_sync_av_delay()` returning 0 or jumping). That makes `heard_audio_ts` jump and can force unnecessary reanchors.
- This is why `smoothed_av_delay` is preferred when valid: it damps jitter in the audio chain delay and keeps the audible anchor stable.
- `smoothed_av_delay` is computed in `stream_sync_audio()` using low‑pass filtering: either a LWMA (`stream_calc_lwma`) or an exponential smoother `smoothed = (prev * delay_fb + current * (1000 - delay_fb)) / 1000`.
- **android_sync=0**: if delay validity drops during steady playback, last‑good delay is held for anchoring to avoid losing latency compensation.

## 1.0x Stutter Observations and Remedies

- **Low‑latency devices (e.g., Shield)**: Stutter showed up as periodic sink drops (`DROP blit=...`) in `codec_sfdec2` while audio latency was stable (~70–120ms). The drop policy was too aggressive for steady 1.0x cadence.
- **High‑latency devices (e.g., Google streamer 4K)**: Stutter appeared as anchor jitter during AudioTrack warm‑up and latency ramps (headpos=0 → delay jumps). This caused backward anchor resets and pacing discontinuities without explicit drops.

**Remedies applied (android_sync=0, sfdec2):**
- **Convergence window (post‑seek, android_sync=0)**: For ~500 ms after a seek, video gating uses heard‑time (no early‑start bias) to let audio catch up. Once the window expires, the sink is re‑anchored to `heard_audio_ts` and further gating stops, preventing per‑frame stalls.
- **Monotonic anchor**: prevent backward anchor movement during steady playback; only allow resets on explicit speed change or large discontinuity.
- **Resume rebasing**: On devices with unstable AudioTrack timing after resume (e.g., **Sabrina/Chromecast 4K**), the first audio output rebases to static latency, then a later rebase to measured delay corrects the remaining offset.

**Remedies applied (android_sync=1, MediaCodec):**
- **Always render_ts**: the sink always calls MediaCodec with `render_ts_ns`, avoiding a pacing mode switch.
- **Static-to-dynamic slew**: initialize offset with static latency, then slew toward dynamic delay when timing becomes valid.
- **Passthrough=1 (IEC)**: use playback‑head delay when available; fall back to static latency if head position is unstable.
- **Passthrough=2 startup hold**: hold video until audio_time is valid, then initialize with residual static latency to avoid double‑counting. Slew is event‑driven only (seek/resume/speed).
  - **Note on Kirkwood (Google streamer 4K)**: long invalid‑delay windows after resume can delay the slew, so the initial offset is static until delay validity is established.

## Filters and Time Domains

- Audio filters (including atempo) operate on raw PCM samples, not timestamps. They are unaffected by RST/TS/WC mappings.
- Time domains only affect timestamp accounting and A/V sync math; filter processing remains monotonic in the physical sample stream.
- Therefore, discontinuities in delay estimates impact anchoring/sync, not filter sample order.

## FFmpeg atempo Delay Behavior

- `af_atempo.c` updates tempo via `process_command()` which calls `yae_update()` to reset fragment origins; it does not expose an explicit delay value.
- Our delay estimate (`stream_filter_audio_atempo.c::_delay()`) is **derived**, not read from FFmpeg:
  - FIFO output samples (can drop to 0 if the FIFO is empty or reset),
  - WSOLA internal delay computed from fragment size and scaled by `1 / speed` (goes to 0 when speed returns to 1.0).
- If the atempo graph is rebuilt or flushed, FIFO contents are reset, so the computed delay can jump toward zero. This is expected and is another reason to anchor with `smoothed_av_delay` rather than raw delay.
- To avoid transient zeros during speed changes, `_delay()` rate-limits downward jumps using a WSOLA-based stabilization window (derived from internal delay + FIFO).

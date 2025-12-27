# AVOS Sync & Anchoring Rules

## Introduction

- **`venc_put_time` (TS)**: The last TS value passed into the video sink as the anchor point.
- **`venc_ref_time` (WC)**: The monotonic clock sample taken when `venc_put_time` was set. Together, the sink estimates current TS as `venc_put_time + (now_wc - venc_ref_time)`.
- **`heard_audio_ts` (TS)**: The audio time that is actually audible at the speakers. Formula: `heard_audio_ts = audio_time - chain_delay_ts - RST_TO_TS_DELTA(av_delay)`, where `chain_delay_ts` is `smoothed_av_delay` if valid, otherwise `stream_sync_av_delay()`.
- **Monotonic clock variables (WC)**: `CLOCK_MONOTONIC` timestamps are used for wall‑clock pacing because they never jump due to system time changes. They provide stable elapsed‑time deltas for TS↔WC anchoring.
- **`timeline_map_apply()`**: Installs a single piecewise‑linear mapping between RST and TS at a given anchor `(rst_anchor, ts_anchor, speed)`. Absolute conversions: `ts = ts_anchor + (rst - rst_anchor) / speed`, `rst = rst_anchor + (ts - ts_anchor) * speed`. Duration conversions use `RST_TO_TS_DELTA` / `TS_TO_RST_DELTA`.
- **`smoothed_av_delay`**: A low‑pass filtered estimate of audio‑video offset (TS) used as a stable proxy for the audible delay. When it is invalid, fall back to `stream_sync_av_delay()`.

## Time Domain Anchors (Single per Sink)

- **android_sync=0** (`codec_sfdec2.c`): Owns single TS↔WC via `venc_put_time` (TS) / `venc_ref_time` (WC). Anchor only from `heard_audio_ts` (audio_time - chain delay - av_delay) in `stream_set_av_speed()`. `videosink_put_time()` may reanchor only on speed change or when drift exceeds a single fixed threshold.

- **android_sync=1** (`sfdec_ndkmediacodec.cpp`): Owns single TS↔WC via `start_off` (TS first-frame) / `start_monotonic` (WC start). Stream/sink layers must not manage WC anchors; MediaCodec handles resets/projection.

**Unification**: `stream_set_av_speed()` computes `heard_audio_ts`, calls `timeline_map_apply(rst_from_ts(heard_audio_ts), heard_audio_ts, new_speed)` and `video_sink->put_time(heard_audio_ts)`. No other layers touch WC anchors.

## Heard-Audio Anchor Definition

- `heard_audio_ts = audio_time - stream_sync_av_delay() - RST_TO_TS_DELTA(av_delay)`
- Use `s->smoothed_av_delay` when valid; otherwise fall back to `stream_sync_av_delay()`.
- Clamp to zero; never allow negative anchors.
- `heard_audio_ts` is an audio‑side anchor (audible time). Video TS is aligned to it via delay compensation in `stream_sync_av_delay()`.

## Mapping Rules

- Only one RST↔TS mapping exists (`timeline_map_apply`). Every speed change updates it.
- Parser timestamps are always in TS; all sync math uses TS unless explicitly converting with `RST_TO_TS_*` or `TS_TO_RST_*`.
- Heard‑audio anchoring is compatible with both atempo (software) and AudioTrack PlaybackParams (hardware) speed paths; both share the same TS timeline and use the audible clock as the anchor.

## Audio Speed Change Flow

1) Compute `heard_audio_ts` (TS) from current audio time minus chain delay and `av_delay`.
2) Convert to RST: `anchor_rst = TS_TO_RST_TIME(heard_audio_ts)`.
3) Apply mapping: `timeline_map_apply(anchor_rst, heard_audio_ts, new_speed)`.
4) Anchor the sink: `video_sink->put_time(heard_audio_ts)` so the TS↔WC anchor matches what is heard.
5) Sinks keep pacing off their own WC reference (`venc_ref_time` or `start_monotonic`) using the new TS anchor.

## Android Path (sfdec2)

- **Sink selection**: On Android, the active video sink is `sfdec2` (`codec_sfdec2.c`). The sink is created via `stream_get_default_video_sink()` but the name logged is `sfdec2`.
- **android_sync=0**: `codec_sfdec2.c` owns TS↔WC anchoring (`venc_put_time`, `venc_ref_time`) and pacing (blit wait/drop). `stream_sync.c` still computes A/V delay and audio master timing, but the sink uses its own WC anchor to schedule frames.
- **android_sync=1**: The sink bypasses its own wait/drop path and delegates scheduling to MediaCodec. `sfdec_ndkmediacodec.cpp` maintains the TS↔WC anchor (`start_off`, `start_monotonic`) and releases buffers at an absolute WC time. `stream_sync.c` still runs, but does not own WC anchoring.

## In-Flight Data During Speed Changes

- **No draining**: Queues are not flushed or drained on speed change. In‑flight audio/video retains its original TS.
- **Audio in flight**: Samples already in the audio sink continue at the old tempo. New tempo applies to decoded output after the change. The mapping anchor is set to `heard_audio_ts` so the timeline pins to what is actually audible.
- **Video in flight**: Frames already queued keep their TS and remain ordered. The sink is re‑anchored to `heard_audio_ts`, so pacing stays aligned without flushing.

## Time Continuity Guarantee

- **RST↔TS continuity**: `timeline_map_apply()` changes only the slope (speed) while pinning the current `heard_audio_ts`, so TS does not jump.
- **TS↔WC continuity**: The sink anchor is reset with `video_sink->put_time(heard_audio_ts)` so WC pacing advances smoothly from the same TS reference.

## atempo Speed Change Behavior

- **No forced rebuild**: `stream_filter_audio_atempo.c::_filter()` updates speed in place via `atempo_update_speed()`; it rebuilds the filter graph only if the runtime update fails.
- **Delay recomputation**: `stream_filter_audio_atempo.c::_delay()` recomputes delay from:
  - FIFO output samples (already time‑scaled), plus
  - WSOLA internal delay based on fragment size, scaled by `1 / current_speed`.
- **Continuity**: FIFO contents are preserved across speed changes, so the audible timeline remains continuous; the mapping anchor (`heard_audio_ts`) absorbs any small internal delay shift.

## Low/High Latency Devices

- There are no special‑case code paths for high‑latency devices; behavior is unified.
- High latency only increases `stream_sync_av_delay()` (and thus `heard_audio_ts`), so anchors shift earlier to match what is actually heard.
- Reanchor thresholds are fixed; no adaptive thresholds or seek‑realignment heuristics are applied.

## Pause/Resume and Seek

- **Pause**: WC continues to advance; TS does not. On resume the sink is re‑anchored to `heard_audio_ts`, so the wall‑clock gap is ignored.
- **Resume**: `heard_audio_ts` is computed from the paused audio clock minus chain delay; `video_sink->put_time(heard_audio_ts)` resets the TS↔WC anchor.
- **Seek**: The UI target is RST. After seek, the parser emits new TS timestamps from the new RST position, and the sink is re‑anchored to the new `heard_audio_ts` so playback resumes without a TS discontinuity.

## Speed Change Cadence

- No debounce/coalescing is required for correctness: TS/RST continuity is preserved by `timeline_map_apply()` and the sink anchor.
- Rapid changes may increase CPU or audible artifacts, but they do not break monotonic timelines.

## Delay Jitter and Stability

- The biggest stability risk is **noisy delay reporting** (e.g., `stream_sync_av_delay()` returning 0 or jumping). That makes `heard_audio_ts` jump and can force unnecessary reanchors.
- This is why `smoothed_av_delay` is preferred when valid: it damps jitter in the audio chain delay and keeps the audible anchor stable.
- `smoothed_av_delay` is computed in `stream_sync_audio()` using low‑pass filtering: either a LWMA (`stream_calc_lwma`) or an exponential smoother `smoothed = (prev * delay_fb + current * (1000 - delay_fb)) / 1000`.

## FFmpeg atempo Delay Behavior

- `af_atempo.c` updates tempo via `process_command()` which calls `yae_update()` to reset fragment origins; it does not expose an explicit delay value.
- Our delay estimate (`stream_filter_audio_atempo.c::_delay()`) is **derived**, not read from FFmpeg:
  - FIFO output samples (can drop to 0 if the FIFO is empty or reset),
  - WSOLA internal delay computed from fragment size and scaled by `1 / speed` (goes to 0 when speed returns to 1.0).
- If the atempo graph is rebuilt or flushed, FIFO contents are reset, so the computed delay can jump toward zero. This is expected and is another reason to anchor with `smoothed_av_delay` rather than raw delay.

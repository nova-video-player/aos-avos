# AVOS Sync & Anchoring Rules

## Introduction

- **`venc_put_time` (TS)**: The last TS value passed into the video sink as the anchor point.
- **`venc_ref_time` (WC)**: The monotonic clock sample taken when `venc_put_time` was set. Together, the sink estimates current TS as `venc_put_time + (now_wc - venc_ref_time)` (android_sync=0).
- **`heard_audio_ts` (TS)**: The audio time that is estimated to be audible at the speakers. Derivation is **centralized** in `stream_get_heard_audio_ts()`.
  - **Steady State**: `heard_audio_ts = audio_time - chain_delay_ts`, where `chain_delay_ts` is `smoothed_av_delay` if valid.
  - **Mode 2 Startup Fill Window**: For high-latency passthrough/recoding, a synthetic timeline is used until submitted audio advances at real-time speed or timeout: `heard_ts = fill_start_pts - static_latency + elapsed_wall_clock`.
- **Monotonic clock variables (WC)**: `CLOCK_MONOTONIC` timestamps are used for wall‑clock pacing because they never jump due to system time changes. They provide stable elapsed‑time deltas for TS↔WC anchoring.
- **`timeline_map_apply()`**: Installs a single piecewise‑linear mapping between RST and TS at a given anchor `(rst_anchor, ts_anchor, speed)`. Absolute conversions: `ts = ts_anchor + (rst - rst_anchor) / speed`, `rst = rst_anchor + (ts - ts_anchor) * speed`. Duration conversions use `RST_TO_TS_DELTA` / `TS_TO_RST_DELTA`.
- **`smoothed_av_delay`**: A low‑pass filtered estimate of audio‑video offset (TS) used as a stable proxy for the audible delay. When it is invalid, fall back to `stream_sync_av_delay()`.
- **Best delay provider**: Audio delay is chosen by a single provider: use stable `AudioTrack.getTimestamp()` when available; otherwise fall back to playback‑head latency, then static latency.
- **`put_time_mode`**: Enabled when a sink exposes `put_time()`. In this mode the sync layer keeps audio as the master (via `heard_audio_ts`) but does not disable sync on early frames; the sink owns TS↔WC pacing.

## Time Domain Anchors (Single per Sink)

- **android_sync=0** (`codec_sfdec2.c`): Owns single TS↔WC via `venc_put_time` (TS) / `venc_ref_time` (WC). Anchor from `heard_audio_ts` (centralized in `stream_get_heard_audio_ts()`). `videosink_put_time()` may reanchor only on speed change or when drift exceeds a fixed threshold, with grace/monotonic guards. Manual A/V delay does not shift `put_time` anchors.

- **android_sync=1** (`codec_sfdec2.c` + MediaCodec): Always supplies `render_ts_ns` to MediaCodec. For init/reanchor windows, anchor selection is **put_time-authoritative when fresh** (`venc_put_time`), with fallback to recomputed `stream_get_heard_audio_ts()`. For passthrough=2, timing uses the centralized synthetic fill window and skips slew.

## Heard-Audio Anchor Definition

- `heard_audio_ts` is computed centrally via `stream_get_heard_audio_ts()` and is the **single source of truth** for all synchronization and anchoring.
- **Mode 2 Startup (Fill Window)**: For passthrough modes >= 2 and AC3 recoding, the derivation uses **synthetic wall-clock pacing** from the `first_pts` until submitted audio has advanced at real-time speed for a short window (or timeout). This shields the video scheduler from the uneven producer-side advancement rate during physical buffer fill.
- The short real-time advancement check is only a fill-window exit gate: it does not estimate, persist, or apply any latency correction.
- **Integrated Startup Clamp**: A 50ms startup clamp allows video to start promptly on high-latency devices. For Mode 2, this clamp is integrated into the synthetic window via a base re-alignment to ensure continuity, but it is suppressed until the passthrough playhead has proven it advances. This prevents blind startup writes from poisoning the fill base before platform timing is trustworthy.
- **Mode 2 Fill Exit Cap**: At fill exit, the audio rebase is capped against recent video progress plus effective latency and a small margin. This prevents multi-second anchors when video startup is still held or barely moving.
- **Steady State**: Once the physical buffer is stable, `heard_audio_ts` follows the logical clock. For Mode 2, the delay offset is the platform static latency, with wall-clock interpolation between compressed write bursts. Without this interpolation, `audio_time - static_latency` advances in burst-sized steps, so the video scheduler sees a sawtooth A/V diff even when average latency is correct.
- **Rebase Handoff**: At the end of the fill window, `audio_time` is rebased to the synthetic baseline to ensure a continuous transition to logical clocking without "jumps."

## Mapping Rules

- Only one RST↔TS mapping exists (`timeline_map_apply`). Absolute timestamps are always in TS.
- Heard‑audio anchoring is compatible with both atempo (software) and hardware speed paths.

## Android Path (sfdec2)

- **android_sync=0**: `codec_sfdec2.c` owns TS↔WC anchoring and pacing (blit wait/drop).
- **android_sync=1**: The sink bypasses wait/drop and delegates scheduling to MediaCodec via `render_ts_ns`.
  - **passthrough=2 post-seek re-init rule**: synchronization anchors (`sink_ref_time`) are strictly reset on every seek. This ensures the startup fill logic (including the clamp) re-fires for every new seek epoch.
  - **passthrough=2 timing source**: audio TS progression uses compressed-frame `fakeSize` as logical PCM-duration. DTS/DTS-HD follows parser duration first because raw mode 2 writes can be 512-sample DTS frames; otherwise the clock can run 3x too fast. Other formats use codec metadata when available, then logical base units (1536 for EAC3, 1280 for TrueHD).
  - **passthrough=2 delay source**: the scheduler uses static passthrough latency from the platform plus startup fill and steady-state interpolation. It does not maintain an adaptive calibration offset.
- **Manual A/V delay policy**: keep anchors physical; apply user delay at final presentation scheduling.

## Pause/Resume and Seek

- **Pause**: On resume the sink is re‑anchored to `heard_audio_ts`.
- **Seek**: After seek, synchronization state is reset (`sink_ref_time = -1`). The video clock is immediately updated to the target. The first audible audio frame triggers the centralized **Startup Fill Window** to establish a latency-compensated anchor for the new epoch.
  - **EAC3/AC3**: Uses a tight 32ms startup hold threshold.
  - **TrueHD**: Uses a relaxed 300ms hold threshold to accommodate extremely high packet cadence (1200/sec) and prevent video freezes while filling the HAL pipeline.

## Delay Jitter and Stability

- `smoothed_av_delay` is preferred when valid to damp jitter in the audio chain.
- The **Synthetic Fill Window** specifically targets stability on high-latency devices by providing a monotonic real-time baseline while physical latency is reporting "static." In mode 2, the baseline uses platform static latency, waits for passthrough playhead proof before clamping to submitted audio time, and caps the final rebase against video progress.
- Internal sync stability does not prove physical lipsync when downstream devices add unreported decode/DSP latency after HDMI/ARC. That class of offset must be handled as route/user delay outside the core scheduler.

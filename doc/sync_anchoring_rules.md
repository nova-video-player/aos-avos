# AVOS Sync & Anchoring Rules

## Introduction

- **`venc_put_time` (TS)**: The last TS value passed into the video sink as the anchor point.
- **`venc_ref_time` (WC)**: The monotonic clock sample taken when `venc_put_time` was set. Together, the sink estimates current TS as `venc_put_time + (now_wc - venc_ref_time)` (android_sync=0).
- **`heard_audio_ts` (TS)**: The audio time that is estimated to be audible at the speakers. Derivation is **centralized** in `stream_get_heard_audio_ts()`.
  - **Steady State**: `heard_audio_ts = audio_time - chain_delay_ts`, where `chain_delay_ts` is `smoothed_av_delay` if valid.
  - **Mode 2 passthrough**: `audio_time` advances from submitted compressed packet duration; heard time subtracts the static passthrough latency baseline plus an optional bounded positive residual.
- **Monotonic clock variables (WC)**: `CLOCK_MONOTONIC` timestamps are used for wall‑clock pacing because they never jump due to system time changes. They provide stable elapsed‑time deltas for TS↔WC anchoring.
- **`timeline_map_apply()`**: Installs a single piecewise‑linear mapping between RST and TS at a given anchor `(rst_anchor, ts_anchor, speed)`. Absolute conversions: `ts = ts_anchor + (rst - rst_anchor) / speed`, `rst = rst_anchor + (ts - ts_anchor) * speed`. Duration conversions use `RST_TO_TS_DELTA` / `TS_TO_RST_DELTA`.
- **`smoothed_av_delay`**: A low‑pass filtered estimate of audio‑video offset (TS) used as a stable proxy for the audible delay. When it is invalid, fall back to `stream_sync_av_delay()`.
- **Best delay provider**: Audio delay is chosen by a single provider: use stable `AudioTrack.getTimestamp()` when available; otherwise fall back to playback‑head latency, then static latency.
- **`put_time_mode`**: Enabled when a sink exposes `put_time()`. In this mode the sync layer keeps audio as the master (via `heard_audio_ts`) but does not disable sync on early frames; the sink owns TS↔WC pacing.

## Time Domain Anchors (Single per Sink)

- **android_sync=0** (`codec_sfdec2.c`): Owns single TS↔WC via `venc_put_time` (TS) / `venc_ref_time` (WC). Anchor from `heard_audio_ts` (centralized in `stream_get_heard_audio_ts()`). `videosink_put_time()` may reanchor only on speed change or when drift exceeds a fixed threshold, with grace/monotonic guards. Manual A/V delay does not shift `put_time` anchors.

- **android_sync=1** (`codec_sfdec2.c` + MediaCodec): Always supplies `render_ts_ns` to MediaCodec. For init/reanchor windows, anchor selection is **put_time-authoritative when fresh** (`venc_put_time`), with fallback to recomputed `stream_get_heard_audio_ts()`.

## Heard-Audio Anchor Definition

- `heard_audio_ts` is computed centrally via `stream_get_heard_audio_ts()` and is the **single source of truth** for all synchronization and anchoring.
- **Mode 2 passthrough**: For passthrough mode 2 and mode-2 AC3 recoding, `audio_time` is advanced from the submitted compressed packet duration (`fakeSize`). Heard time is then derived from that logical clock by subtracting platform static latency. If mode-2 dynamic delay is enabled, a capped, slewed, positive-only residual may be added above the static baseline.
- **Mode 2 startup**: startup still anchors on the first committed audio output and uses the same centralized heard-time calculation. The older synthetic fill-window/rebase handoff experiments are not part of the current code path.
- **Mode 2 diagnostics**: logs expose packet-duration source and write-timeline geometry (`fakeSize`, `chunk_us`, `dur_bpf`, `dur_rate`) so packet timing can be audited without changing the scheduler.

## Mapping Rules

- Only one RST↔TS mapping exists (`timeline_map_apply`). Absolute timestamps are always in TS.
- Heard‑audio anchoring is compatible with both atempo (software) and hardware speed paths.

## Android Path (sfdec2)

- **android_sync=0**: `codec_sfdec2.c` owns TS↔WC anchoring and pacing (blit wait/drop).
- **android_sync=1**: The sink bypasses wait/drop and delegates scheduling to MediaCodec via `render_ts_ns`.
  - **passthrough=2 post-seek re-init rule**: synchronization anchors (`sink_ref_time`) are strictly reset on every seek so the next committed compressed write establishes a fresh latency-compensated epoch.
  - **passthrough=2 timing source**: audio TS progression uses compressed-frame `fakeSize` as logical PCM-duration. DTS/DTS-HD follows parser duration first because raw mode 2 writes can be 512-sample DTS frames; otherwise the clock can run 3x too fast. Other formats use codec metadata when available, then logical base units (1536 for EAC3/AC3, 1280 for TrueHD).
  - **passthrough=2 delay source**: the scheduler uses static passthrough latency from the platform as baseline. If mode-2 dynamic delay is enabled, stable AudioTrack evidence can add a bounded positive residual, but stream-level last-good fallback is not treated as fresh sink evidence.
- **Manual A/V delay policy**: keep anchors physical; apply user delay at final presentation scheduling.

## Pause/Resume and Seek

- **Pause**: On resume the sink is re‑anchored to `heard_audio_ts`.
- **Seek**: After seek, synchronization state is reset (`sink_ref_time = -1`). The video clock is immediately updated to the target. The first committed audio output establishes the latency-compensated anchor for the new epoch.
  - **EAC3/AC3**: Uses a tight 32ms startup hold threshold.
  - **TrueHD**: Uses a relaxed 300ms hold threshold to accommodate extremely high packet cadence (1200/sec) and prevent video freezes while filling the HAL pipeline.

## Delay Jitter and Stability

- `smoothed_av_delay` is preferred when valid to damp jitter in the audio chain.
- Mode 2 deliberately keeps the core model simple: submitted packet duration plus static latency baseline, with only a conservative positive residual when dynamic delay evidence is stable. It does not currently use a synthetic fill window or a steady-state interpolation layer.
- Internal sync stability does not prove physical lipsync when downstream devices add unreported decode/DSP latency after HDMI/ARC. That class of offset must be handled as route/user delay outside the core scheduler.

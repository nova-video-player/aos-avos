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
- **Latency terminology**: geometry/app latency is local AudioTrack buffer
  geometry. Pipeline latency is the conservative platform estimate
  `max(track_latency, system_latency + app_latency)`. Static latency means the
  selected fallback delay for the current path; it is not necessarily raw
  `AudioTrack.getLatency()`. See `doc/delay_estimation.md`.
- **`put_time_mode`**: Enabled when a sink exposes `put_time()`. In this mode the sync layer keeps audio as the master (via `heard_audio_ts`) but does not disable sync on early frames; the sink owns TS↔WC pacing.

## Time Domain Anchors (Single per Sink)

- **android_sync=0** (`codec_sfdec2.c`): Owns single TS↔WC via `venc_put_time` (TS) / `venc_ref_time` (WC). Anchor from `heard_audio_ts` (centralized in `stream_get_heard_audio_ts()`). `videosink_put_time()` may reanchor only on speed change or when drift exceeds a fixed threshold, with grace/monotonic guards. Manual A/V delay does not shift `put_time` anchors.

- **android_sync=1** (`codec_sfdec2.c` + MediaCodec): Always supplies `render_ts_ns` to MediaCodec. For init/reanchor windows, anchor selection is **put_time-authoritative when fresh** (`venc_put_time`), with fallback to recomputed `stream_get_heard_audio_ts()`.

## Heard-Audio Anchor Definition

- `heard_audio_ts` is computed centrally via `stream_get_heard_audio_ts()` and is the **single source of truth** for all synchronization and anchoring.
- **Mode 2 passthrough**: For passthrough mode 2 and mode-2 AC3 recoding, `audio_time` is advanced from the submitted compressed packet duration (`fakeSize`). Heard time is then derived from that logical clock by subtracting the selected static latency baseline. Current validated mode-2 policy is codec-aware: plain AC3/EAC3 uses the larger pipeline latency, while DTS/DTS-HD, TrueHD, and DDP/JOC use app-buffer geometry latency. If mode-2 dynamic delay is enabled, a capped, slewed, positive-only residual may be added above the static baseline.
- **Mode 2 startup**: startup still anchors on the first committed audio output and uses the same centralized heard-time calculation. The older synthetic fill-window/rebase handoff experiments are not part of the current code path.
- **Mode 2 diagnostics**: logs expose packet-duration source, selected latency source, and write-timeline geometry (`fakeSize`, `chunk_us`, `dur_bpf`, `dur_rate`) so packet timing and codec latency policy can be audited without changing the scheduler.

## Sync-Mode Selection Principle

Do not treat `STREAM_SYNC_SAMPLES` as a global replacement for PTS-based
sync. Select the sync mode by audio path and codec evidence:

- **PCM decode**: keep PTS anchoring at stream start/seek/discontinuity, then
  advance `audio_time` from committed decoded duration. PCM is already in
  decoded sample units, so forcing global sample sync would lose useful packet
  PTS discontinuity and seek information.
- **FLAC / codecs with unreliable packet PTS but reliable decoded duration**:
  `STREAM_SYNC_SAMPLES` is appropriate because the decoded sample count is the
  most stable clock.
- **Mode 2 compressed passthrough**: `STREAM_SYNC_SAMPLES` is appropriate
  because demuxed packet PTS can be a poor scheduler clock while the submitted
  logical duration (`fakeSize`) is the value that represents the audio clock.
  This applies to AC3, EAC3/DDP, EAC3-JOC/Atmos, DTS/DTS-HD, and TrueHD only
  after validating that `fakeSize` reflects the codec's real logical duration.
- **Mode 1 IEC passthrough and AC3 recoding**: do not inherit mode-2 policy
  automatically. Treat them as separate paths because their packetization,
  buffering, and AudioTrack reporting differ from codec-specific mode 2.

The design rule is: use sample sync only when submitted/decoded logical
duration is more trustworthy than per-packet PTS for that path. The scheduler
still uses the same heard-time model:

```
heard_ts = audio_time - selected_delay
```

Measured delay evidence may update `selected_delay`, but it must not become a
second clock or continuously chase the AudioTrack playhead.

## Mapping Rules

- Only one RST↔TS mapping exists (`timeline_map_apply`). Absolute timestamps are always in TS.
- Heard‑audio anchoring is compatible with both atempo (software) and hardware speed paths.

## Android Path (sfdec2)

- **android_sync=0**: `codec_sfdec2.c` owns TS↔WC anchoring and pacing (blit wait/drop).
- **android_sync=1**: The sink bypasses wait/drop and delegates scheduling to MediaCodec via `render_ts_ns`.
  - **passthrough=2 post-seek re-init rule**: synchronization anchors (`sink_ref_time`) are strictly reset on every seek so the next committed compressed write establishes a fresh latency-compensated epoch.
  - **passthrough=2 timing source**: audio TS progression uses compressed-frame `fakeSize` as logical PCM-duration. DTS/DTS-HD follows parser duration first because raw mode 2 writes can be 512-sample DTS frames; otherwise the clock can run 3x too fast. Other formats use codec metadata when available, then logical base units (1536 for EAC3/AC3, 1280 for TrueHD).
  - **passthrough=2 delay source**: the scheduler uses a selected static baseline, not a second clock. Current testing on Nvidia Shield and Google Streamer 4K supports a codec-aware split: AC3/EAC3 use pipeline latency; DTS/DTS-HD, TrueHD, and DDP/JOC use app-buffer geometry latency. This is a tested policy, not a claim that Android exposes reliable per-codec latency. If mode-2 dynamic delay is enabled later, stable AudioTrack evidence may add a bounded correction, but stream-level last-good fallback is not treated as fresh sink evidence.
- **Manual A/V delay policy**: keep anchors physical; apply user delay at final presentation scheduling.

## PCM Mode 0 — Startup and Seek Sync

### Startup audio hold

Before the first audio write is committed, PCM audio is held until video
reaches `audio_start_pts - anchor_delay`. Hold threshold:
- **PCM (all except TrueHD)**: `-32ms` — audio released as soon as heard
  time is within 32ms of video. Tighter than the old `-150ms` to allow
  earlier delivery; `pcm_reanchor` corrects remaining drift.
- **TrueHD**: `-300ms` — relaxed to accommodate the extremely high packet
  cadence (~1200 bursts/sec) that would cause a freeze on a tight threshold.

`startup_audio_hold` runs as long as needed (not only for `video_time < 1000`),
so seeks late in a file are also covered.

### PCM resume reanchor state machine

On the first write after resume, `stream_sync_pcm_reanchor_arm()` arms a
state machine that selects delay in priority order:

1. Dynamic delay (streak ≥ 3) — most accurate
2. `last_good_delay_ms + live_atempo_delay` — warm fallback
3. Static latency — cold fallback

The reanchor sets `audio_time = sync_v_time + delay` (using the video
thread's last reported position, not the potentially stale `video_time`).
It expires if `seek_epoch` changes mid-resume to prevent stale rebases.

### Post-seek convergence

`_stream_seek_converge_update()` runs a three-part strategy:

- **Part A**: Every `stream_sync_video()` call in put_time mode immediately
  calls `put_time(heard_ts)` — continuous scheduler updates during the
  convergence window instead of one shot at T+500ms.
- **Part B**: If `heard_ts` leads `sync_v_time` by more than
  `STREAM_SEEK_CONVERGE_APPLY_DIFF_MS` (80ms), audio writes are gated so
  audio cannot run away while video catches up.
- **Part C**: After `STREAM_SEEK_CONVERGE_WINDOW_MS` (500ms), a dedicated
  `sfdec2_refresh_sched_anchor() + put_time()` fires and marks convergence
  done. Max wait: `STREAM_SEEK_CONVERGE_MAX_WAIT_MS` (1500ms).

### PCM audio lead gate

`stream_sync_pcm_audio_lead_gate()` holds audio writes when heard_ts
leads `sync_v_time` by too much — a failure mode where AudioTrack accepts
PCM fast after seek while video is still catching up.

- Enter HOLDING: heard_ts leads by ≥ 220ms for 3 consecutive calls, or
  immediately at 250ms.
- Release HOLDING: lead drops below 120ms (100ms hysteresis).
- Maximum 30 holds (~300ms) before forced expiry.

Applies only to PCM (not passthrough), and not during passthrough bursts.

### PCM delay memory

`_stream_pcm_delay_memory_reset()` is called on seek with
`reset_smoothed=0`: `last_good_delay` is wiped (stale after seek),
LWMA history cleared, but `smoothed_av_delay` is kept as a warm starting
point. On full init `reset_smoothed=1` wipes everything.

### Late-audio-start guard

When delay source is static and audio starts significantly ahead of early
video (`video_time < 1000`), `anchor_valid` is suppressed in
`stream_sync_video()` only — not inside `stream_get_heard_audio_ts()`.
This prevents a premature scheduler anchor without affecting `heard_ts`
used elsewhere (e.g. `pcm_audio_lead_gate`).

## Pause/Resume and Seek

- **Pause**: On resume the sink is re‑anchored to `heard_audio_ts`.
- **Seek**: After seek, synchronization state is reset (`sink_ref_time = -1`). The video clock is immediately updated to the target. The first committed audio output establishes the latency-compensated anchor for the new epoch.
  - **PCM**: `startup_audio_hold` gates writes; `pcm_reanchor` then sets the anchor from dynamic/last-good/static delay.
  - **EAC3/AC3 passthrough**: `startup_anchor_commit` sets `audio_time = video_time + latency`; pre-commit negative anchors are suppressed (see below).
  - **TrueHD passthrough**: Uses a relaxed 300ms hold threshold to accommodate extremely high packet cadence (1200/sec) and prevent video freezes while filling the HAL pipeline.

## Passthrough Startup Anchor

### `startup_anchor_commit` (mode 1 and mode 2)

On the first audio write after seek/resume, passthrough sets
`audio_time = video_time + anchor_delay` and calls
`sfdec2_refresh_sched_anchor()`. This restores the pre-refactoring
`android_sync=0` startup alignment that was lost during PCM sync
restructuring. PCM is excluded: it uses `startup_audio_hold` to achieve
the same alignment by holding writes rather than adjusting `audio_time`.

`sfdec2_refresh_sched_anchor()` is required alongside the `audio_time`
change: on seek a `no_sched=1` reanchor fires before the commit,
anchoring the scheduler at the wrong `heard_ts`. Zeroing the anchors
ensures the post-commit `put_time` gets `no_sched_anchor=1` and
reanchors at the correct value regardless of grace period.

### Pre-commit negative anchor guard

Before `startup_anchor_commit` runs, `heard_ts` for passthrough is
deeply negative: `first_audio_pts - static_latency` (e.g. 64 - 871 =
-807ms). Calling `put_time(-807)` with `no_sched_anchor=1` locks the
sfdec2 scheduler at a phantom reference. The subsequent correct
`put_time` cannot override it because the jump (842ms) is below the
mode-2 hard-discontinuity threshold (1500ms) so `reanchor_disc=0`.

**Fix**: in `stream_sync_audio`, skip `put_time` when
`passthrough_mode && anchor_ts < 0`. `sink_ref_time` remains `-1` so
after `startup_anchor_commit` sets `audio_time = video_time + latency`,
the next `stream_sync_audio` call sees `no_sched_anchor=1` and seeds the
scheduler correctly.

PCM is unaffected: `!passthrough_mode` is always true for PCM so the
guard short-circuits and `put_time` is always called as before.

This fix eliminates a systematic ~46ms pre-convergence audio lead on
EAC3 2.0 passthrough (and any other passthrough format where
`static_latency >> first_audio_pts`).

## Delay Jitter and Stability

- `smoothed_av_delay` is preferred when valid to damp jitter in the audio chain.
- Mode 2 deliberately keeps the core model simple: submitted packet duration plus static latency baseline, with only a conservative positive residual when dynamic delay evidence is stable. It does not currently use a synthetic fill window or a steady-state interpolation layer.
- Internal sync stability does not prove physical lipsync when downstream devices add unreported decode/DSP latency after HDMI/ARC. That class of offset must be handled as route/user delay outside the core scheduler.

# AVOS Sync & Anchoring Rules

## Introduction

- **`venc_put_time` (TS)**: The last TS value passed into the video sink as the anchor point.
- **`venc_ref_time` (WC)**: The monotonic clock sample taken when `venc_put_time` was set. Together, the pair provides the freshness and drift reference used by the current timed-release scheduler.
- **`heard_audio_ts` (TS)**: The audio time that is estimated to be audible at the speakers. Derivation is **centralized** in `stream_get_heard_audio_ts()`.
  - **Steady State**: `heard_audio_ts = audio_time - chain_delay_ts`, where `chain_delay_ts` is `smoothed_av_delay` if valid.
  - **AudioTrack PlaybackParams speed epoch**: for plain PCM hardware speed changes, `heard_audio_ts` is temporarily derived from a playhead checkpoint: `epoch_heard_ts + RST_TO_TS_DELTA(frames_delta * 1000 / rate)`.
  - **Mode 2 passthrough**: `audio_time` advances from submitted compressed packet duration. The raw heard frontier subtracts normalized passthrough latency, and a bounded wall-clock interpolator advances heard time between coarse write batches.
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

The current `sfdec2` implementation is the restored platform-timed release path
(historically `android_sync=1`); there is no runtime mode-0 branch. It always
supplies `render_ts_ns` to MediaCodec. `videosink_put_time()` maintains the
TS/WC reference and forces reanchor on seek epoch, resume edge, speed change,
missing scheduler anchor, or hard discontinuity. During initialization and
reanchor, fresh `venc_put_time` is authoritative; otherwise the sink recomputes
the centralized `stream_get_heard_audio_ts()` value.

## Heard-Audio Anchor Definition

- `heard_audio_ts` is computed centrally via `stream_get_heard_audio_ts()` and is the **single source of truth** for all synchronization and anchoring.
- **Mode 2 passthrough**: For passthrough mode 2 and mode-2 AC3 recoding, `audio_time` is advanced from submitted compressed packet duration (`fakeSize` or codec-specific logical duration). AVOS collects paired accepted bytes and logical samples for at least 250ms, replaces the platform's nominal compressed-buffer component with the observed byte/sample duration, and freezes the normalized latency without an empirical cap. The former codec-aware app/pipeline policy remains only as startup fallback.
- **Mode 2 continuous heard time**: for direct mode 2, `audio_time - selected_delay` is the submitted full-buffer frontier. `mode2_heard_interp_ts` advances from monotonic wall time between batches, snaps forward to new frontiers, and cannot lead the frontier by more than encoded capacity (plus the starvation floor). AC3 recode uses its separate wall-clock burst pacer instead.
- **Mode 2 epoch ownership**: first start seeds at the raw frontier; an explicitly empty replacement track seeds at `audio_time - fixed_latency`; delay changes preserve monotonic phase after the playback epoch is established. Pause preserves phase without crediting paused wall time. Seek and track changes use `mode2_heard_frontier_seed_pending` to carry empty-track ownership across resets.
- **Mode 2 diagnostics**: logs expose packet-duration source and write-timeline geometry (`fakeSize`, `chunk_us`, `dur_bpf`, `dur_rate`). The one-shot `mode2_normalized_latency` record exposes the paired evidence and resulting selected latency without changing the scheduler after the estimate is frozen.

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
- **Mode 1 IEC passthrough**: do not inherit mode-2 policy automatically. Treat
  it as a separate path because its packetization, buffering, and AudioTrack
  reporting differ from codec-specific mode 2.
- **AC3 recoding**: depends on the resolved sink. In mode 1 it keeps the mode-1
  policy. When it resolves to mode 2 (e.g. an eARC route), it adopts the mode-2
  samples clock under `ac3_mode2_plain_policy` (default on) — it must not stay on
  the mode-1 CDATA synthetic anchor, which hides a fixed audio-leads-picture
  offset that only surfaces on real mode-2 hardware. After startup it uses the
  same paired byte/sample normalized latency as every other mode-2 compressed
  format; the prior output-layout selection is only a startup fallback. It still keeps its
  dedicated wall-clock pacer and stays exempt from the ordinary mode-2 lead
  gate, so it is not the *complete* plain-mode2 policy.

The design rule is: use sample sync only when submitted/decoded logical
duration is more trustworthy than per-packet PTS for that path. The scheduler
still uses the same heard-time model:

```
heard_ts = audio_time - selected_delay
```

Measured delay evidence may update `selected_delay`, but it must not become a
second clock or continuously chase the AudioTrack playhead, except for the
explicit AudioTrack PlaybackParams speed-epoch checkpoint described below.

## Mapping Rules

- Only one RST↔TS mapping exists (`timeline_map_apply`). Absolute timestamps are always in TS.
- Heard‑audio anchoring is compatible with both atempo (software) and hardware speed paths.
- For plain PCM AudioTrack PlaybackParams speed changes, arm the playhead
  checkpoint before applying PlaybackParams. The same `anchor_ts` passed to the
  video sink becomes `at_speed_epoch_heard_ts`, and subsequent heard time comes
  from AudioTrack presented-frame deltas converted through `RST_TO_TS_DELTA`.
  This keeps video anchored to the audio actually presented by hardware instead
  of to write bursts or stale delay-cache state.
- The speed epoch is re-armed on every hardware speed change, including return
  to 1.0x, and is cleared on seek, flush, or stop.
- For atempo software speed changes, the authoritative media anchor comes from
  the atempo output ledger. The ledger maps transformed output samples back to
  their media/RST position and lets video/timeline speed commits wait until the
  matching output boundary reaches the AudioTrack playhead.

## Android Path (sfdec2)

- **Current timed-release path**: `codec_sfdec2.c` maintains the TS/WC render
  offset, holds frames outside the 200ms submission lookahead, drops frames more
  than 200ms late, and delegates final presentation to MediaCodec through
  `render_ts_ns`.
  - **passthrough=2 post-seek re-init rule**: synchronization anchors (`sink_ref_time`) are strictly reset on every seek so the next committed compressed write establishes a fresh latency-compensated epoch.
  - **passthrough=2 timing source**: audio TS progression uses compressed-frame `fakeSize` as logical PCM-duration. DTS/DTS-HD follows parser duration first because raw mode 2 writes can be 512-sample DTS frames; otherwise the clock can run 3x too fast. Other formats use codec metadata when available, then logical base units (1536 for EAC3/AC3, 1280 for TrueHD).
  - **passthrough=2 delay source**: after 250ms of paired accepted-byte/logical-sample evidence, all compressed mode-2 formats use normalized buffer capacity plus residual platform latency. Codec-specific app/pipeline selection is retained only during startup or when paired evidence is unavailable. Synchronous AudioTrack playhead/timestamp evidence does not alter this delay.
  - **passthrough=2 renderer source**: `_get_render_heard_ts()` uses a fresh audio-thread `put_time` sample when it is at most 100ms old; otherwise it recomputes the centralized interpolated heard clock. Mode 2 renderer initialization has forward-lead and backward-seek guards but does not maintain a competing audio clock.
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

- **Pause**: direct Mode 2 preserves its heard phase and resets only the wall epoch, so paused duration is not credited. The renderer is reanchored to the preserved centralized heard clock on resume.
- **Seek**: synchronization state is reset (`sink_ref_time = -1`). When playback was already established, the flushed compressed track is explicitly marked empty and the next Mode 2 epoch seeds at the submitted frontier minus fixed route latency.
  - Audio preroll cannot establish the new epoch until video preroll reaches `seek_video_target_ts`. The audio thread waits on epoch-tagged `seek_video_target_pending`; the video thread clears it only when a frame from the current epoch reaches the target. This handshake is independent from audio occupancy or latency estimation.
  - **PCM**: `startup_audio_hold` gates writes; `pcm_reanchor` then sets the anchor from dynamic/last-good/static delay.
  - **EAC3/AC3 passthrough**: `startup_anchor_commit` sets `audio_time = video_time + latency`; pre-commit negative anchors are suppressed (see below).
  - **TrueHD passthrough**: Uses a relaxed 300ms hold threshold to accommodate extremely high packet cadence (1200/sec) and prevent video freezes while filling the HAL pipeline.

## Passthrough Startup Anchor

### `startup_anchor_commit` (mode 1)

On the first mode-1 audio write after seek/resume, passthrough sets
`audio_time = video_time + anchor_delay` and calls
`sfdec2_refresh_sched_anchor()`. This preserves the legacy mode-1 startup
alignment after the timed-release scheduler refactoring. PCM is excluded: it
uses `startup_audio_hold` to achieve
the same alignment by holding writes rather than adjusting `audio_time`.
Plain mode 2 and AC3-recode mode 2 (under `ac3_mode2_plain_policy`) are also
excluded: they run the `STREAM_SYNC_SAMPLES` clock, so no `startup_anchor_commit`
fires (its presence/absence in the log identifies the active policy).

`sfdec2_refresh_sched_anchor()` is required alongside the `audio_time`
change: on seek a `no_sched=1` reanchor fires before the commit,
anchoring the scheduler at the wrong `heard_ts`. Zeroing the anchors
ensures the post-commit `put_time` gets `no_sched_anchor=1` and
reanchors at the correct value regardless of grace period.

### Pre-commit negative anchor guard

Before a valid playback epoch exists, `heard_ts` can be deeply negative because
the selected delay exceeds the first audio PTS. Publishing that value with
`no_sched_anchor=1` would lock the scheduler to a phantom reference that later
burst-smoothing rules might not replace.

**Current rule**: `stream_sync_audio()` publishes `put_time` only when the
centralized anchor is non-negative. `sink_ref_time` remains `-1`, so mode 1 can
publish after `startup_anchor_commit` and direct mode 2 can publish when its
interpolated heard epoch reaches an audible value.

PCM has its own pre-audible startup handling before this publication point;
the non-negative `put_time` rule remains shared.

## Delay Jitter and Stability

- `smoothed_av_delay` is preferred when valid to damp jitter in the audio chain.
- Mode 2 uses submitted packet duration, normalized latency, and a bounded steady-state wall-clock interpolator. The interpolator smooths write batches but does not claim to measure AudioTrack occupancy.
- The synchronous Mode 2 playhead audit is diagnostic and can perturb the writer through JNI. A future occupancy estimator must poll asynchronously, publish epoch-tagged immutable snapshots, and remain shadow-only until validated.
- Internal sync stability does not prove physical lipsync when downstream devices add unreported decode/DSP latency after HDMI/ARC. That class of offset must be handled as route/user delay outside the core scheduler.

# Audio Passthrough Spec

This document describes the passthrough detection and routing logic across Java (`CustomApplication`) and native `avos`, with a state diagram and key decision points.

## Terms

- `IEC61937` / "IEC": manual encapsulation of compressed bitstreams in an IEC container.
- **Passthrough mode 1**: manual IEC encapsulation in `avos` (aka “Nova encapsulation”).
- **Passthrough mode 2**: codec-specific `AudioTrack` encodings (aka “System encapsulation”).
- **Passthrough mode 3**: AC3 recoding pipeline; runtime chooses mode 1 or 2 depending on IEC capability.

## High-Level Flow

1. Java detects audio output devices and their capabilities.
2. Java computes codec flags and IEC capability.
3. Java passes codec flags + passthrough mode to native `avos`.
4. Native selects the actual mode and configures `AudioTrack` accordingly.

## Java: Detection and Capability Model

**Primary entrypoint**

- `CustomApplication.refreshAudioOutputCapabilities(reason)`

**Device scanning**

- `AudioManager.getDevices(GET_DEVICES_OUTPUTS)`
- HDMI types: `TYPE_HDMI`, `TYPE_HDMI_ARC`, `TYPE_HDMI_EARC`
- SPDIF types: `TYPE_LINE_DIGITAL`, `TYPE_AUX_LINE` (Android TV special-case)

**Capability flags**

- `hdmiAudioEncodingFlag`: derived from HDMI device encodings
- `spdifAudioEncodingFlag`: derived from SPDIF device encodings
- `isIecEncapsulationCapable`: route-aware IEC capability:
  - if HDMI/ARC/eARC is present: derived from HDMI flags only
  - else: derived from SPDIF flags (with SPDIF missing-encodings fallback)
- `isDirectPcmMultichannelCapable`: probed via `AudioManager.getDirectPlaybackSupport()` or `isDirectPlaybackSupported()`

**Fallback for SPDIF (Sony/Bravia case)**

If SPDIF device is present but `AudioDeviceInfo.getEncodings()` is empty:

- **If “Force audio passthrough” is enabled**: advertise **all codecs** (same as HDMI bitmask) so passthrough can be forced.
- **If “Force audio passthrough” is disabled**: do not inject codec flags.

IEC capability is only forced to `true` for SPDIF-missing-encodings when **no HDMI route is active**. This avoids leaking SPDIF fallback into HDMI ARC/eARC routing.

**Observed TV behavior**

- Some Sony/Bravia Android TVs expose SPDIF output but do **not** advertise any encodings in `AudioDeviceInfo`.
  - `dumpsys media.audio_policy` may still list AC3/E_AC3/DTS for HDMI ARC, while SPDIF profiles are "dynamic".
  - `dumpsys media.audio_flinger` often shows active output as `SPEAKER|SPDIF` rather than HDMI ARC.
- Other Android TV models do report SPDIF encodings correctly, so the fallback is **not** always required.

**Effective PCM channel limit**

- `CustomApplication.getEffectiveMaxPcmChannels()`
  - IEC capable: use reported `maxAudioChannelCount`
  - IEC not available: allow multichannel PCM if direct playback supports it
  - otherwise default to reported channel count or stereo

## Java → Native Interface

From `PlayerActivity.onStart()`:

- `LibAvos.setMaxPcmChannels(getEffectiveMaxPcmChannels())`
- `LibAvos.setPcmChannelMasks(getHdmiChannelMasks())`
- `LibAvos.setPassthrough(force_audio_passthrough_multiple)`
- `LibAvos.setHdmiSupportedAudioCodecs(getNativeAudioCodecsFlag())`

`getNativeAudioCodecsFlag()`:

- if HDMI/ARC/eARC is present: send **HDMI-only** flags
- otherwise: send SPDIF flags

`force_audio_passthrough_multiple` mapping:

- `0`: no passthrough
- `1`: IEC encapsulation (mode 1)
- `2`: codec-specific encodings (mode 2)
- `3`: AC3 recoding (mode 3)

## Native: Passthrough Selection and Sink Configuration

### Mode selection

`libavos_set_passthrough(mode)`

- mode `0/1/2`: direct passthrough mode
- mode `3`:
  - enables AC3 recoding
  - defaults to mode 1, but can be overridden to mode 2 later if IEC not supported

### IEC capability check

Native determines IEC support by inspecting codec flags set by Java:

- `get_hdmi_supports_iec()` checks flag `ENCODING_IEC61937`
- With current Java routing, native receives route-scoped flags (HDMI-only when HDMI is active), so SPDIF fallback cannot force IEC on ARC/eARC sessions.

### AudioTrack configuration

`audio_interface_audiotrack_java.c`:

- **Mode 2**: uses codec-specific `AudioTrack` encodings (AC3/EAC3/DTS/TrueHD, etc.)
  - unsupported route/codec combinations are rejected before `AudioTrack` setup and fall back to PCM decode unless passthrough is explicitly forced
  - AC3/EAC3/DTS core use stereo channel masks as codec-specific compressed transport
  - E-AC3 JOC uses `ENCODING_E_AC3_JOC` only on API 29+ when advertised; otherwise it falls back to base `ENCODING_E_AC3`
  - DTS-HD MA uses `ENCODING_DTS_HD_MA` only on API 34+ when advertised; otherwise it falls back to generic `ENCODING_DTS_HD`, then DTS core
  - TrueHD uses `ENCODING_DOLBY_TRUEHD` only when the route advertises TrueHD; multichannel masks follow the source channel count, with a stereo retry only if the platform rejects the multichannel `AudioTrack`
  - content sample rate is preserved (e.g., 48kHz)
- **Mode 1**: IEC61937
  - container rates like 192kHz for EAC3/TrueHD/DTS-HD
  - stereo or 8ch depending on format and IEC 8ch support
  - compressed IEC bursts are written atomically; they must not be split into
    PCM-sized chunks

### Passthrough write rules

- Compressed passthrough bursts (direct passthrough and AC3-recoded output) are
  written as whole bursts, not PCM-sized sub-chunks.
- A positive short `AudioTrack.write()` is continued from the remaining byte
  offset until the same compressed unit is complete. No following unit may be
  submitted, and logical duration is published exactly once after completion.
  Pause/play is serialized with this transaction so IEC and raw access units
  cannot be split across a track pause. Destructive aborts flush the incomplete
  stream and restart its timing epoch.
- Passthrough `can_write()` may use exact capacity gating when playback-head
  accounting is usable, but falls back to the previous permissive behavior if
  the exact gate stalls on a given track instance.
- After a passthrough flush, `play()` is deferred until the first successful
  post-flush write. This avoids starting an empty direct/compressed AudioTrack
  while still ensuring the track restarts after seek/resume.
- The permissive fallback is startup-aware:
  - before the passthrough playhead has ever advanced, the stall threshold is
    `250ms` so routes with frozen startup playhead accounting do not starve;
  - after the playhead has advanced, the threshold is `latency + 250ms`,
    clamped to `250..1500ms`, so high-latency full-buffer stalls get time to
    recover before blind writes resume.

### AC3 recoding (Mode 3)

`stream_audio_setup_ac3_sink()`:

- If IEC supported: use mode 1
- If IEC unsupported: switch to mode 2 (e.g. an ARC/eARC route)

**Mode-2 timing policy (`ac3_mode2_plain_policy`, default on).** When AC3 recoding
resolves to mode 2, it must adopt the plain-mode2 timing, not the mode-1 policy it was
historically left on. Two coupled changes apply together (the gate drives both):
1. enter the PTS-seeded `STREAM_SYNC_SAMPLES` audio clock instead of the mode-1 CDATA
   synthetic startup anchor (see Startup Anchoring below), and
2. use the mode-2 latency policy described below.
Under the synthetic anchor the static latency cancels, so the bug was invisible in mode 1
but produced a fixed audio-leads-picture offset on real mode-2 (eARC) sinks; the samples
clock and the latency selection apply together. Mode 1 and ordinary (non-recode) mode 2
are unchanged. See [debug.md](debug.md) for the Shield eARC-emulation A/B workflow.

**Mode-2 normalized latency.** Android's compressed `AudioTrack` latency and local
buffer geometry cannot be interpreted with PCM bytes-per-frame math. AVOS therefore
collects paired accepted compressed bytes and PCM-equivalent logical samples. After at
least 250ms of logical audio, it estimates the duration represented by the configured
compressed buffer:

```
capacity_ms = buf_size * logical_samples * 1000
              / (compressed_bytes * sample_rate * speed)
residual_ms = max(0, track_latency - buf_size * 1000 / sample_rate)
selected_ms = max(residual_ms + capacity_ms,
                  system_latency + capacity_ms)
```

The resulting value becomes the selected mode-2 latency for AC3 recode, direct
AC3/EAC3/JOC, TrueHD, and the DTS family. Until the evidence window completes, or if
valid paired evidence is unavailable, the existing codec-aware app/pipeline selection is
retained as a startup fallback. There is no production ceiling: low-bitrate streams can
legitimately represent more than one second of media in the configured compressed
buffer, and truncating that capacity caused a persistent phase error after track changes.

`audio_spdif.c` mode-2 handling for recoding:

- In mode 2 with parser output: send raw codec frames (`ENCODING_AC3` path) and keep timing via `fakeSize`
- In mode 2 with **no parser** (AC3 recoding path): bypass IEC wrapping and send raw AC3 syncframes directly (`PT_MODE2_NOPARSER` path)
- Mode 1 keeps static passthrough delay. Mode 2 uses the normalized compressed-buffer
  latency as its selected delay after the evidence window.
- The asynchronous presentation observer also samples Mode 1 IEC tracks in shadow
  mode. Completed IEC bursts publish carrier frames at the configured container rate,
  paired with the exact accepted byte frontier. `mode1_iec_occupancy_shadow` compares
  that submitted frontier with an advancing `AudioTimestamp`, but does not yet change
  Mode 1 heard time or video scheduling. Promotion requires device evidence that the
  timestamp advances in the IEC carrier-frame domain and remains stable across start,
  seek, pause/resume, and track recreation.
- `atempo` may still be instantiated for later non-passthrough speed changes,
  but no samples flow through it in passthrough and its delay is not counted in
  passthrough / AC3 recoding sync or speed-anchor calculations.
- Mode 2 uses platform latency as an input to the normalized route baseline. A
  generation-scoped asynchronous observer now compares complete submitted units with
  Android presentation progress. Mode 2 production promotion can be enabled for all
  direct compressed profiles with `mode2_dynamic_all` (currently on for device
  validation); disabling it restores the raw AC3/44.1 kHz allowlist. Mode 1 remains
  shadow-only, and playback-head, byte, and alternate frame interpretations remain
  diagnostic. See [`mode2.md`](../mode2.md).

### Mode-2 heard-time interpolation

Direct mode 2 advances `audio_time` when complete compressed writes are accepted. Those
writes arrive in coarse batches, so `audio_time - selected_delay` is a submitted frontier,
not a continuous presentation clock. `stream_get_heard_audio_ts()` maintains a separate
wall-clock interpolator for direct mode 2:

```
raw_frontier = audio_time - selected_delay
interpolated = previous_interpolated + monotonic_elapsed
```

The interpolated value is monotonic, snaps forward when a new raw frontier overtakes it,
and may lead the full-buffer frontier only by the encoded-capacity portion of the selected
delay (with a small starvation floor). AC3 recode is excluded because its dedicated burst
pacer already provides continuous write timing.

Epoch seeding depends on why the clock changed:

- First playback starts from the raw full-buffer frontier.
- A recreated or flushed mid-playback track starts empty, so
  `mode2_heard_frontier_seed_pending` seeds at `audio_time - fixed_latency`, retaining only
  downstream route delay while the new compressed buffer refills.
- A normalized-latency change preserves monotonic phase once an authoritative playback
  epoch exists; initial normalization before that epoch adopts the new raw phase.
- Pause/resume preserves the interpolated phase and resets its wall epoch so paused time is
  never counted as audio progress. A seek starts a new sync epoch but carries explicit
  empty-track ownership when playback had already been established.

For the validated raw AC3/44.1 kHz profile, the submitted-unit ledger and asynchronous
`AudioTimestamp` observer add a dynamic presentation bound above this fallback. Entry
requires advancing rate and stable occupancy streaks. Heard time holds instead of moving
backward when the measured delay grows, and the MediaCodec render offset converges with a
bounded 5ms-per-frame slew. A non-flushing pause retains ledger occupancy and permits a
750ms observer-remapping grace period; other evidence loss returns gradually to the
static interpolator.

## State Diagram

```
                    +----------------------+
                    |  Detect Devices      |
                    |  (AudioManager)      |
                    +----------+-----------+
                               |
                               v
                    +----------------------+
                    | Compute Capabilities |
                    | HDMI / SPDIF flags   |
                    +----------+-----------+
                               |
                               v
                    +----------------------+
                    |  Active route?       |
                    | HDMI present ?       |
                    +----+-----------+-----+
                         |           |
                 Yes     |           |   No
                         |           |
                         v           v
                +-----------------------+  +-------------------+
                | IEC from HDMI flags   |  | IEC from SPDIF    |
                | (no SPDIF fallback)   |  | (+fallback if none)|
                +------+---------+  +-----------+------------+
                       |                        |
                       v                        v
                 +----------------+      +--------------------+
                 | Native selects|      | Native selects      |
                 | Mode 1 / 2    |      | Mode 2 / PCM        |
                 +----------------+      +--------------------+
```

## Passthrough A/V Startup Anchoring

### `startup_anchor_commit`

On the first write after seek/resume, mode 1 passthrough
sets `audio_time = video_time + anchor_delay` and calls
`sfdec2_refresh_sched_anchor()` in `stream_audio.c`:

```c
if( s->put_time_mode && passthrough_active && s->video_time >= 0 && anchor_delay > 0 ) {
    start_time = s->video_time + anchor_delay;
    sfdec2_refresh_sched_anchor( s );
}
```

This restores the pre-refactoring startup alignment. Without it,
`audio_time` was set to `audio_start_pts` (~24ms), making `heard_ts`
deeply negative and causing the sfdec2 scheduler to stall video for
hundreds of milliseconds before releasing in a burst.

PCM is excluded: it uses `startup_audio_hold` to achieve alignment by
holding writes, not by adjusting `audio_time`.

Plain mode 2 and AC3-recode mode 2 under `ac3_mode2_plain_policy` are excluded. They enter
`STREAM_SYNC_SAMPLES`, count logical samples from the first PTS, and use the Mode-2 epoch
and interpolator rules above. The presence or absence of this commit in the log is the
quickest way to distinguish the mode-1 anchor policy from the mode-2 samples clock.

### Pre-commit negative anchor guard

Before a valid playback epoch exists, the selected delay can exceed the first
audio PTS and make the centralized heard timestamp negative. Publishing that
value as `put_time` with no scheduler anchor would establish a phantom reference.

Current rule: `stream_sync_audio()` publishes `put_time` only when the final
centralized anchor is non-negative. `sink_ref_time` remains `-1` until mode 1
commits its startup anchor or direct mode 2 advances its interpolated heard epoch
to an audible value.

## Manual A/V Delay

- **Positive internal `av_delay` (delay video) is supported on all routes**,
  including passthrough. It is realized physically by the sfdec2 video-hold
  (the effective delay slews into the blit schedule), independent of the audio
  sink. This is the direction normally needed for AVR setups, where compressed
  decode/DSP makes audio late and the player must hold video to match.
- **Negative internal `av_delay` (delay audio) is not supported on compressed
  passthrough.** It works only for decoded PCM, where the delay is realized by
  inserting PCM silence on the audio path. On passthrough AVOS does not own
  decoded samples, and on-device testing showed timestamp/anchor-only schemes
  cannot realize it: the video pacer is anchored to physical audio progression,
  and mode 2's heard clock is a write-derived bounded estimate rather than a
  controllable decoded-audio queue. Shifting timestamps only makes the internal
  clocks agree without physically delaying the audio the receiver hears. A real compressed-audio hold (IEC
  pause/null bursts, codec-specific silent frames, or an AudioTrack pause/gap)
  would be required and carries high AVR-mute / decoder-relock / drift risk.
- **Guarding**: Nova's UI prevents selecting a negative passthrough delay (live
  slider and remembered presets), so native code does not need to clamp it. If a
  future path could bypass the UI, a defensive native clamp of `av_delay < 0` to
  `0` for passthrough routes would be the place to add it.

## Edge Cases

- **SPDIF reported without encodings**: fallback may enable IEC only when HDMI route is absent.
- **ARC/eARC not active**: HDMI caps won’t be seen; SPDIF route may be used instead.
- **PCM decode after passthrough**: sample rate must be re-anchored to avoid A/V drift.
- **Unsupported passthrough formats**: if passthrough is requested but the current route does not advertise the codec, native disables passthrough for that stream and decodes PCM. Mode 2 should not create a codec-specific `AudioTrack` for unsupported TrueHD/DTS-HD/JOC just by changing the channel mask.
- **Forced passthrough**: force mode can expose codecs beyond route-reported support for devices with incomplete capability reporting. In that case `AudioTrack` construction is the final guard; if it rejects a codec-specific configuration, format-specific fallbacks may be attempted before giving up.
- **Codec-specific fallback vs IEC fallback**: stereo in mode 1 describes the IEC transport container. Stereo in mode 2 is only valid for codec families that Android expects as stereo compressed transport, or as a compatibility retry after a route has advertised support but rejected a multichannel codec-specific mask.
- **Mode 2 A/V timing**: timing is based on logical PCM-equivalent duration via `fakeSize`. For DTS/DTS-HD, raw mode 2 writes follow the parser's frame duration because DTS may use 512-sample frames; treating every DTS write as 1536 samples advances the AVOS audio clock too quickly. Other formats use codec metadata when available, then logical base units (1536 for EAC3/AC3, 1280 for TrueHD). Current diagnostics log the selected duration source (`parser`, `avctx`, codec fallback, or physical fallback) and the sink duration geometry (`dur_bpf`, `dur_rate`) used for analysis.
- **Mode 2 sync mode**: mode 2 should use `STREAM_SYNC_SAMPLES`, not
  `STREAM_SYNC_CDATA`, when the compressed packet PTS cadence is less reliable
  than the logical submitted duration. This is a path-specific rule, not a
  general rule for all audio. PCM keeps PTS anchoring plus committed-duration
  advancement; FLAC can use sample sync because decoded sample count is the
  stable clock. Mode 1 IEC passthrough and AC3 recoding must be validated
  independently before inheriting the mode-2 policy.
- **Mode 2 heard-time baseline**: mode 2 uses submitted compressed packet duration to advance `audio_time`, then subtracts the normalized compressed-buffer latency to form the raw heard frontier. The latency estimate freezes after a 250ms paired byte/sample evidence window. Existing codec-aware app/pipeline selection remains only as the startup fallback.
- **Mode 2 continuous clock**: direct mode 2 wall-clock-interpolates between accepted compressed batches, bounded by the raw frontier plus encoded capacity. This is not the former synthetic fill-window experiment and does not measure actual AudioTrack occupancy.
- **Latency terminology**: geometry/app latency is the PCM-style local AudioTrack buffer calculation and is not a reliable duration for compressed bytes. Raw pipeline latency is the platform maximum of AudioTrack-reported track latency and output/system latency plus app geometry. Normalized mode-2 latency replaces the platform's nominal compressed-buffer component with the duration derived from accepted compressed bytes and logical samples.
- **Mode 2 dynamic evidence**: the synchronous playhead/timestamp audit is diagnostic-only. It does not change selected latency, interpolator phase, or scheduler anchors.
- **Physical Route Latency Limit**: AudioTrack latency APIs stop at the Android output boundary. Unreported downstream latency added by a soundbar or AVR after HDMI/ARC still requires a route/user offset outside the scheduler model.

## Debug Tips

- `adb shell dumpsys media.audio_policy` shows available devices and supported formats.
- `adb shell dumpsys media.audio_flinger` shows active output device (SPDIF vs HDMI ARC).
- `at_mode2_audit`: runtime debug parameter for mode2 passthrough. When
  enabled, logs `mode2_playhead_audit` every ~2s with logical samples written
  from `fakeSize`, AudioTrack playhead/timestamp frames, derived
  playhead/timestamp delays, selected delay, pipeline latency, and app
  latency. This invokes JNI from the writer path and has previously perturbed
  timing, so it must remain off during normal playback. It is diagnostic-only:
  it must not update `selected_delay` or reanchor audio/video clocks.
- `mode2_epoch_seed`: records interpolator epoch ownership and the seed cause
  (`initial_raw`, `restart_frontier`, `initial_latency`, `delay_monotonic`,
  `delay_raw`, or `raw_discontinuity`).
- `mode2_heard_interp`: records the raw submitted frontier, interpolated heard
  time, frontier gap, selected delay, reset state, and pause state.
- `mode2_normalized_latency`: a production record emitted when the normalized
  estimate is calculated (normally once per mode-2 AudioTrack configuration),
  showing format, raw track/system/app values, paired evidence, calculated
  capacity, residual, and selected normalized latency.
- Nova logs:
  - `refreshAudioOutputCapabilities(...)`
  - `updateIecEncapsulationCapability`
  - `onStart: PCM diagnostic`

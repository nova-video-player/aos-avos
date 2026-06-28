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
- If `AudioTrack.write()` accepts only part of a compressed burst, Nova drops
  the remainder of that burst instead of retrying the tail as if it were PCM.
  Retrying a tail fragment would corrupt IEC / compressed framing.
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
2. pick the static heard delay by **recode output layout**: a stereo 2.0/192k target uses
   `pipeline_latency`, while a multichannel/640k target uses `app_latency`
   (AudioTrack buffer geometry).
Under the synthetic anchor the static latency cancels, so the bug was invisible in mode 1
but produced a fixed audio-leads-picture offset on real mode-2 (eARC) sinks; the samples
clock and the latency selection apply together. Mode 1 and ordinary (non-recode) mode 2
are unchanged. See [debug.md](debug.md) for the Shield eARC-emulation A/B workflow.

**Why output-aware.** The encoded AC3 payload layouts differ, but Android configures both
compressed AudioTracks with the same two-channel carrier (`ch=2`). A multichannel recode is
in sync on `app_latency`, while a stereo recode using that policy leaves the picture about
553ms ahead of the sound (= `pipeline_latency - app_latency`, 724-171). The discriminator
is therefore the **encoder target channels** published after a successful encoder open,
not the AudioTrack carrier channel count. This is empirical calibration: the recode-mode2
internal sync model does not track physical sync on this path (the on-screen diff swings
~200ms while the audible error moves ~553ms), so the latency is tuned to the acoustic
result. The target layout is reset before encoder initialization, published only after a
fully successful open, and latched into the AudioTrack context at sink configuration.

`audio_spdif.c` mode-2 handling for recoding:

- In mode 2 with parser output: send raw codec frames (`ENCODING_AC3` path) and keep timing via `fakeSize`
- In mode 2 with **no parser** (AC3 recoding path): bypass IEC wrapping and send raw AC3 syncframes directly (`PT_MODE2_NOPARSER` path)
- Mode 1 keeps static passthrough delay. Mode 2 normally uses static
  passthrough latency as its baseline, and may add a bounded positive dynamic
  residual when `enable_dynamic_audio_delay` and `stream_mode2_dynamic_delay`
  are enabled.
- `atempo` may still be instantiated for later non-passthrough speed changes,
  but no samples flow through it in passthrough and its delay is not counted in
  passthrough / AC3 recoding sync or speed-anchor calculations.
- Mode 2 uses the platform-reported static latency (`AudioTrack.getLatency()` /
  `getOutputLatency`) as the route baseline. Any dynamic residual is conservative:
  it is capped, slewed, positive-only, and ignores stream-level last-good state
  as fresh sink evidence.

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

On the first write after seek/resume, passthrough (mode 1 and mode 2)
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

Plain mode 2 (and AC3-recode mode 2 under `ac3_mode2_plain_policy`) is also excluded:
it enters `STREAM_SYNC_SAMPLES` and counts decoded samples from the first PTS, so no
`startup_anchor_commit` fires. The presence/absence of this commit in the log is the
quickest way to tell which timing policy a given playback used.

### Pre-commit negative anchor guard

Before `startup_anchor_commit` fires, `heard_ts` for passthrough equals
`first_audio_pts - static_latency` — e.g. 64 - 871 = -807ms for EAC3
on Streamer 4K. Previously, `stream_sync_audio` would call
`put_time(-807)` with `no_sched_anchor=1`, locking the sfdec2 scheduler
at a phantom reference. The subsequent correct `put_time` could not
override it: the jump (842ms) fell below the mode-2
hard-discontinuity threshold (1500ms) so `reanchor_disc=0`. This caused
a systematic ~46ms pre-convergence audio lead on every startup and seek.

Fix: `stream_sync_audio` skips `put_time` when
`passthrough_mode && anchor_ts < 0`. `sink_ref_time` stays `-1`, so
after `startup_anchor_commit` the next `stream_sync_audio` call has
`no_sched_anchor=1` and seeds the scheduler at the correct value.

Expected residual error after fix: ~16ms pre-convergence (sub-frame at
30fps, imperceptible), and ~15ms post-convergence from
`_snap_timestamp_ns` half-frame rounding — also imperceptible.

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
  and mode 2's heard clock is synthetic (`heard = audio_time - static_latency`),
  so shifting anchors only makes the internal clocks agree without physically
  delaying the audio the receiver hears. A real compressed-audio hold (IEC
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
- **Mode 2 heard-time baseline**: mode 2 currently uses submitted compressed packet duration to advance `audio_time`, then subtracts a selected static latency baseline to estimate heard time. The current tested policy is codec-aware: plain AC3/EAC3 uses pipeline latency, while DTS/DTS-HD, TrueHD, and DDP/JOC use app-buffer geometry latency. This split has been validated by repeated playback testing on Nvidia Shield and Google Streamer 4K, but should still be treated as an empirical policy until a reliable measured-delay path replaces it. The old synthetic fill-window and sawtooth interpolation experiments are not part of the current code path.
- **Latency terminology**: geometry/app latency is the local AudioTrack buffer geometry (`buf_size / frame_size / sample_rate`, speed-adjusted). Pipeline latency is the platform maximum of AudioTrack-reported track latency and output/system latency plus app geometry. The selected static baseline may choose either value depending on codec policy.
- **Mode 2 dynamic residual**: when dynamic delay is enabled, mode 2 can add a small measured residual above the static baseline. The residual is capped by `stream_mode2_dynamic_max_ms`, slewed by `stream_mode2_dynamic_slew_ms`, requires a stability streak, and is positive-only so it cannot pull playback earlier than the static latency baseline.
- **Physical Route Latency Limit**: AudioTrack latency APIs stop at the Android output boundary. Unreported downstream latency added by a soundbar or AVR after HDMI/ARC still requires a route/user offset outside the scheduler model.

## Debug Tips

- `adb shell dumpsys media.audio_policy` shows available devices and supported formats.
- `adb shell dumpsys media.audio_flinger` shows active output device (SPDIF vs HDMI ARC).
- `at_mode2_audit`: runtime debug parameter for mode2 passthrough. When
  enabled, logs `mode2_playhead_audit` every ~2s with logical samples written
  from `fakeSize`, AudioTrack playhead/timestamp frames, derived
  playhead/timestamp delays, selected delay, pipeline latency, and app
  latency. This is diagnostic-only: it must not update `selected_delay` or
  reanchor audio/video clocks.
- Nova logs:
  - `refreshAudioOutputCapabilities(...)`
  - `updateIecEncapsulationCapability`
  - `onStart: PCM diagnostic`

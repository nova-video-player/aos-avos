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
  - output channels often forced to 2 (stereo container)
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
- If IEC unsupported: switch to mode 2

`audio_spdif.c` mode-2 handling for recoding:

- In mode 2 with parser output: send raw codec frames (`ENCODING_AC3` path) and keep timing via `fakeSize`
- In mode 2 with **no parser** (AC3 recoding path): bypass IEC wrapping and send raw AC3 syncframes directly (`PT_MODE2_NOPARSER` path)
- During passthrough / AC3 recoding, dynamic AudioTrack delay is disabled and
  sync uses the static passthrough latency path.
- `atempo` may still be instantiated for later non-passthrough speed changes,
  but no samples flow through it in passthrough and its delay is not counted in
  passthrough / AC3 recoding sync or speed-anchor calculations.
- Mode 2 uses the platform-reported static latency (`AudioTrack.getLatency()` /
  `getOutputLatency`) plus wall-clock interpolation for bursty compressed
  writes. There is no adaptive per-route calibration layer.

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

## Edge Cases

- **SPDIF reported without encodings**: fallback may enable IEC only when HDMI route is absent.
- **ARC/eARC not active**: HDMI caps won’t be seen; SPDIF route may be used instead.
- **PCM decode after passthrough**: sample rate must be re-anchored to avoid A/V drift.
- **Mode 2 A/V timing**: timing is based on logical PCM-equivalent duration via `fakeSize`. `fakeSize` is derived with multi-layered priority: **Parser Duration** > **Context FrameSize** > **Logical Base Units** (1536 for EAC3/AC3, 1280 for TrueHD). This ensures accurate clocking even with high packet cadences.
- **Startup Fill Window**: All Mode 2 passthrough and AC3 recoding benefit from a centralized **Synthetic Fill Window** during startup, ensuring smooth wall-clock paced synchronization while the physical HAL buffer fills. The fill window uses platform static latency, suppresses fill-start clamping until the passthrough playhead has proven it advances, and caps fill-exit rebasing against recent video progress.
- **Steady Mode 2 Timing**: After fill exit, Mode 2 keeps heard time continuous with wall-clock interpolation between compressed write bursts while preserving the static passthrough latency offset reported by the platform.
- **Physical Route Latency Limit**: AudioTrack latency APIs stop at the Android output boundary. Unreported downstream latency added by a soundbar or AVR after HDMI/ARC still requires a route/user offset outside the scheduler model.

## Debug Tips

- `adb shell dumpsys media.audio_policy` shows available devices and supported formats.
- `adb shell dumpsys media.audio_flinger` shows active output device (SPDIF vs HDMI ARC).
- Nova logs:
  - `refreshAudioOutputCapabilities(...)`
  - `updateIecEncapsulationCapability`
  - `onStart: PCM diagnostic`

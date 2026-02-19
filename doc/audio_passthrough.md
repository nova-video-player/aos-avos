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
- `isIecEncapsulationCapable`: true if **HDMI or SPDIF** advertises `ENCODING_IEC61937`
- `isDirectPcmMultichannelCapable`: probed via `AudioManager.getDirectPlaybackSupport()` or `isDirectPlaybackSupported()`

**Fallback for SPDIF (Sony/Bravia case)**

If SPDIF device is present but `AudioDeviceInfo.getEncodings()` is empty:

- **If “Force audio passthrough” is enabled**: advertise **all codecs** (same as HDMI bitmask) so passthrough can be forced.
- **If “Force audio passthrough” is disabled**: do not inject codec flags.

IEC capability is still forced to `true` when SPDIF is present and encodings are missing so that mode 1 can be selected, but codec flags are only broadened when force passthrough is explicitly enabled.

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
- `LibAvos.setHdmiSupportedAudioCodecs(flags)`

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
- This includes **SPDIF** once Java sets `spdifAudioEncodingFlag`

### AudioTrack configuration

`audio_interface_audiotrack_java.c`:

- **Mode 2**: uses codec-specific `AudioTrack` encodings (AC3/EAC3/DTS/TrueHD, etc.)
  - output channels often forced to 2 (stereo container)
  - content sample rate is preserved (e.g., 48kHz)
- **Mode 1**: IEC61937
  - container rates like 192kHz for EAC3/TrueHD/DTS-HD
  - stereo or 8ch depending on format and IEC 8ch support

### AC3 recoding (Mode 3)

`stream_audio_setup_ac3_sink()`:

- If IEC supported: use mode 1
- If IEC unsupported: switch to mode 2

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
                    | HDMI + SPDIF flags   |
                    +----------+-----------+
                               |
                               v
                    +----------------------+
                    |  IEC Capable?        |
                    | (HDMI|SPDIF IEC)     |
                    +----+-----------+-----+
                         |           |
                 Yes     |           |   No
                         |           |
                         v           v
                +----------------+  +------------------------+
                | Mode 1 allowed |  | Mode 1 disabled         |
                | IEC passthrough|  | Use Mode 2 or PCM       |
                +------+---------+  +-----------+------------+
                       |                        |
                       v                        v
                 +----------------+      +--------------------+
                 | Native selects|      | Native selects      |
                 | Mode 1 / 2    |      | Mode 2 / PCM        |
                 +----------------+      +--------------------+
```

## Edge Cases

- **SPDIF reported without encodings**: fallback enables IEC so mode 1 can work.
- **ARC/eARC not active**: HDMI caps won’t be seen; SPDIF route may be used instead.
- **PCM decode after passthrough**: sample rate must be re-anchored to avoid A/V drift.

## Debug Tips

- `adb shell dumpsys media.audio_policy` shows available devices and supported formats.
- `adb shell dumpsys media.audio_flinger` shows active output device (SPDIF vs HDMI ARC).
- Nova logs:
  - `refreshAudioOutputCapabilities(...)`
  - `updateIecEncapsulationCapability`
  - `onStart: PCM diagnostic`

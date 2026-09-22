# Software spatialization integration

Updated September 2026. This supersedes the former v-sofa.diff/m-sofa.diff/a-sofa.diff design.

## Settings and lifecycle

Video exposes one persisted `player_spatialization_mode`: 0 Off, 1 System,
2 TV stereo speakers (experimental), 3 Stereo headphones (SOFA). The legacy
platform checkbox and `player_sofa_mode` migrate into that selection. There
is no implicit fallback from a disabled/unavailable Android spatializer.
Passthrough and AC3 recoding bypass software spatialization.

`SpatializationSettings` owns the shared policy for settings, phone/TV menus,
PlayerService and player preparation. System mode retains multichannel PCM
when the cached Android capabilities report support, availability and enabled
state. As before, the platform capability probes use 48 kHz 5.1/7.1 examples;
these probes are not proof that every stream/output format is spatialized.
SOFA modes preserve decoded channels even when the physical sink accepts only
two, and configure the sink separately as stereo S16 PCM.

MediaLib extracts the selected asset on a worker thread into filesDir/sofa,
using SHA-256 validation, versioned names and AtomicFile. Native preparation
waits for this step. JNI passes the actual path, never an APK asset pseudo-path.
A mutex protects the global desired configuration; the stream snapshots it
before decoder opening and passes that same configuration to the filter. Changes use refreshAudioOutput, which pauses/idles the pipeline,
closes the old filters/decoder and recreates the sink. Changes during prepare
are retained until the player is prepared. Ordinary pause retains DSP history.

Software output explicitly requests Android SPATIALIZATION_BEHAVIOR_NEVER,
including stereo output, to avoid processing it twice. System, software and
Off are mutually exclusive. Existing device-route refresh reopens playback,
which reapplies the selected policy before native preparation.

## DSP and timing

The actual implementation is `Source/stream_filter_audio_mysofa.c`:

```
decode PCM, retain channel layout
  -> selected speed filter (atempo / Sonic), if enabled
  -> existing PCM effects / JNI transformer
  -> float conversion -> FFmpeg sofalizer(type=time) -> float stereo
  -> optional experimental TV crosstalk cancellation
  -> saturated S16 stereo -> sink
```

The wrapper uses the decoded channel mask, with a default layout only when
metadata does not match the frame. It accepts AVOS interleaved U8, S16,
packed S24 and F32 input, 1–8 channels, 8–192 kHz. It owns its input AVFrame
and returned output storage; failures clear the output and stop the audio
path instead of silently emitting dry or misformatted PCM.

A small `sofalizer.patch` adds a `sample_rate` option that resamples the HRTF
once via libmysofa **before** lookup/filter construction. Programme audio is
not resampled. Time-domain convolution returns exactly N output frames for
N input frames at the same rate; the wrapper verifies this invariant. There
is no wrapper FIFO or sample realignment. FFmpeg's convolution history contains
acoustic response, not unreturned input frames. Like upstream sofalizer, output
is cropped to programme duration: no extra response tail is appended at EOF.
Seeking recreates the graph and the TV FIR history; format/layout changes also
recreate that history. Ordinary pause retains it.

### Signal delay and speed

Frame preservation does not mean zero signal delay. `delay()` remains zero
because no programme frames are queued; the separate `signal_delay_us()` reports
a common acoustic timing reference for the hash-pinned bundled models:

- TV: 256 samples of inverse design delay plus 25 samples of KU100 reference,
  approximately **5.854 ms** at the 48 kHz design rate.
- Headphones: SADIE D1 front-center energy reference of 107 samples,
  approximately **2.229 ms**. Its FIR length/2 is not the correct reference.

These are common references, not exact delays for every direction/frequency.
Per-ear arrival differences and the original HRIR phase remain intact. Measured
front-center energy timing is within 0.1 ms at the tested 8–192 kHz rates
after the time-domain ring-wrap fix below (regression tolerance 0.25 ms).
The TV coefficients are resampled once when the input rate changes, with their
integral corrected. Programme audio is not resampled.

The audio thread publishes the signal reference as an atomic stream snapshot;
the renderer never dereferences the filter context. Normal delay/anchor paths
include it, while the hardware-delay cache excludes it. For atempo/Sonic, the
presentation ledger subtracts the reference in **output frames before TS/RST
lookup**, including when it crosses a speed boundary. Audible speed commit
checks use that same corrected frame index. Mixer-to-DAC latency remains a
separate correction; direct DAC timestamps also need the SOFA signal correction.
AudioTrack PlaybackParams scales the reference by playback speed because it
speeds the spatialized PCM too. SOFA-off streams publish zero.

This permits SOFA after atempo/Sonic: output-frame indices and partial-write
media lookups retain their meaning. SOFA never changes parser/video speed
epochs. A 1.0x restriction is unnecessary for accounting; CPU and perceived
quality at different speeds still need device evaluation. Sink reconfiguration,
samples-based clocks and manual-delay silence use stereo output geometry while
decoder/source metadata remains multichannel.

## The two playback modes

Headphones use **SADIE II v2-2 D1 / Neumann KU100**, the publisher's 48 kHz,
256-tap, low-frequency-extended, diffuse-field-equalized file. It replaces
CIPIC subject 003. FFmpeg uses its standard virtual speaker layout with HRTF
interpolation enabled. The `normalize=1` option is level normalization, not
headphone equalization. No extra diffuse-field EQ, independent ear alignment,
minimum-phase conversion, personalized correction or head tracking is applied.

TV retains **TH Koeln KU100** for binaural rendering, followed by a generated
four-path, regularized speaker-to-ear inverse. The old fixed 0.35 cross-feed
feedback approximation is removed. Current assumptions are **1 m speaker
spacing, 2 m perpendicular listening distance, centered listener, speakers at
ear height** (about ±14.04 degrees). The user specified distance and centering;
spacing and height are engineering defaults pending physical measurements.

`test/design-sofa-tv.py` uses all four interpolated KU100 paths, relative path
propagation at 343 m/s and distance attenuation. It fits the center and ±5 cm
lateral offsets together. It uses Tikhonov regularization (0.03 times the largest
normal-matrix eigenvalue), a matrix gain limit of 2, and partial cancellation:
150–300 Hz fade-in, full weighting at 300–3000 Hz, 3000–6000 Hz fade-out to a
delayed dry path. The 513-tap, 48 kHz FIR uses a Kaiser window and 256-sample
common delay, followed by 6 dB output headroom. The finite filter is checked
again for maximum matrix gain; the current design measures 0.9323. This bounds
frequency-domain energy gain, not arbitrary waveform peaks; output conversion
still saturates to S16.

The method follows the inverse-filtering/robustness literature discussed in
[sofa_model_selection.md](sofa_model_selection.md). These bandwidth, headroom,
regularization and geometry values are **Nova tuning choices**, not a claim
that a paper prescribes one universally correct TV configuration.
[sofa_tv_filter.json](sofa_tv_filter.json) records the reproducible parameters.
The model predicts separation over 300–1500 Hz of about 19.5 dB at center and
11.3–11.6 dB at ±5 cm, versus 2.7 dB without the inverse. These are predictions
on the design model, not measured room or listening-test results.

TV mode remains **experimental**. Neither the model nor its inverse measures
the actual TV drivers, furniture, reflections, listener anatomy or head motion.
The far-field HRTFs plus relative propagation approximate the stated geometry;
they do not become measured 2 m room responses. A standing listener whose ears
are above the speakers needs a different elevation model. The scalar C loop
also adds appreciable convolution work (four 513-tap paths at 48 kHz, longer
at higher rates); low-end TV CPU testing remains necessary.

## Alternative renderers and performance options

Assessment: 22 September 2026. These are alternatives and proposed optimizations,
not additional integrated backends. No comparative CPU, power or listening
benchmark on Nova's target devices has established a faster or better renderer.

### Separate SOFA preparation from audio rendering

Libmysofa reads SOFA measurements, resamples them and retrieves/interpolates
HRTFs. The caller performs the actual audio convolution; see the
[libmysofa API documentation](https://github.com/hoene/libmysofa).
In Nova, libmysofa prepares HRTFs when the FFmpeg graph is created or recreated,
FFmpeg `sofalizer` continuously convolves the programme audio, and the TV mode
adds our four-path crosstalk-cancellation FIR. Replacing the SOFA reader alone
would mainly change preparation time and memory use, leaving these convolution
costs in place.

### Available alternatives

| Option | Capabilities and ecosystem | Assessment for Nova |
| --- | --- | --- |
| **FFmpeg + libmysofa (current)** | SOFA-based binaural rendering with time- and frequency-domain processing. [Documentation](https://ffmpeg.org/ffmpeg-filters.html#sofalizer) | Good fit for the existing FFmpeg pipeline. Optimize and benchmark this baseline first. |
| **Steam Audio** | Dedicated virtual-surround effect for stereo/5.1/7.1; custom SOFA HRTFs; C API and game/middleware integrations. Supports Android ARMv7/ARM64 and other platforms; Apache 2.0. [Virtual surround](https://valvesoftware.github.io/steam-audio/doc/capi/virtual-surround-effect.html), [HRTF support](https://partner.steamgames.com/doc/features/steam_audio), [project/platforms](https://github.com/ValveSoftware/steam-audio) | First replacement renderer to benchmark for headphone mode. Its C API can process PCM without adopting a game engine. A new adapter must handle buffering, timing, seek/reset and tails. No demonstrated speed advantage over our implementation yet. |
| **OpenAL Soft** | Full positional-audio engine with HRTF rendering. Its default HRTF dataset derives from SADIE II; engine licence is LGPL 2 or later. [Project](https://github.com/kcat/openal-soft) | More appropriate for a broader audio-engine redesign than a small filter replacement. Requires adaptation to AVOS's existing sink and timing ownership. |
| **Resonance Audio** | Ambisonic binaural rendering with selectable quality/CPU modes and game/middleware integrations. Apache 2.0. [Project](https://github.com/resonance-audio/resonance-audio), [rendering modes](https://github.com/resonance-audio/resonance-audio/blob/master/resonance_audio/api/resonance_audio_api.h) | Interesting for many moving sources and head tracking, but the official repository was archived in November 2023. Not recommended for a new Nova integration. |
| **Spatial Audio Framework / SPARTA** | HRTF processing, binaural/Ambisonic algorithms and DSP building blocks. Core modules are ISC; some optional modules are GPLv2. Requires numerical libraries and can itself use libmysofa. [Project](https://github.com/leomccormack/Spatial_Audio_Framework) | Useful for advanced DSP development, with more integration and dependency work than a ready-made virtual-surround effect. |
| **Android system spatializer (already integrated)** | Device-provided spatialization and, where supported, head tracking. Availability depends on device, output route and user settings. [Android documentation](https://developer.android.com/media/grow/spatial-audio) | Retain the existing System mode on supported routes. Do not assume lower total CPU/power on every device; measure it. Keep it mutually exclusive with software spatialization. |

A headphone binaural renderer produces ear signals. TV speakers still require
a separate speaker-to-ear cancellation stage. Adopting Steam Audio or another
headphone renderer would not, by itself, solve TV geometry, room reflections
or listener movement. Renderer choice also does not establish which HRTF best
matches an individual listener.

### Recommended optimization order

1. **Profile on target ARM devices.** Measure graph creation and seek time,
   peak memory, sustained CPU and underruns separately for headphones and TV.
   Compare stereo, 5.1 and 7.1 at representative rates and playback speeds,
   keeping HRTFs, gains and output conditions comparable where possible.
2. **Optimize the TV FIR.** At the 48 kHz design rate, four 513-tap paths imply
   `4 * 513 * 48000 = 98,496,000` coefficient multiplications per second before
   binaural rendering. This is an operation count, not a CPU measurement.
   Our TV loop is plain C; the vendored FFmpeg time-domain renderer already
   uses its optimized float dot-product infrastructure. Benchmark vectorized
   direct convolution against block/partitioned FFT convolution.
3. **Evaluate FFmpeg frequency-domain rendering.** The vendored `sofalizer`
   consumes fixed blocks in frequency mode, with a minimum `framesize` of
   1024 samples (21.33 ms of audio at 48 kHz). That is the block duration,
   not a universal end-to-end latency. Merely changing `type=time` to
   `type=freq` breaks our current N-in/N-out per-call contract: an adapter
   must account for queued output, partial writes, EOF, seek and speed
   transitions, separately from the acoustic reference delay.
4. **Prepare compact filter banks offline.** Fixed virtual speaker directions
   without head tracking need only a subset of the full SOFA sphere. Exporting
   the required HRIRs and their delays can reduce asset size, preparation work
   and runtime memory, while retaining libmysofa as an offline tool. Preserve
   supported layouts/rates, interpolation results, gain, ITDs, timing references,
   provenance and licences. This primarily optimizes preparation and storage;
   unchanged FIRs still require the same continuous convolution work.
5. **Benchmark Steam Audio if needed.** Compare equivalent headphone rendering
   after establishing the optimized FFmpeg baseline. Migrate only for measured
   gains or required features, with the same frame-accounting and sync checks.

The current recommendation is to retain libmysofa and optimize convolution and
profile preparation. The existing integration prioritizes explicit timing and
frame preservation; it has not been established as the most efficient design.

## Assets and builds

Canonical app assets: `Video/assets/sofa/`. Existing identical MediaLib asset
copies are redundant; app assets take precedence during merging. Keep the
hashes in SofaProfileManager in sync when replacing models. Both bundled models
are **48 kHz** SimpleFreeFieldHRIR datasets. SADIE's file
is about 36.3 MB uncompressed. The old CIPIC asset is no longer packaged.
`test/install-sadie-profile.py ARCHIVE LICENSE` verifies the official SADIE v2-2
archive and license before installing identical assets in both modules. The
profile manager hash changes the extracted filename, avoiding reuse of the old
profile. See bundled `NOTICE.txt` and `LICENSE-SADIE.txt`: SADIE is Apache 2.0;
KU100 and the derived TV coefficients are CC BY-SA 3.0.

The full Android FFmpeg build already enables libmysofa. Rebuild FFmpeg as
well as AVOS: the new wrapper requires the sofalizer sample_rate option.
`native/ffmpeg-android-builder/sofalizer.patch` is applied by build.sh;
bootstrap/build cache checks invalidate prebuilts lacking that patch stamp.
LibAvos loads libmysofa before libavfilter. The AVP packaging rules already
copy libmysofa for each selected ABI.

## Verification

### Garbled audio at 44.1 kHz: time-domain ring-wrap fix

`ChID-BLITS-EBU.mp4` contains 44.1 kHz, six-channel HE-AAC. Resampling the
48 kHz HRIRs produces lengths that need not be powers of two. The vendored
FFmpeg `sofalizer` time-domain convolution split a wrapped input window using
the padded IR length and `read % ir_samples`, instead of the actual ring
boundary. This skipped/replaced samples for some ring positions and made the
filter time-varying, producing a plausible cause of metallic/garbled audio in
both profiles. This was a DSP bug, not an intended HRTF coloration.

The fix copies exactly `ir_samples`, splitting only at `buffer_length - read`,
and leaves SIMD padding zero. It is included both in the vendored source and
`native/ffmpeg-android-builder/sofalizer.patch`. Android prebuilts now carry
`.nova-sofalizer.sha256`; a missing or changed patch stamp forces a rebuild.
Updating AVOS alone while keeping the old `libavfilter.so` does not apply the fix.

The regression sends two identical, isolated impulses at different ring
positions and compares their stereo responses. Before the fix, the TV 44.1 kHz
case differed by up to 6964 S16 units; after it, both profiles at 44.1/48 kHz
match exactly. Previous block-partition/frame-count tests did not detect this:
the same corrupted signal was produced regardless of API block boundaries.
The earlier 44.1/8 kHz timing deviations were also caused by this bug; the
previous explanation based on band limiting was insufficient.

As a separate check, the first 35 seconds of the clip were decoded to 44.1 kHz
5.1 S16 PCM and passed through the production wrapper. Its output was compared
against a test-only wrapper using FFmpeg's frequency-domain convolution with
1024-sample input blocks (last block padded, output cropped to input duration).
Both profiles differed by at most one S16 unit across 3,087,000 channel samples,
with no clipped output samples. This validates the host DSP path; confirming
the reported listening problem is resolved still requires Android playback.

Diagnostics: `mysofa:` reports input rate, bit depth, channel mask/resolved
layout and TV tap count when configuring the graph. `mysofa_pcm:` reports
output peak, clipped samples and non-finite convolution samples on the first
block and every five seconds of PCM. These bounded logs help distinguish
remaining clipping/format problems without per-frame log traffic.

### Host regression commands

`python3 native/avos/test/run-mysofa-filter.py` builds local libmysofa and a
minimal vendored FFmpeg without downloads, then exercises both actual assets
and production wrappers. Existing host builds can be supplied with
`--ffmpeg-build DIR --mysofa-prefix DIR`.

Regenerate the TV coefficients with NumPy and a host libmysofa:
`python3 native/avos/test/design-sofa-tv.py --libmysofa /path/to/libmysofa.dylib`
(or the corresponding `.so`). Optional `--spacing`, `--distance` and `--height`
parameters change the assumed geometry; regenerated coefficients require a
native rebuild and evaluation. This is an offline design tool, not an app setting.

`python3 native/avos/test/run-sofa-sync.py` exercises production presentation
helpers for signal-delay correction across speed boundaries, mixer/DAC sources,
startup, PlaybackParams scaling and SOFA-off behavior. The existing
`run-speed-transitions.py` and `run-sonic-filter.py` remain regression checks.

Coverage: mono/stereo/5.1 input, 8/44.1/48/96/192 kHz timing references,
44.1/48 kHz format changes, exact frame counts,
1-sample through 1024-sample partition invariance, seek reset, EOF accounting,
left-ear directionality, malformed input/missing profiles, and actual atempo
and Sonic transitions through 1x/0.5x/1.5x/2x/1x with partial-write media lookups.
The existing Sonic/shared-ledger regressions remain applicable.

Still requires device testing: TV/headphone acoustics and loudness, CPU on
low-end TVs, AudioTrack PlaybackParams, rapid mode/track switches, pause/seek,
HDMI/headset route changes, and coexistence with system spatialization.

Local validation for this integration: ARMv7 and ARM64 AVOS/JNI and patched
FFmpeg libraries built successfully; the resulting libraries were copied to
MediaLib/libs for those ABIs. Real-profile and speed-composition tests passed,
also with UndefinedBehaviorSanitizer on the wrappers. Existing Sonic and
presentation-ledger tests passed. Changed Java sources compiled against the
SDK/cached dependencies with temporary resource symbols; aapt2 compiled the
changed XML. A full Gradle build could not start because the environment
blocked access to Gradle's cache lock even after an escalation attempt.
An AddressSanitizer run timed out before returning results; it is not counted
as a pass. No acoustic/device playback validation has been performed.

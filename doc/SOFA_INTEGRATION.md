# SOFA HRTF Integration Guide

## Overview
This directory contains SOFA (Spatially Oriented Format for Acoustics) HRTF profile files used by the Nova Video Player spatial audio filter (`stream_filter_audio_mysofa.c`).

## Files

### HRIR_FULL2DEG_TV_speakers.sofa
- **Source**: Neumann KU100 (professional dummy head)
- **Use Case**: TV speaker stereo pair simulation (front-facing)
- **Resolution**: 2-degree full sphere
- **Sample Rate**: 96 kHz
- **Description**: HRTF data measured in far-field conditions suitable for simulating stereo speaker pairs at standard listening distances.

### subject_003_headphones.sofa
- **Source**: CIPIC Database Subject 003
- **Use Case**: Ear-covering headphone binaural simulation
- **Sample Rate**: 44.1 kHz / 48 kHz (downsampled as needed)
- **Description**: HRTF data optimized for headphone listening with natural ear coupling simulation.

## Integration in Nova

### Native Configuration
The FFmpeg `sofalizer` filter is applied in `stream_filter_audio_mysofa.c`:

**TV Mode (Mode 1)**:
```c
avfilter_init_str(ctx->filter_ctx,
    "sofalizer=sofa=" SOFA_PATH "/HRIR_FULL2DEG_TV_speakers.sofa:type=freq");
```

**Headphones Mode (Mode 2)**:
```c
avfilter_init_str(ctx->filter_ctx,
    "sofalizer=sofa=" SOFA_PATH "/subject_003_headphones.sofa:type=freq");
```

### Java/Android Side
Control via `PlayerService.java`:
```java
// Toggle through modes
playerService.toggleSofaMode();  // OFF → TV → Headphones → OFF

// Set specific mode
playerService.setSofaMode(1);    // TV speakers
playerService.setSofaMode(2);    // Headphones
```

## Audio Processing Pipeline

1. **Input**: Multi-channel or stereo audio from decoder
2. **Downmix**: Convert to stereo if needed
3. **SOFA Filter**: Apply selected HRTF profile via `stream_filter_audio_mysofa.c`
4. **Output**: Spatially enhanced stereo audio

## Supported Modes

| Mode | Setting | Profile | Use Case |
|------|---------|---------|----------|
| 0 | OFF | None (passthrough) | No spatial processing |
| 1 | TV | HRIR_FULL2DEG_TV_speakers.sofa | Speaker-based playback |
| 2 | Headphones | subject_003_headphones.sofa | Headphone playback |

## Technical Details

- **SOFA Standard**: AES69-2022
- **FFmpeg Filter**: sofalizer (requires libmysofa support)
- **Processing**: Frequency-domain HRTF convolution
- **Latency**: ~10-20ms depending on FFT size
- **CPU Impact**: ~5-10% per core (moderate)

## References

- SOFA Convention: https://www.sofaconventions.org/
- CIPIC Database: https://www.ece.ucdavis.edu/cipic/
- FFmpeg sofalizer: https://ffmpeg.org/ffmpeg-filters.html#sofalizer
- libmysofa: https://github.com/hoene/libmysofa

## Building with SOFA Support

Ensure FFmpeg is built with libmysofa support:

```bash
cd native/ffmpeg-android-builder
# Configure with: --enable-libmysofa
bash bootstrap_avp_ffmpeg.sh
```

## Troubleshooting

### SOFA files not found
- Verify files are in `native/avos/assets/sofa/`
- Check file permissions: `chmod 644 *.sofa`
- Verify SOFA paths in `stream_filter_audio_mysofa.c`

### Filter initialization fails
- Ensure FFmpeg is compiled with `libmysofa` support
- Check SOFA file format compatibility (AES69-2022)
- Verify audio channel count (sofalizer requires 2 channels for stereo mode)

### Audio distortion or artifacts
- Check filter latency compensation in `stream_audio.c`
- Verify SOFA file sample rate compatibility
- Reduce FFT size if CPU is saturated

## License

These SOFA files are provided by their respective sources:
- **HRIR_FULL2DEG_TV_speakers.sofa**: SOFA Conventions (public domain)
- **subject_003_headphones.sofa**: CIPIC Database (free for research use)

For production use, verify license compliance with your distribution model.

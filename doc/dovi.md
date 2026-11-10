# Dolby Vision GPU reshaping

Profile 5 played without a Dolby Vision decoder. The reshaping itself is the
standard single-layer process: per-component polynomial and MMR curves from the
RPU, YCC to RGB, LMS to BT.2020, PQ. The LMS to BT.2020 RGB matrix weights
are taken from libplacebo (`dovi_lms2rgb` in `Source/video_dovi_reshape.c`).
User-facing behavior is described in `Video/doc/DOVI.md`.

## Design

- **Inside the decoder library.** `libsfdec` renders the HEVC decoder output into
  an `AImageReader`, and its own GL thread presents to the app window
  (`external/android/libsfdec/sfdec_dovi_gl.cpp`). Playback, A/V sync and the
  player's frame scheduling are unchanged; no app-side surface is involved.
- **RPU next to the input.** `codec_sfdec2.c` parses each access unit's RPU
  before queueing it and hands the parameters to `libsfdec` with the codec
  timestamp. Output frames are matched by that timestamp, and a resent access
  unit reuses its slot, so a frame never uses another frame's parameters.
- **Presentation-time pacing.** Frames are acquired as soon as the decoder
  releases them, held, and drawn shortly before their presentation time with
  `eglPresentationTimeANDROID`. Frames arrive well ahead of time, and queueing
  them early to the window blocks the swap.
- **Raw decoder samples.** `GL_EXT_YUV_target` gives the 10-bit codes directly.
  Without it, a calibrated inverse of the driver's YUV to RGB conversion
  recovers the same codes (NVIDIA Tegra).
- **Output follows the display.** BT.2020 PQ on HDR10 displays. On SDR displays,
  or without a BT.2020 PQ window surface: luminance tone mapping up to the RPU's
  source peak, 203-nit reference white, BT.709, gamma 2.2.
- **Fails safe.** If the GL path cannot start, or the decoder rejects the image
  reader, the hardware decoder renders directly as before.

## Validation

- `test/test_dovi_reshape.py`: per-frame parameters against an independent RPU
  parser.
- Device statistics (`sfdec_dovi_gl: frames= stale= unmatched= dropped= late=`)
  every 240 frames; stale, unmatched and dropped stay at 0 during playback and
  seeks.

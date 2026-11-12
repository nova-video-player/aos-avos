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

## Heuristics

Choices made from the stream, the device or measurements rather than from a
specification. Each one logs its outcome, so a capture shows which applied.

- **Activation** (`codec_sfdec2.c`): HEVC Profile 5 on the hardware HEVC
  decoder, that is no DV decoder or DV mode Off, built with libdovi. The GL
  path needs API 26 (`AImageReader` private buffers, `AHardwareBuffer`) and the
  EGL extensions `EGL_ANDROID_get_native_client_buffer`,
  `EGL_ANDROID_image_native_buffer` and `EGL_ANDROID_presentation_time`.
  Anything missing, or `AMediaCodec_configure` rejecting the reader's window
  (MediaTek OMX), falls back to direct rendering. Log: `GPU reshaping: active
  ... output=PQ|SDR|none`.
- **PQ or SDR output.** SDR when the display reports no HDR10
  (`acodecs_get_display_hdr_capabilities()`; unknown keeps PQ), or when EGL
  lacks `EGL_KHR_gl_colorspace` and `EGL_EXT_gl_colorspace_bt2020_pq`. Android
  hides that color space when SurfaceFlinger runs without wide color, even on
  an HDR10 display (Amlogic s7d). Log: `no BT.2020 PQ color space`.
- **Window surface format**, checked against the exact red and alpha sizes:
  - PQ: RGBA1010102, then RGBA16F (NVIDIA Tegra has no 10-bit config), then
    RGBA8888 (banding, correct colors);
  - SDR: RGBA8888, then RGBA16F, then RGBA1010102. Hardware composers scan out
    RGBA8888, while an RGBA16F layer was composited on the GPU every frame on
    the Amlogic Mali-G310 and half the 1080p60 frames came late;
  - SDR on NVIDIA Tegra (`ro.board.platform` or `ro.hardware.egl` contains
    `tegra`; `EGL_VENDOR` is "Android" behind the platform loader): RGBA16F
    first, as the RGBA8888 surface showed nothing there.

  Log: `ready on <GPU>: <format> <PQ|SDR> output`.
- **Sampling.** `GL_EXT_YUV_target` gives the raw 10-bit codes. Without it the
  driver's conversion is inverted assuming BT.601 full range, the conversion
  drivers apply to Profile 5 (full range, no matrix coefficients), measured
  exact on Tegra X1. Log: `raw YUV` or `RGB (BT.601 full range inverted)`.
- **Frames held.** Frames are released up to 200 ms ahead and held until 30 ms
  before their presentation time. Held count is `ceil(200 ms / frame
  duration) + 1`, clamped to 4..14 (6 at 25 fps, 13 at 60 fps; 14 when the rate
  is unknown), so 10-bit 4K frames (about 25 MB each) stay bounded. The reader
  has two more buffers, for the frame on screen and the one being drawn. When
  all are held, the oldest is presented early (`held frames are not enough`).
- **RPU to frame matching.** Parameters are queued by codec timestamp. Queue
  entries older than a matched frame are discarded as frames the codec never
  delivered. A frame without a match reuses the last parameters
  (`unmatched=`); before any parameters it is dropped (`dropped=`, `no RPU
  parameters yet`). A flush (seek) drops held frames and the last parameters.
- **RPU history** (`video_dovi_reshape_update`). Same as FFmpeg: mappings are
  kept per `vdr_rpu_id` (16 slots), `use_prev_vdr_rpu_flag` takes
  `prev_vdr_rpu_id`, else id 0, and DM data persists when
  `vdr_dm_metadata_present_flag` is 0. It is reset on open and on flush.
- **HDR metadata** (PQ output only, when `EGL_EXT_surface_SMPTE2086_metadata`
  or `EGL_EXT_surface_CTA861_3_metadata` is present):
  - mastering luminance and MaxCLL/MaxFALL from L6;
  - without L6, the maximum is the RPU's source peak (`source_max_pq`);
  - primaries are P3-D65, Dolby's default without an L9 block;
  - set again only when the values change.

  Log: `HDR metadata:`.
- **SDR tone mapping.** Luminance up to the RPU source peak (1000 nits, PQ
  0.7518, when absent), 203-nit reference white (BT.2408) mapped to 1.0,
  BT.709 primaries, gamma 2.2.

## Validation

- `test/test_dovi_reshape.py`: per-frame parameters against an independent RPU
  parser.
- Device statistics (`sfdec_dovi_gl: frames= stale= unmatched= dropped= late=`)
  every 240 frames; stale, unmatched and dropped stay at 0 during playback and
  seeks.

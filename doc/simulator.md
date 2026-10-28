# Native (sim) build

The sim build compiles the real AVOS sources into a host binary (`build/avos`). It is
the fastest way to build, run and debug the demuxer, decoders, audio pipeline and
subtitle engine on Linux or macOS without an Android device or NDK.

`avos_config.mk` selects it by default on non-ARM hosts: `GUI=OFF`, `CLI=ON`,
`VIDEO_FFMPEG=ON`, `AUDIO_INTERFACE=SDL`, `FRAMEBUFFER=QVFB`, `AUDIO_COMPRESS=ON`.

## Prerequisites

The sim links FFmpeg, libass, FreeType, SDL2 and libzip from the host.

Linux (Debian/Ubuntu):

```bash
sudo apt-get install -y --no-install-recommends \
  build-essential pkg-config \
  libass-dev libfreetype-dev libsdl2-dev libzip-dev zlib1g-dev \
  libavcodec-dev libavformat-dev libavutil-dev \
  libswscale-dev libswresample-dev libavfilter-dev
```

macOS:

```bash
brew install libass freetype sdl2 libzip ffmpeg pkgconf
```

The audio compressor is built from a sibling checkout, which `make` builds on demand:

```bash
git clone https://github.com/nova-video-player/aos-audiocompress.git ../audiocompress
```

## Build

```bash
make -j"$(getconf _NPROCESSORS_ONLN)"
./build/avos
```

`build/avos` is the player; `build/avsh` is the host build of the debug socket client
(see `doc/debug.md`).

Useful overrides:

```bash
make clean
make ASAN=1                 # AddressSanitizer for the sim binary
make TOOLCHAIN_PATH=/prefix # non-default include/lib prefix
```

Homebrew is detected with `brew --prefix`; `/opt/homebrew` is no longer assumed, so
Apple Silicon and Intel prefixes both work. libass and freetype2 flags and libraries
come from `pkg-config`, with plain `-lass`/`-lfreetype` only as a fallback.

## What is different from Android

- `Include/host_compat/` supplies header-only stand-ins for the NDK, JNI, EGL and
  GLES2 headers the subtitle engine includes. The directory is added to the include
  path for the sim build and the host tests only; Android builds never see it.
- `Source/fb_dummy.c` implements the framebuffer. It has no window: the OSD buffer is
  allocated from `osd_props` in `DUMMY_connect()`, and the GL render path is a no-op.
- Audio goes through SDL2 (ALSA/OSS are not used on macOS).
- Video decoding is FFmpeg software decoding only; MediaCodec is Android-only.

## Subtitles in the sim

The sim draws subtitles itself: `Source/stream_sink_video_sim2.c` pulls the engine's CPU-blended
frame (the path Android uses in 3D mode) and composites it over the video in the SDL window.
Text (SRT/VTT/ASS) and bitmap (PGS, VobSub idx/sub) tracks all go through it.

Debug commands, typed in `./build/avsh` (they also appear in `help`):

| Command    | What it does |
|------------|--------------|
| `vsl`      | list the subtitle tracks: index, int/ext, name, format, source file |
| `vss <n>`  | select track `n`; `vss -1` turns subtitles off; no argument cycles |
| `subm`     | toggle "use margins": extend the subtitle canvas into the top/bottom letterbox bars |
| `dbgsub n` | subtitle logging: 1 = track list and cue feeding, 2 = also idx cue times and packet headers |

## CI and tests

`.github/workflows/avos-sim-build.yml` builds this target on `ubuntu-latest` and
`macos-latest` for every push and pull request.

The native subtitle engine tests in `test/host/` are separate from the sim binary:
they compile selected engine sources plus stubs and run on the host. See `doc/TEST.md`.

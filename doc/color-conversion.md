# FFmpeg pixel conversion

AVOS uses libswscale for standard pixel conversions. `codec_utils.c` owns the
adapter; libyuv, `neon_rgb.S`, `neon_yuv.S`, and their callers have been removed.
The existing portable UI copy/rotation implementations replace the helpers that
also lived in those assembly files. NEON memory helpers and the separate
RenderX deinterlacer remain.

## Buffer and format contract

- FFmpeg decoder frames retain their exact `AVPixelFormat`, including planar vs
  packed layout, full-range YUVJ formats, and 10-bit endianness.
- Source strides are bytes and may be negative. Source memory must remain valid
  through the synchronous conversion and contain every visible row. Invalid
  pointers/lifetimes cannot be detected from a stride alone.
- AVOS RGB destination strides remain pixels. YUV strides are bytes. Destination
  plane extents are checked before conversion. Packed 4:2:2 requires storage for
  the final complete pair even at odd widths.
- AVOS YV12 pointers are Y/V/U; FFmpeg uses Y/U/V. OMX shadow frames are normalized
  to this convention. OMX's legacy AV_IMAGE_YUV_422 label denotes planar 422 and
  is handled explicitly by its sink callers.
- The adapter copies visible source bytes into reusable aligned AVFrames, extends
  the right/bottom borders by replication, and converts at a working width
  divisible by 32 and even height. Only visible output bytes are copied back.
  Caller buffers need no extra SIMD tail padding. This avoids both overruns and
  partial-block conversion bugs observed in small/odd-sized swscale paths.
- Matrix and range are configured on every conversion, including reused contexts.
  Unspecified YUV range defaults to limited; YUVJ and RGB inputs are full range.
  RGB output is full range; YUV output preserves source range. Matrix conversion
  supports 601/709/FCC/240M/2020 NCL; unsupported matrix conversions return an
  error instead of silently using the wrong coefficients. This is not HDR tone
  mapping or transfer-function conversion.

Qualcomm 64x32 tiled input is detiled into the owned linear NV12 buffer first;
its input must contain the complete padded Qualcomm tile allocation. RenderX
remains a separate stage for 8-bit planar 420. It processes pairs of rows and
preserves an unmatched final chroma row.

## Context ownership and errors

The existing `codec_convert_mt_*` names remain, but AVOS no longer creates a
pool of independent slice workers. Each pipeline keeps one scaler plus reusable
input/output/deinterlace buffers; swscale owns its worker threads. A context
mutex serializes calls. Owners must finish calls before destroying the context.
The stateless entry point creates a temporary context.

Conversion returns zero on success or a negative AVERROR and marks the output
frame erroneous on failure. Decoder callers propagate failure. Android copy
sinks recycle failed output buffers instead of displaying them.

## Validation

Run against an installed host FFmpeg:

```sh
# Set PKG_CONFIG_PATH if the host FFmpeg is not in pkg-config's search path.
test/run-color-conversion.sh
```

Or build the checked-out FFmpeg out of tree with libswscale and libavutil enabled,
then use its static libraries:

```sh
FFMPEG_BUILD_DIR=/path/to/ffmpeg-build test/run-color-conversion.sh
```

The script enables UBSan by default (`SANITIZERS=address,undefined` or `none`
can override it). Every source/destination row in the boundary tests ends at a
protected page. Tests cover 765 input/output/dimension combinations, negative
strides, black/white endpoints, independent matrix/range reference calculations,
YV12 plane order, invalid layouts, context reuse, concurrent independent/shared
contexts, odd-sized deinterlacing and a known Qualcomm tile map.

Validated locally:

- All tests pass on macOS ARM64 against the checked-out FFmpeg's libswscale
  10.1.101, built with AArch64 NEON enabled, with UBSan and guard pages.
- Android NDK builds and links pass for arm64-v8a, armeabi-v7a, x86 and x86_64.
- ASan could not start successfully in this environment; it was not counted as
  passed. ADB could not start its local server, so Android runtime playback,
  thumbnail/teardown stress and performance remain to be validated on devices.

The padded staging approach adds copies and per-context memory. Benchmark software
playback and thumbnail workloads on ARMv7 and ARM64 before shipping. Any later
zero-copy optimization must prove the source and destination allocation extents
and preserve the guarded-buffer tests.

## Parent build cleanup

AVOS itself no longer includes or links libyuv. The parent repository's
`AVP/core.mk` still lists it as a separate native build component. Applying that
edit was blocked by the workspace's filesystem permissions, including after an
escalated attempt. The minimal patch preserves existing edits in that file:

```sh
git -C /path/to/nova-publish/AVP apply /path/to/avos/doc/remove-libyuv-parent.patch
```

The patch removes the native package entry, dedicated build target and cleanup
entry. It does not delete the separate libyuv checkout.

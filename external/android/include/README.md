# Retained Android private headers

This directory keeps the six existing API-21-era headers required by
`libavos_android` for its native-window and optional gralloc implementation:

- `hardware/libhardware/include/hardware/gralloc.h`
- `hardware/libhardware/include/hardware/hardware.h`
- `hardware/libhardware/include/hardware/fb.h`
- `system/core/include/system/window.h`
- `system/core/include/system/graphics.h`
- `system/core/include/cutils/native_handle.h`

Paths above are relative to `21/`. The header contents and their original
license notices are unchanged. The directory name describes the vendored
header snapshot, not the application's minimum API level (23).

After removing the obsolete OMX decoders and unused color-conversion module,
clean NDK dependency files for all four supported ABIs identified only
`android_buffer.c` and `android_window.c` as consumers of this set. Pruning does
not remove gralloc, migrate rotation, or change the public-window fallback.

`dump.sh` downloads audit references into `../header-reference/snapshots/` using
the manifest-aware downloader. It never regenerates this directory or changes
build include paths. The old `<aosp_path> <api>` import interface is removed.

```sh
# From the repository root; omit --api to download all selected API 23-37 releases.
./external/android/include/dump.sh --api 23 --jobs 2
```

Downloaded reference headers and HTTP archives are ignored by Git. Keep only
scripts and written audit documentation as described in
[the reference README](../header-reference/README.md). Any production-header
update still requires a separate compatibility review.

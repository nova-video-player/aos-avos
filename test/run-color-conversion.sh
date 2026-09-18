#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/avos-color.XXXXXX")
trap 'rm -rf "$test_build_dir"' EXIT

# Use the checked-out FFmpeg's static libraries when available. Otherwise use
# a host FFmpeg installation, selected with PKG_CONFIG_PATH if necessary.
if [[ -n ${FFMPEG_BUILD_DIR:-} ]]; then
    ffmpeg_flags=(-I"$repo_dir/ext/ffmpeg" -I"$FFMPEG_BUILD_DIR")
    ffmpeg_libs=("$FFMPEG_BUILD_DIR/libswscale/libswscale.a" "$FFMPEG_BUILD_DIR/libavutil/libavutil.a")
else
    read -r -a ffmpeg_flags <<< "$(pkg-config --cflags libswscale libavutil)"
    read -r -a ffmpeg_libs <<< "$(pkg-config --libs libswscale libavutil)"
fi
sanitize_flags=()
if [[ ${SANITIZERS:-undefined} != none ]]; then
    sanitize_flags=(-fsanitize="${SANITIZERS:-undefined}" -fno-omit-frame-pointer)
fi
"${CC:-cc}" -std=gnu11 -g -O1 -Wall -Wextra -Wno-deprecated-declarations \
    "${sanitize_flags[@]}" -I"$repo_dir/Include" "${ffmpeg_flags[@]}" \
    "$repo_dir/test/color_conversion.c" "$repo_dir/Source/codec_utils.c" \
    "$repo_dir/external/libdeinterlace/deinterlace.c" \
    "${ffmpeg_libs[@]}" -lpthread -lm -o "$test_build_dir/color-conversion"
"$test_build_dir/color-conversion"

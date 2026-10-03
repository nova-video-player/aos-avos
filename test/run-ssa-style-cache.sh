#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/avos-ssa-style.XXXXXX")
trap 'rm -rf "$test_build_dir"' EXIT

# Unlike run-ssa-geometry.sh (which only needs ssa_geometry.h, a pure-arithmetic
# header), this test links the REAL sub_format_ssa.c backend so it exercises
# sync_styles()'s cache-invalidation through the production open()/resize()/
# set_video_box()/render_at() vtable, not a reimplementation of it.
#
# sub_format_ssa.c pulls in sub_engine.h, which -- only for the unrelated
# sub_engine_attach_surface() declaration -- includes <android/native_window.h>.
# sub_format_ssa.c itself never touches ANativeWindow, so a minimal stub header
# (test/android_stub/android/native_window.h) stands in for the NDK on a host
# build; nothing in Source/ is modified to make this test possible.
#
# -DDEBUG_MSG is not passed explicitly: global.h already #defines it
# internally (confirmed against a real build), so sub_format_ssa.c sees it
# regardless of this script's flags.
#
# sub_style.c (SUB_USER_STYLE's setters/snapshot/serial logic) is expected
# to already exist in Source/; point SUB_STYLE_C elsewhere if it lives
# somewhere else in your tree. debug.c and log.c are handled below.

read -r -a ass_flags <<< "$(pkg-config --cflags libass)"
read -r -a ass_libs  <<< "$(pkg-config --libs libass)"

sanitize_flags=()
if [[ ${SANITIZERS:-undefined} != none ]]; then
    sanitize_flags=(-fsanitize="${SANITIZERS:-undefined}" -fno-omit-frame-pointer)
fi

sub_style_src="${SUB_STYLE_C:-$repo_dir/Source/sub_style.c}"
debug_src="${DEBUG_C:-$repo_dir/Source/debug.c}"

# log.c is platform-split in this tree: log_avos.c (real TTY/console serial
# output -- needs tty.h/console.h) and log_android.c (Android NDK logging --
# needs androidndk_utils.h, a non-starter on a host build, same problem
# native_window.h was). Neither is a good fit for a host-side unit test on
# its own merits (log_avos.c's TTY_open()/TTY_write() want a real serial
# device or console behind them, which this test has no business depending
# on), so unlike debug.c -- which is plain enough to prefer for real whenever
# it's present -- log.c's implementation always comes from test_stubs.c's
# no-op serprintf(). LOG_C is still available as an override if you want to
# force-link a real log_*.c anyway (e.g. to catch a log.h signature drift).
log_src="${LOG_C:-}"

# Prefer real debug.c over test_stubs.c's Debug[] stand-in whenever it's
# present -- test_stubs.c's job is to fill whatever gap is left, not to
# shadow a real implementation that's right there. debug.c and log.c are
# tracked independently now: this tree has the former but not a
# host-buildable version of the latter, and the old combined check
# (both-or-neither) silently fell back on debug.c too as a result.
# serprintf() specifically always comes from test_stubs.c (see log_src
# comment above) -- when DEBUG_C alone is real, we still need to skip
# debug.c's OWN Debug[]-sizing/definition inside test_stubs.c (via
# SSA_TEST_STUB_SKIP_DEBUG_DEF) while still providing test_stubs.c's
# serprintf().
stub_flags=()
extra_srcs=()
if [[ -f "$debug_src" ]]; then
    stub_flags+=(-DSSA_TEST_STUB_SKIP_DEBUG_DEF)
    extra_srcs+=("$debug_src")
else
    echo "note: $debug_src not found -- falling back to test_stubs.c's" \
         "Debug[] stand-in for this run." >&2
fi
if [[ -n "$log_src" && -f "$log_src" ]]; then
    stub_flags+=(-DSSA_TEST_STUB_SKIP_SERPRINTF)
    extra_srcs+=("$log_src")
fi

"${CC:-cc}" -std=gnu11 -g -O1 -Wall -Wextra -Wno-deprecated-declarations \
    "${sanitize_flags[@]}" "${stub_flags[@]}" \
    -I"$repo_dir/Include" -I"$repo_dir/test/android_stub" "${ass_flags[@]}" \
    "$repo_dir/test/ssa_style_cache_test.c" \
    "$repo_dir/test/test_stubs.c" \
    "${extra_srcs[@]}" \
    "$repo_dir/Source/sub_format_ssa.c" \
    "$sub_style_src" \
    "${ass_libs[@]}" -lpthread -o "$test_build_dir/ssa-style-cache"
"$test_build_dir/ssa-style-cache"

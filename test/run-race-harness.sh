#!/usr/bin/env bash
#
# Stale-frame-after-close race: hammers open_track()/close_track() against a deliberately
# slow fake backend and asserts nothing is ever visible once close_track() has returned.
# This is the concrete bug fixed by moving poll+publish into one engine-lock hold -- before
# that, a frame from a just-closed track could still land on screen. Reverting that fix
# turns "stale-frame-after-close=0" into thousands of hits (checked by mutation while
# developing this test).
#
# Links the REAL sub_engine.c/sub_render_gl.c against a controllable fake subtitle backend
# (fake_backend.c) instead of the real SSA/SRT/GFX decoders: the race depends on render_at()
# being slow and its NULL/frame answer being under this script's control, which the real
# decoders don't offer. See test/android_stub/ for what's stubbed and why.
source "$(dirname "$0")/frame-identity-lib.sh"

"${CC:-cc}" -std=gnu11 -g -O1 -Wall -Wextra -Wno-deprecated-declarations \
    "${sanitize_flags[@]}" "${stub_flags[@]}" \
    "${fi_common_cflags[@]}" \
    "$repo_dir/test/race_harness_test.c" \
    "$repo_dir/test/fake_backend.c" \
    "$repo_dir/test/test_stubs.c" \
    "$fi_android_stub/android_stub.c" \
    "${extra_srcs[@]}" \
    "$repo_dir/Source/sub_engine.c" \
    "$repo_dir/Source/sub_render_gl.c" \
    "$sub_style_src" \
    -lpthread -o "$test_build_dir/race-harness"
"$test_build_dir/race-harness"

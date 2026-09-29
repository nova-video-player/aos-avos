#!/usr/bin/env bash
#
# When the native render thread announces "subtitle content changed" (the 3D-path push):
# only while paused, only in a 3D UI mode, once per generation, and never with an engine or
# renderer lock held (the test's callback calls back into both, which would self-deadlock
# if either lock were still held).
#
# Links the REAL sub_engine.c/sub_render_gl.c against fake_backend.c; see
# run-race-harness.sh's header for why a fake backend rather than the real decoders.
source "$(dirname "$0")/frame-identity-lib.sh"

"${CC:-cc}" -std=gnu11 -g -O1 -Wall -Wextra -Wno-deprecated-declarations \
    "${sanitize_flags[@]}" "${stub_flags[@]}" \
    "${fi_common_cflags[@]}" \
    "$repo_dir/test/push_policy_test.c" \
    "$repo_dir/test/fake_backend.c" \
    "$repo_dir/test/test_stubs.c" \
    "$fi_android_stub/android_stub.c" \
    "${extra_srcs[@]}" \
    "$repo_dir/Source/sub_engine.c" \
    "$repo_dir/Source/sub_render_gl.c" \
    "$sub_style_src" \
    -lpthread -o "$test_build_dir/push-policy"
"$test_build_dir/push-policy"

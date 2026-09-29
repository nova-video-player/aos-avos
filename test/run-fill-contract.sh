#!/usr/bin/env bash
#
# The renderer's UNCHANGED / CLEAR / FRAME / ERROR contract: which one a fill_bitmap() pull
# answers for a given sequence of backend results, and exactly when frame_generation moves.
#
# Links the REAL sub_engine.c/sub_render_gl.c against fake_backend.c; see
# run-race-harness.sh's header for why a fake backend rather than the real decoders.
source "$(dirname "$0")/frame-identity-lib.sh"

"${CC:-cc}" -std=gnu11 -g -O1 -Wall -Wextra -Wno-deprecated-declarations \
    "${sanitize_flags[@]}" "${stub_flags[@]}" \
    "${fi_common_cflags[@]}" \
    "$repo_dir/test/fill_contract_test.c" \
    "$repo_dir/test/fake_backend.c" \
    "$repo_dir/test/test_stubs.c" \
    "$fi_android_stub/android_stub.c" \
    "${extra_srcs[@]}" \
    "$repo_dir/Source/sub_engine.c" \
    "$repo_dir/Source/sub_render_gl.c" \
    "$sub_style_src" \
    -lpthread -o "$test_build_dir/fill-contract"
"$test_build_dir/fill-contract"

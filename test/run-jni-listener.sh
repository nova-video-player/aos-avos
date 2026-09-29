#!/usr/bin/env bash
#
# Lifecycle of jni_sub_engine.c's native->Java "content changed" listener, run against a
# mock JavaVM/JNIEnv (defined in the test itself) that counts attaches/detaches, tracks live
# global refs, and flags any Java call made on a deleted global ref, from a detached thread,
# or on the wrong object. This validates OUR use of the JNI contract (ref lifetime, thread
# attach/detach, ordering against sub_engine_destroy()) -- it is not a real ART and can't
# stand in for a device run; see jni_listener_lifecycle_test.c's file header.
#
# Additionally links jni_sub_engine.c and the real sub_engine_registry.c (unchanged by this
# work, but required by jni_sub_engine.c's nativeCreate/nativeDestroy).
source "$(dirname "$0")/frame-identity-lib.sh"

"${CC:-cc}" -std=gnu11 -g -O1 -Wall -Wextra -Wno-deprecated-declarations \
    "${sanitize_flags[@]}" "${stub_flags[@]}" \
    "${fi_common_cflags[@]}" \
    "$repo_dir/test/jni_listener_lifecycle_test.c" \
    "$repo_dir/test/fake_backend.c" \
    "$repo_dir/test/test_stubs.c" \
    "$fi_android_stub/android_stub.c" \
    "${extra_srcs[@]}" \
    "$repo_dir/Source/sub_engine.c" \
    "$repo_dir/Source/sub_render_gl.c" \
    "$repo_dir/Source/jni_sub_engine.c" \
    "$repo_dir/Source/sub_engine_registry.c" \
    "$sub_style_src" \
    -lpthread -o "$test_build_dir/jni-listener"
"$test_build_dir/jni-listener"

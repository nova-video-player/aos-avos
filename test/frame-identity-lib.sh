# Sourced by run-race-harness.sh / run-fill-contract.sh / run-push-policy.sh /
# run-jni-listener.sh. Unlike run-ssa-geometry.sh / run-ssa-style-cache.sh /
# run-color-conversion.sh -- each of which needs a different real backend file and so is
# self-contained -- these four all link the SAME engine sources against the SAME stub set,
# so the repo_dir/sanitizer/debug-or-stub detection is factored out here instead of repeated
# four times. Sourced, not executed: sets variables and leaves running the compiler to the
# calling script, which knows which test .c file and which extra sources it needs.
#
# Exports: repo_dir, test_build_dir (caller's trap should clean it up), sanitize_flags[],
# stub_flags[], extra_srcs[], sub_style_src, fi_android_stub, fi_common_cflags[].

set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/avos-frame-identity.XXXXXX")
trap 'rm -rf "$test_build_dir"' EXIT

fi_android_stub="$repo_dir/test/android_stub"
fi_common_cflags=(-I"$repo_dir/Include" -I"$fi_android_stub" -Wno-pointer-sign -Wno-unused-parameter)

# -Wno-unused-parameter (beyond the other scripts' -Wno-deprecated-declarations): every file
# these four tests link is JNI- or vtable-shaped (JNIEnv*/jobject on nearly every native
# entry point, SUB_FORMAT_BACKEND's fixed callback signatures, the mock JNINativeInterface_
# table in jni_listener_lifecycle_test.c) and routinely leaves params unused by design, not
# by mistake. Applies to the actual files under test too, not just borrowed ones -- unlike
# the debug.c/log.c handling below, this isn't about "not the code we're testing", it's that
# the warning class itself doesn't mean anything here.
fi_warn_flags=(-Wall -Wextra -Wno-deprecated-declarations -Wno-unused-parameter)

# SANITIZERS follows the same convention as the other run-*.sh scripts: a comma-separated
# -fsanitize= list, default "undefined", "none" to disable. Try SANITIZERS=thread for the
# race harness, or SANITIZERS=address,undefined for the rest.
sanitize_flags=()
if [[ ${SANITIZERS:-undefined} != none ]]; then
    sanitize_flags=(-fsanitize="${SANITIZERS:-undefined}" -fno-omit-frame-pointer)
fi

# Same real-vs-stub preference as run-ssa-style-cache.sh: prefer Source/sub_style.c and
# Source/debug.c whenever present, fall back to test_stubs.c's stand-ins otherwise.
# serprintf() always comes from test_stubs.c -- see that file's log_src rationale.
# SSA_TEST_STUB_SKIP_FRAME_UNREF is always passed here (unlike the debug/log flags, which
# depend on what's present): sub_engine.c -- always linked by these four tests -- defines
# sub_frame_unref() itself, so test_stubs.c's copy must be skipped unconditionally or the
# link fails on a duplicate symbol.
sub_style_src="${SUB_STYLE_C:-$repo_dir/Source/sub_style.c}"
debug_src="${DEBUG_C:-$repo_dir/Source/debug.c}"
log_src="${LOG_C:-}"

stub_flags=(-DSSA_TEST_STUB_SKIP_FRAME_UNREF)
extra_srcs=()
if [[ -f "$debug_src" ]]; then
    stub_flags+=(-DSSA_TEST_STUB_SKIP_DEBUG_DEF)
    extra_srcs+=("$debug_src")
else
    echo "note: $debug_src not found -- falling back to test_stubs.c's Debug[] stand-in for this run." >&2
fi
if [[ -n "$log_src" && -f "$log_src" ]]; then
    stub_flags+=(-DSSA_TEST_STUB_SKIP_SERPRINTF)
    extra_srcs+=("$log_src")
fi

# debug.c/log.c are borrowed for realism, not the code these tests exist to check -- unlike
# fi_warn_flags above, warnings from THEM specifically aren't informative here (whatever they
# are, they're pre-existing and out of scope for this test change). Compile each with -w into
# an object file and link that instead of the .c, so the files actually under test
# (sub_engine.c, sub_render_gl.c, jni_sub_engine.c, fake_backend.c, the test .c itself) still
# get full fi_warn_flags scrutiny.
compiled_extra_srcs=()
for fi_src in "${extra_srcs[@]}"; do
    fi_obj="$test_build_dir/$(basename "${fi_src%.c}").o"
    "${CC:-cc}" -std=gnu11 -g -O1 -w -c "${fi_common_cflags[@]}" "$fi_src" -o "$fi_obj"
    compiled_extra_srcs+=("$fi_obj")
done

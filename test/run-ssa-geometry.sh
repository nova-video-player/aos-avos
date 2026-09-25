#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/avos-ssa.XXXXXX")
trap 'rm -rf "$test_build_dir"' EXIT

# Use a host libass installation, selected with PKG_CONFIG_PATH if necessary.
read -r -a ass_flags <<< "$(pkg-config --cflags libass)"
read -r -a ass_libs  <<< "$(pkg-config --libs libass)"

sanitize_flags=()
if [[ ${SANITIZERS:-undefined} != none ]]; then
    sanitize_flags=(-fsanitize="${SANITIZERS:-undefined}" -fno-omit-frame-pointer)
fi

"${CC:-cc}" -std=gnu11 -g -O1 -Wall -Wextra -Wno-deprecated-declarations \
    "${sanitize_flags[@]}" -I"$repo_dir/Include" "${ass_flags[@]}" \
    "$repo_dir/test/ssa_geometry_test.c" \
    "${ass_libs[@]}" -o "$test_build_dir/ssa-geometry"
"$test_build_dir/ssa-geometry"

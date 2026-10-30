#!/bin/sh
# Download audit references only; never regenerate the production include/21 tree.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
reference_dir="$script_dir/../header-reference"

usage() {
    echo "Usage: $0 [--api 23..37]... [--jobs N]" >&2
    echo "Downloads references to external/android/header-reference/snapshots/." >&2
    echo "The old <aosp_path> <api> interface is no longer supported." >&2
}

# Do not allow --output to redirect a download into the production headers.
# Keep the original argument list for Python's value/range validation.
validate_args() {
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --api|--jobs)
                if [ "$#" -lt 2 ]; then usage; exit 2; fi
                shift 2
                ;;
            -h|--help) usage; exit 0 ;;
            *) usage; exit 2 ;;
        esac
    done
}
validate_args "$@"

exec python3 "$reference_dir/dump_aosp_headers.py" \
    --output "$reference_dir/snapshots" "$@"

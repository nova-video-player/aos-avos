#!/bin/bash
# Hold D-pad right/left for at least five seconds, then require playback to
# recover before the next hold. Requires Android's `input keyevent --duration`.
#
# Usage: ./stress_seek_dpad_hold_validate.sh [count] [timeout_seconds] [stable_media_ms]
#
# Holds are paired: a hold in one direction is followed by an equal-duration
# hold in the opposite direction. Playback is initially centered at 50% to
# avoid progressively reaching the end of the file. Use a long video because
# Nova may accelerate the seek while the key remains down.
#
# Example:
#   REQUIRE_AUDIO_PRESENTATION=1 ./stress_seek_dpad_hold_validate.sh 40 10 3000
#
# Optional tuning (values below 5000 ms are rejected):
#   DPAD_HOLD_MIN_MS=5000 DPAD_HOLD_MAX_MS=8000
#   DPAD_RECENTER_PERCENT=50  (-1 keeps the current playback position)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export SEEK_DRIVER=dpad_hold
export DPAD_RECENTER_EACH_PAIR=${DPAD_RECENTER_EACH_PAIR:-1}

exec "$SCRIPT_DIR/stress_seek_validate.sh" "$@"

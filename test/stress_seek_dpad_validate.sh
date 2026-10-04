#!/bin/bash
# Exercise the Android TV seek UI with repeated D-pad left/right bursts, then
# require playback to recover before sending the next burst.
#
# Usage: ./stress_seek_dpad_validate.sh [count] [timeout_seconds] [stable_media_ms]
#
# Bursts are paired: a random forward/backward burst is followed by the same
# number of presses in the opposite direction. Playback is initially centered
# at 50%, so the test does not progressively drift toward the end of the file.
# Use a video long enough for the configured maximum burst.
#
# Example with AudioTrack presentation required:
#   REQUIRE_AUDIO_PRESENTATION=1 ./stress_seek_dpad_validate.sh 100 8 2000
#
# Optional tuning:
#   DPAD_MIN_PRESSES=3 DPAD_MAX_PRESSES=10 DPAD_KEY_INTERVAL_MS=80
#   DPAD_RECENTER_PERCENT=50  (-1 keeps the current playback position)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export SEEK_DRIVER=dpad

exec "$SCRIPT_DIR/stress_seek_validate.sh" "$@"

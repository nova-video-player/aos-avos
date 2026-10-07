#!/bin/bash
# Speed ramp validation using analyze_stress.py.
# See doc/TEST.md for arguments, thresholds, evidence and limitations.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
COUNT=${1:-5}
HOLD_SEC=${2:-8}
STEP_DELAY_SEC=${3:-0.02}
SETTLE_SEC=${4:-2}
LABEL=${LABEL:-auto}
SPEED_STEPS=${SPEED_STEPS:-10}
SPEED_HI=$(awk -v steps="$SPEED_STEPS" 'BEGIN { printf "%.2f", 1 + steps * 0.05 }')
HI_SPEED_MIN=${HI_SPEED_MIN:-1.45}
WINDOW_GAP_MS=${WINDOW_GAP_MS:-1500}
AV_DIFF_MAX_MS=${AV_DIFF_MAX_MS:-1000}
VIDEO_GAP_MAX_MS=${VIDEO_GAP_MAX_MS:-250}
WRITE_GAP_MAX_MS=${WRITE_GAP_MAX_MS:-250}
LATE_DROP_MAX=${LATE_DROP_MAX:-0}
UNDERRUN_MAX=${UNDERRUN_MAX:-0}
ARM_UNDERRUN=${ARM_UNDERRUN:-0}
REQUIRE_FILTER=${REQUIRE_FILTER:-}
CAPTURE_BUGREPORT=${CAPTURE_BUGREPORT:-0}
SPEED_UP_KEY=${SPEED_UP_KEY:-166}
SPEED_DOWN_KEY=${SPEED_DOWN_KEY:-167}
PACKAGE=${PACKAGE:-org.courville.nova}
HOLD_MIN_MS=${HOLD_MIN_MS:-$(( HOLD_SEC * 600 ))}
STAMP=$(date +%Y%m%d-%H%M%S)
OUTPUT_DIR=${OUTPUT_DIR:-"$SCRIPT_DIR/../speed-validation-$STAMP"}
RAW_LOG="$OUTPUT_DIR/logcat.log"
LATEST="$OUTPUT_DIR/current-cycle.log"
RESULTS="$OUTPUT_DIR/results.txt"
LOGCAT_PID=""
LOGCAT_ADB_PID=""
LOGCAT_PIPE=""

fail()
{
	printf 'ERROR: %s\n' "$1" >&2
	exit 2
}

cleanup()
{
	stress_stop_logcat
}

capture_failure()
{
	reason=$1
	cycle=$2

	stress_failure_status "$reason"
	printf '%s cycle=%s label=%s reason=%s\n' "$FAILURE_STATUS" "$cycle" "$LABEL" "$reason" | tee -a "$RESULTS"
	cp "$LATEST" "$OUTPUT_DIR/failure-cycle-$cycle.log" 2>/dev/null || true
	adb shell dumpsys media.audio_flinger > "$OUTPUT_DIR/audio_flinger.txt" 2>&1 || true
	adb shell dumpsys SurfaceFlinger > "$OUTPUT_DIR/surfaceflinger.txt" 2>&1 || true
	adb exec-out screencap -p > "$OUTPUT_DIR/screenshot.png" 2>/dev/null || true
	if [ "$CAPTURE_BUGREPORT" = "1" ]; then
		adb bugreport "$OUTPUT_DIR/bugreport" > "$OUTPUT_DIR/bugreport-command.txt" 2>&1 || true
	fi
	printf 'Artifacts: %s\n' "$OUTPUT_DIR"
}

# Two passes over the segment: the first derives the steady high-speed window
# from put_time_calc's speed field, the second measures inside that window.
analyse_cycle()
{
	python3 "$SCRIPT_DIR/analyze_stress.py" speed "$LATEST"
}

is_uint()
{
	case "$1" in
		''|*[!0-9]*) return 1 ;;
		*) return 0 ;;
	esac
}

for number in "$COUNT" "$HOLD_SEC" "$SPEED_STEPS" "$WINDOW_GAP_MS" \
	"$AV_DIFF_MAX_MS" "$VIDEO_GAP_MAX_MS" "$WRITE_GAP_MAX_MS" \
	"$LATE_DROP_MAX" "$UNDERRUN_MAX" "$HOLD_MIN_MS"; do
	is_uint "$number" || fail "arguments and thresholds must be non-negative integers"
done
[ "$COUNT" -gt 0 ] || fail "count must be greater than zero"
[ "$HOLD_SEC" -gt 0 ] || fail "hold_sec must be greater than zero"
[ "$SPEED_STEPS" -gt 0 ] && [ "$SPEED_STEPS" -le 20 ] || fail "speed_steps must be 1..20 (up to 2x)"
for key in "$SPEED_UP_KEY" "$SPEED_DOWN_KEY"; do
	is_uint "$key" || fail "speed keycodes must be integers"
done
[[ "$STEP_DELAY_SEC" =~ ^([0-9]+([.][0-9]*)?|[.][0-9]+)$ ]] || fail "invalid step delay"
is_uint "$SETTLE_SEC" || fail "settle_sec must be an integer"
[ "$SETTLE_SEC" -gt 0 ] || fail "settle_sec must be greater than zero"
[ "$ARM_UNDERRUN" -eq 0 ] || [ "$ARM_UNDERRUN" -eq 1 ] || fail "ARM_UNDERRUN must be 0 or 1"

STRESS_MODE=speed
. "$SCRIPT_DIR/stress_common.sh"
stress_configure

command -v adb >/dev/null 2>&1 || fail "adb was not found"
adb get-state >/dev/null 2>&1 || fail "no adb device is ready"
PLAYER_PID=$(adb shell pidof -s "$PACKAGE" 2>/dev/null | tr -d '\r')
[ -n "$PLAYER_PID" ] || fail "Nova is not running; start playback first"

mkdir -p "$OUTPUT_DIR" || fail "cannot create $OUTPUT_DIR"
: > "$RAW_LOG"
: > "$RESULTS"
trap cleanup EXIT
trap 'exit 130' INT TERM

if [ "$ARM_UNDERRUN" = "1" ]; then
	printf 'Arming AudioTrack underrun logging (perturbs the writer path).\n'
	"$SCRIPT_DIR/av.sh" at_underrun 1
fi

stress_start_logcat

printf 'Audio speed validation: label=%s cycles=%d ramp=1.00x->%.2fx in %d steps @%ss hold=%ss settle=%ss\n' \
	"$LABEL" "$COUNT" "$SPEED_HI" "$SPEED_STEPS" "$STEP_DELAY_SEC" "$HOLD_SEC" "$SETTLE_SEC"
printf 'Artifacts: %s\n' "$OUTPUT_DIR"

i=1
while [ "$i" -le "$COUNT" ]; do
	start_line=$(( $(wc -l < "$RAW_LOG") + 1 ))
	printf '[%d/%d] ramp up ... ' "$i" "$COUNT"
	adb shell "for s in \$(seq 1 $SPEED_STEPS); do input keyevent $SPEED_UP_KEY; sleep $STEP_DELAY_SEC; done" \
		>/dev/null 2>&1 || fail "speed-up injection failed"
	printf 'hold %ss ... ' "$HOLD_SEC"
	sleep "$HOLD_SEC"
	printf 'ramp down ... '
	adb shell "for s in \$(seq 1 $SPEED_STEPS); do input keyevent $SPEED_DOWN_KEY; sleep $STEP_DELAY_SEC; done" \
		>/dev/null 2>&1 || fail "speed-down injection failed"
	printf 'settle %ss ... ' "$SETTLE_SEC"
	sleep "$SETTLE_SEC"

	current_pid=$(adb shell pidof -s "$PACKAGE" 2>/dev/null | tr -d '\r')
	if [ -z "$current_pid" ] || [ "$current_pid" != "$PLAYER_PID" ]; then
		tail -n "+$start_line" "$RAW_LOG" > "$LATEST"
		capture_failure "Nova process exited or restarted" "$i"
		exit "$FAILURE_RC"
	fi
	stress_snapshot
	if grep -Eq "Fatal signal|FATAL EXCEPTION|ANR in $PACKAGE|ERROR_DEAD_OBJECT" "$LATEST"; then
		capture_failure "fatal runtime or decoder error" "$i"
		exit "$FAILURE_RC"
	fi

	health=$(analyse_cycle) || fail "analyzer failed: $health"
	healthy=$(printf '%s\n' "$health" | sed -n 's/.*healthy=\([0-9][0-9]*\).*/\1/p')
	if [ "$healthy" = "1" ]; then
		printf 'PASS %s\n' "$health" | tee -a "$RESULTS"
	else
		reason="steady high-speed window incomplete or thresholds exceeded"
		hi_ms=$(printf '%s\n' "$health" | sed -n 's/.*hi_ms=\([0-9][0-9]*\).*/\1/p')
		[ -n "$hi_ms" ] && [ "$hi_ms" -lt "$HOLD_MIN_MS" ] && reason="high-speed window too short (${hi_ms}ms)"
		hi_wg=$(printf '%s\n' "$health" | sed -n 's/.*hi_max_write_gap=\([0-9][0-9]*\).*/\1/p')
		[ -n "$hi_wg" ] && [ "$hi_wg" -gt "$WRITE_GAP_MAX_MS" ] && reason="audio write gap at high speed (${hi_wg}ms)"
		hi_vg=$(printf '%s\n' "$health" | sed -n 's/.*hi_max_video_gap=\([0-9][0-9]*\).*/\1/p')
		[ -n "$hi_vg" ] && [ "$hi_vg" -gt "$VIDEO_GAP_MAX_MS" ] && reason="video scheduling gap at high speed (${hi_vg}ms)"
		cat=$(printf '%s\n' "$health" | sed -n 's/.* hi_underruns=\([0-9][0-9]*\).*/\1/p')
		[ -n "$cat" ] && [ "$cat" -gt "$UNDERRUN_MAX" ] && reason="AudioTrack underruns at high speed (${cat})"
		capture_failure "$reason; $health" "$i"
		exit "$FAILURE_RC"
	fi

	i=$((i + 1))
done

printf 'PASS: %d cycles on %s within thresholds. Results: %s\n' "$COUNT" "$LABEL" "$RESULTS"

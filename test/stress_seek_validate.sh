#!/bin/bash
# Seek validation using analyze_stress.py.
# See doc/TEST.md for arguments, thresholds, evidence and limitations.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
COUNT=${1:-100}
TIMEOUT_SEC=${2:-8}
STABLE_MEDIA_MS=${3:-2000}
MIN_PERCENT=${MIN_PERCENT:-5}
MAX_PERCENT=${MAX_PERCENT:-90}
POLL_MS=${POLL_MS:-500}
AV_DIFF_MAX_MS=${AV_DIFF_MAX_MS:-1000}
VIDEO_GAP_MAX_MS=${VIDEO_GAP_MAX_MS:-250}
REQUIRE_AUDIO_PRESENTATION=${REQUIRE_AUDIO_PRESENTATION:-1}
CAPTURE_BUGREPORT=${CAPTURE_BUGREPORT:-0}
SEEK_DRIVER=${SEEK_DRIVER:-absolute}
DPAD_MIN_PRESSES=${DPAD_MIN_PRESSES:-3}
DPAD_MAX_PRESSES=${DPAD_MAX_PRESSES:-10}
DPAD_KEY_INTERVAL_MS=${DPAD_KEY_INTERVAL_MS:-80}
DPAD_RECENTER_PERCENT=${DPAD_RECENTER_PERCENT:-50}
DPAD_HOLD_MIN_MS=${DPAD_HOLD_MIN_MS:-5000}
DPAD_HOLD_MAX_MS=${DPAD_HOLD_MAX_MS:-5000}
DPAD_RECENTER_EACH_PAIR=${DPAD_RECENTER_EACH_PAIR:-0}
PACKAGE=${PACKAGE:-org.courville.nova}
STAMP=$(date +%Y%m%d-%H%M%S)
OUTPUT_DIR=${OUTPUT_DIR:-"$SCRIPT_DIR/../seek-validation-$STAMP"}
RAW_LOG="$OUTPUT_DIR/logcat.log"
LATEST="$OUTPUT_DIR/current-seek.log"
RESULTS="$OUTPUT_DIR/results.txt"
LOGCAT_PID=""

fail()
{
	printf 'ERROR: %s\n' "$1" >&2
	exit 2
}

is_uint()
{
	case "$1" in
		''|*[!0-9]*) return 1 ;;
		*) return 0 ;;
	esac
}

cleanup()
{
	stress_stop_logcat
}

capture_failure()
{
	reason=$1
	iteration=$2
	target=$3

	stress_failure_status "$reason"
	printf '%s iteration=%s target=%s reason=%s\n' \
		"$FAILURE_STATUS" "$iteration" "$target" "$reason" | tee -a "$RESULTS"
	cp "$LATEST" "$OUTPUT_DIR/failure-iteration-$iteration.log" 2>/dev/null || true
	adb shell dumpsys media.audio_flinger > "$OUTPUT_DIR/audio_flinger.txt" 2>&1 || true
	adb shell dumpsys media.audio_policy > "$OUTPUT_DIR/audio_policy.txt" 2>&1 || true
	adb shell dumpsys activity processes "$PACKAGE" > "$OUTPUT_DIR/activity_process.txt" 2>&1 || true
	adb shell dumpsys SurfaceFlinger > "$OUTPUT_DIR/surfaceflinger.txt" 2>&1 || true
	adb exec-out screencap -p > "$OUTPUT_DIR/screenshot.png" 2>/dev/null || true
	if [ "$CAPTURE_BUGREPORT" = "1" ]; then
		adb bugreport "$OUTPUT_DIR/bugreport" > "$OUTPUT_DIR/bugreport-command.txt" 2>&1 || true
	fi
	printf 'Playback was left untouched in its failed state. Artifacts: %s\n' "$OUTPUT_DIR"
}

analyse_segment()
{
	python3 "$SCRIPT_DIR/analyze_stress.py" seek "$LATEST"
}

for number in "$COUNT" "$TIMEOUT_SEC" "$STABLE_MEDIA_MS" "$MIN_PERCENT" \
	"$MAX_PERCENT" "$POLL_MS" "$AV_DIFF_MAX_MS" "$VIDEO_GAP_MAX_MS" \
	"$DPAD_MIN_PRESSES" "$DPAD_MAX_PRESSES" "$DPAD_KEY_INTERVAL_MS" \
	"$DPAD_HOLD_MIN_MS" "$DPAD_HOLD_MAX_MS" "$DPAD_RECENTER_EACH_PAIR"; do
	is_uint "$number" || fail "arguments and thresholds must be non-negative integers"
done
[ "$COUNT" -gt 0 ] || fail "count must be greater than zero"
[ "$TIMEOUT_SEC" -gt 0 ] || fail "timeout must be greater than zero"
[ "$STABLE_MEDIA_MS" -gt 0 ] || fail "stable_media_ms must be greater than zero"
[ "$MIN_PERCENT" -ge 0 ] && [ "$MAX_PERCENT" -le 100 ] && \
	[ "$MAX_PERCENT" -gt "$MIN_PERCENT" ] || fail "invalid seek percentage range"
[ "$DPAD_MIN_PRESSES" -gt 0 ] && \
	[ "$DPAD_MAX_PRESSES" -ge "$DPAD_MIN_PRESSES" ] || fail "invalid D-pad press range"
[ "$DPAD_HOLD_MIN_MS" -ge 5000 ] && \
	[ "$DPAD_HOLD_MAX_MS" -ge "$DPAD_HOLD_MIN_MS" ] || fail "D-pad holds must be at least 5000 ms"
[ "$DPAD_RECENTER_EACH_PAIR" -eq 0 ] || [ "$DPAD_RECENTER_EACH_PAIR" -eq 1 ] || \
	fail "DPAD_RECENTER_EACH_PAIR must be 0 or 1"
case "$SEEK_DRIVER" in
	absolute|dpad|dpad_hold) ;;
	*) fail "SEEK_DRIVER must be absolute, dpad, or dpad_hold" ;;
esac
if [ "$SEEK_DRIVER" = "dpad" ] || [ "$SEEK_DRIVER" = "dpad_hold" ]; then
	case "$DPAD_RECENTER_PERCENT" in
		-1) ;;
		*)
			is_uint "$DPAD_RECENTER_PERCENT" || fail "invalid D-pad recenter percentage"
			[ "$DPAD_RECENTER_PERCENT" -le 100 ] || fail "invalid D-pad recenter percentage"
			;;
	esac
fi

STRESS_MODE=seek
. "$SCRIPT_DIR/stress_common.sh"
stress_configure

command -v adb >/dev/null 2>&1 || fail "adb was not found"
adb get-state >/dev/null 2>&1 || fail "no adb device is ready"
if [ "$SEEK_DRIVER" = "dpad_hold" ] &&
   ! adb shell input help 2>&1 | grep -q -- '--duration'; then
	fail "this device does not support adb input keyevent --duration"
fi
PLAYER_PID=$(adb shell pidof -s "$PACKAGE" 2>/dev/null | tr -d '\r')
[ -n "$PLAYER_PID" ] || fail "Nova is not running; start playback first"

mkdir -p "$OUTPUT_DIR" || fail "cannot create $OUTPUT_DIR"
: > "$RAW_LOG"
: > "$RESULTS"
trap cleanup EXIT
trap 'exit 130' INT TERM

if [ "$ARM_UNDERRUN" = 1 ]; then
	"$SCRIPT_DIR/av.sh" at_underrun 1 || fail "could not arm underruns"
fi
stress_start_logcat

printf 'Seek recovery validation: count=%d timeout=%ss stable=%dms range=%d-%d%%\n' \
	"$COUNT" "$TIMEOUT_SEC" "$STABLE_MEDIA_MS" "$MIN_PERCENT" "$MAX_PERCENT"
printf 'Artifacts: %s\n' "$OUTPUT_DIR"

if { [ "$SEEK_DRIVER" = "dpad" ] || [ "$SEEK_DRIVER" = "dpad_hold" ]; } && \
   [ "$DPAD_RECENTER_PERCENT" -ge 0 ]; then
	printf 'Centering playback at %d%% before D-pad bursts...\n' "$DPAD_RECENTER_PERCENT"
	"$SCRIPT_DIR/av.sh" ssx "$DPAD_RECENTER_PERCENT"
	sleep 4
fi

previous_pct=-100
dpad_pair_presses=0
dpad_pair_hold_ms=0
dpad_pair_first=""
i=1
while [ "$i" -le "$COUNT" ]; do
	if [ "$SEEK_DRIVER" = "dpad_hold" ] && [ "$DPAD_RECENTER_EACH_PAIR" = "1" ] &&
	   [ $((i % 2)) -eq 1 ] && [ "$i" -gt 1 ] && [ "$DPAD_RECENTER_PERCENT" -ge 0 ]; then
		printf '[%d/%d] recenter=%d%% before next hold pair\n' \
			"$i" "$COUNT" "$DPAD_RECENTER_PERCENT"
		"$SCRIPT_DIR/av.sh" ssx "$DPAD_RECENTER_PERCENT"
		sleep 4
	fi
	start_line=$(( $(wc -l < "$RAW_LOG") + 1 ))
	if [ "$SEEK_DRIVER" = "dpad" ] || [ "$SEEK_DRIVER" = "dpad_hold" ]; then
		if [ $((i % 2)) -eq 1 ]; then
			if [ "$SEEK_DRIVER" = "dpad" ]; then
				dpad_range=$((DPAD_MAX_PRESSES - DPAD_MIN_PRESSES + 1))
				dpad_pair_presses=$((DPAD_MIN_PRESSES + RANDOM % dpad_range))
			else
				dpad_range=$((DPAD_HOLD_MAX_MS - DPAD_HOLD_MIN_MS + 1))
				dpad_pair_hold_ms=$((DPAD_HOLD_MIN_MS + RANDOM % dpad_range))
			fi
			if [ $((RANDOM % 2)) -eq 0 ]; then
				dpad_pair_first="right"
			else
				dpad_pair_first="left"
			fi
			direction=$dpad_pair_first
		else
			if [ "$dpad_pair_first" = "right" ]; then
				direction="left"
			else
				direction="right"
			fi
		fi
		if [ "$direction" = "right" ]; then
			keycode=KEYCODE_DPAD_RIGHT
		else
			keycode=KEYCODE_DPAD_LEFT
		fi
		if [ "$SEEK_DRIVER" = "dpad_hold" ]; then
			target="hold-${direction}-${dpad_pair_hold_ms}ms"
			printf '[%d/%d] dpad-hold=%s %dms ... ' \
				"$i" "$COUNT" "$direction" "$dpad_pair_hold_ms"
			adb shell input dpad keyevent --duration "$dpad_pair_hold_ms" "$keycode" \
				>/dev/null 2>&1 || fail "timed D-pad injection failed"
		else
			target="${direction}x${dpad_pair_presses}"
			printf '[%d/%d] dpad=%s x%d ... ' "$i" "$COUNT" "$direction" "$dpad_pair_presses"
			press=1
			while [ "$press" -le "$dpad_pair_presses" ]; do
				adb shell input keyevent "$keycode" >/dev/null 2>&1 || fail "D-pad injection failed"
				if [ "$press" -lt "$dpad_pair_presses" ]; then
					sleep "$(awk -v ms="$DPAD_KEY_INTERVAL_MS" 'BEGIN { printf "%.3f", ms / 1000 }')"
				fi
				press=$((press + 1))
			done
		fi
	else
		range=$((MAX_PERCENT - MIN_PERCENT + 1))
		pct=$((MIN_PERCENT + RANDOM % range))
		tries=0
		while [ $((pct - previous_pct)) -lt 10 ] && \
		      [ $((previous_pct - pct)) -lt 10 ] && [ "$tries" -lt 20 ]; do
			pct=$((MIN_PERCENT + RANDOM % range))
			tries=$((tries + 1))
		done
		previous_pct=$pct
		target="${pct}%"
		printf '[%d/%d] seek=%d%% ... ' "$i" "$COUNT" "$pct"
		"$SCRIPT_DIR/av.sh" ssx "$pct" || fail "seek command failed"
		adb shell input keyevent KEYCODE_DPAD_DOWN >/dev/null 2>&1 || true
	fi

	start_time=$(date +%s)
	last_health=""
	while :; do
		current_pid=$(adb shell pidof -s "$PACKAGE" 2>/dev/null | tr -d '\r')
		if [ -z "$current_pid" ] || [ "$current_pid" != "$PLAYER_PID" ]; then
			tail -n "+$start_line" "$RAW_LOG" > "$LATEST"
			capture_failure "Nova process exited or restarted" "$i" "$target"
			exit "$FAILURE_RC"
		fi

		stress_snapshot
		if grep -Eq "Fatal signal|FATAL EXCEPTION|ANR in $PACKAGE|ERROR_DEAD_OBJECT|seek watchdog restart failed" "$LATEST"; then
			capture_failure "fatal runtime or decoder error" "$i" "$target"
			exit "$FAILURE_RC"
		fi

		last_health=$(analyse_segment) || fail "analyzer failed: $last_health"
		healthy=$(printf '%s\n' "$last_health" | sed -n 's/.*healthy=\([0-9][0-9]*\).*/\1/p')
		presented_count=$(printf '%s\n' "$last_health" | sed -n 's/.*presented_count=\([0-9][0-9]*\).*/\1/p')
		presented_span=$(printf '%s\n' "$last_health" | sed -n 's/.*presented_span=\([0-9][0-9]*\).*/\1/p')
		ready_count=$(printf '%s\n' "$last_health" | sed -n 's/.*ready=\([0-9][0-9]*\).*/\1/p')
		if [ "$healthy" = "1" ]; then
			printf 'PASS %s\n' "$last_health" | tee -a "$RESULTS"
			break
		fi

		now=$(date +%s)
		if [ $((now - start_time)) -ge "$TIMEOUT_SEC" ]; then
			reason="recovery timeout"
			case " $last_health " in
				*" ready=0 "*) reason="seek target never reached" ;;
				*" writes=0 "*) reason="audio producer did not resume" ;;
				*" video_span=0 "*|*" video_span=0.000 "*) reason="video is frozen after seek" ;;
			esac
			max_video_gap=$(printf '%s\n' "$last_health" | sed -n 's/.*max_video_gap=\([0-9][0-9]*\).*/\1/p')
			if [ -n "$max_video_gap" ] && [ "$max_video_gap" -gt "$VIDEO_GAP_MAX_MS" ]; then
				reason="post-seek video scheduling gap (${max_video_gap}ms)"
			fi
			if [ "$ready_count" -gt 0 ] && [ "$REQUIRE_AUDIO_PRESENTATION" = "1" ] && \
			   { [ "$presented_count" -lt 2 ] || [ "$presented_span" -le 0 ]; }; then
				reason="AudioTrack presentation did not advance"
			fi
			capture_failure "$reason; $last_health" "$i" "$target"
			exit "$FAILURE_RC"
		fi
		sleep "$(awk -v ms="$POLL_MS" 'BEGIN { printf "%.3f", ms / 1000 }')"
	done

	i=$((i + 1))
done

printf 'PASS: all %d seeks recovered. Results: %s\n' "$COUNT" "$RESULTS"

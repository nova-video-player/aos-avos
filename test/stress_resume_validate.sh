#!/bin/bash
# Pause/resume validation using analyze_stress.py.
# See doc/TEST.md for arguments, thresholds, evidence and limitations.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
COUNT=${1:-50}
TIMEOUT_SEC=${2:-8}
STABLE_MEDIA_MS=${3:-2000}
MIN_PAUSE_MS=${4:-500}
MAX_PAUSE_MS=${5:-3000}
POLL_MS=${POLL_MS:-500}
AV_DIFF_MAX_MS=${AV_DIFF_MAX_MS:-1000}
VIDEO_GAP_MAX_MS=${VIDEO_GAP_MAX_MS:-250}
WRITE_GAP_MAX_MS=${WRITE_GAP_MAX_MS:-250}
RESUME_LATENCY_MAX_MS=${RESUME_LATENCY_MAX_MS:-1000}
REBASE_MAX=${REBASE_MAX:-1}
LATE_DROP_MAX=${LATE_DROP_MAX:-0}
UNDERRUN_MAX=${UNDERRUN_MAX:-0}
ARM_UNDERRUN=${ARM_UNDERRUN:-0}
CAPTURE_BUGREPORT=${CAPTURE_BUGREPORT:-0}
PAUSE_KEY=${PAUSE_KEY:-KEYCODE_MEDIA_PAUSE}
PLAY_KEY=${PLAY_KEY:-KEYCODE_MEDIA_PLAY}
BURST_PAIRS=${BURST_PAIRS:-0}
BURST_GAP_MS=${BURST_GAP_MS:-0}
BURST_KEY=${BURST_KEY:-KEYCODE_DPAD_CENTER}
DRY_RUN=${DRY_RUN:-0}
PACKAGE=${PACKAGE:-org.courville.nova}
STAMP=$(date +%Y%m%d-%H%M%S)
OUTPUT_DIR=${OUTPUT_DIR:-"$SCRIPT_DIR/../resume-validation-$STAMP"}
RAW_LOG="$OUTPUT_DIR/logcat.log"
LATEST="$OUTPUT_DIR/current-resume.log"
RESULTS="$OUTPUT_DIR/results.txt"
LOGCAT_PID=""
LOGCAT_ADB_PID=""
LOGCAT_PIPE=""

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
	python3 "$SCRIPT_DIR/analyze_stress.py" resume "$LATEST"
}

build_burst_command()
{
	# Only validated integers and key names enter this remote shell program.
	printf 'log -p i -t avos_test AVOS_TEST_BURST_BEGIN || exit 1\n'
	if [ "$BURST_GAP_MS" -eq 0 ]; then
		# One input process avoids per-key adb and input-process startup costs.
		printf 'input keyevent'
		for ((press=0; press<BURST_PAIRS*2; press++)); do
			printf ' %s' "$BURST_KEY"
		done
		printf ' || exit 1\n'
	else
		gap_sec=$(awk -v ms="$BURST_GAP_MS" 'BEGIN { printf "%.3f", ms / 1000 }')
		# Delays run on the device. input still has per-invocation overhead;
		# the requested gap is additional sleep, not guaranteed key spacing.
		cat <<EOF
press=0
while [ "\$press" -lt $((BURST_PAIRS * 2)) ]; do
    input keyevent $BURST_KEY || exit 1
    press=\$((press + 1))
    if [ "\$press" -lt $((BURST_PAIRS * 2)) ]; then
        sleep $gap_sec || exit 1
    fi
done
EOF
	fi
	printf 'log -p i -t avos_test AVOS_TEST_BURST_END || exit 1\n'
}

for number in "$COUNT" "$TIMEOUT_SEC" "$STABLE_MEDIA_MS" "$MIN_PAUSE_MS" \
	"$MAX_PAUSE_MS" "$POLL_MS" "$AV_DIFF_MAX_MS" "$VIDEO_GAP_MAX_MS" \
	"$WRITE_GAP_MAX_MS" "$RESUME_LATENCY_MAX_MS" "$REBASE_MAX" \
	"$LATE_DROP_MAX" "$UNDERRUN_MAX" "$BURST_PAIRS" "$BURST_GAP_MS" "$DRY_RUN"; do
	is_uint "$number" || fail "arguments and thresholds must be non-negative integers"
done
[ "$COUNT" -gt 0 ] || fail "count must be greater than zero"
[ "$TIMEOUT_SEC" -gt 0 ] || fail "timeout must be greater than zero"
[ "$STABLE_MEDIA_MS" -gt 0 ] || fail "stable_media_ms must be greater than zero"
[ "$MAX_PAUSE_MS" -ge "$MIN_PAUSE_MS" ] || fail "invalid pause duration range"
[ "$ARM_UNDERRUN" -eq 0 ] || [ "$ARM_UNDERRUN" -eq 1 ] || fail "ARM_UNDERRUN must be 0 or 1"
[ "$DRY_RUN" -le 1 ] || fail "DRY_RUN must be 0 or 1"
[ "$BURST_PAIRS" -le 100 ] || fail "BURST_PAIRS must be 0-100"
[ "$BURST_GAP_MS" -le 5000 ] || fail "BURST_GAP_MS must be 0-5000"
BURST_PAIRS=$((10#$BURST_PAIRS))
BURST_GAP_MS=$((10#$BURST_GAP_MS))
case "$BURST_KEY" in
	''|*[!A-Z0-9_]*) fail "BURST_KEY must be an uppercase key name or numeric keycode" ;;
esac
if [ "$BURST_PAIRS" -eq 0 ] && [ "$BURST_GAP_MS" -ne 0 ]; then
	fail "BURST_GAP_MS requires BURST_PAIRS greater than zero"
fi
export RESUME_BURST_PAIRS="$BURST_PAIRS"

if [ "$DRY_RUN" -eq 1 ]; then
	[ "$BURST_PAIRS" -gt 0 ] || fail "DRY_RUN requires burst mode"
	build_burst_command
	exit 0
fi

STRESS_MODE=resume
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

printf 'Resume recovery validation: count=%d timeout=%ss stable=%dms pause=%d-%dms\n' \
	"$COUNT" "$TIMEOUT_SEC" "$STABLE_MEDIA_MS" "$MIN_PAUSE_MS" "$MAX_PAUSE_MS"
printf 'Artifacts: %s\n' "$OUTPUT_DIR"
if [ "$BURST_PAIRS" -gt 0 ]; then
	printf 'Burst mode: %d pause/play pairs per round, key=%s, added gap=%dms; start playing with the intended control focused.\n' \
		"$BURST_PAIRS" "$BURST_KEY" "$BURST_GAP_MS"
	build_burst_command > "$OUTPUT_DIR/burst-command.sh"
fi

i=1
while [ "$i" -le "$COUNT" ]; do
	start_line=$(( $(wc -l < "$RAW_LOG") + 1 ))
	pause_range=$((MAX_PAUSE_MS - MIN_PAUSE_MS + 1))
	pause_ms=$((MIN_PAUSE_MS + RANDOM % pause_range))
	target="pause=${pause_ms}ms"

	if [ "$BURST_PAIRS" -gt 0 ]; then
		target="burst_pairs=$BURST_PAIRS gap=${BURST_GAP_MS}ms key=$BURST_KEY"
		printf '[%d/%d] %s ... ' "$i" "$COUNT" "$target"
		adb shell "$(cat "$OUTPUT_DIR/burst-command.sh")" > "$OUTPUT_DIR/burst-$i-injection.txt" 2>&1 || fail "burst key injection failed"
	else
		printf '[%d/%d] pause=%dms ... ' "$i" "$COUNT" "$pause_ms"
		adb shell input keyevent "$PAUSE_KEY" >/dev/null 2>&1 || fail "pause key injection failed"
		sleep "$(awk -v ms="$pause_ms" 'BEGIN { printf "%.3f", ms / 1000 }')"
		adb shell input keyevent "$PLAY_KEY" >/dev/null 2>&1 || fail "play key injection failed"
	fi

	start_time=$(date +%s)
	last_health=""
	while :; do
		review_verdict=""
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
		if [ "$healthy" = "1" ]; then
			if [ "$BURST_PAIRS" -gt 0 ]; then
				cp "$LATEST" "$OUTPUT_DIR/burst-$i.log" || fail "could not save burst evidence"
				python3 "$SCRIPT_DIR/analyze_stress_recording.py" "$LATEST" \
					--config "$OUTPUT_DIR/analyzer-config.json" > "$OUTPUT_DIR/burst-$i-review.json"
				review_status=$?
				if [ "$review_status" -ne 0 ] && [ "$review_status" -ne 1 ]; then
					fail "full-burst timing analyzer failed"
				fi
				review_verdict=$(python3 - "$OUTPUT_DIR/burst-$i-review.json" <<'PYEOF'
import json
import sys
verdict = json.load(open(sys.argv[1]))['verdict']
if verdict not in ('NO_ISSUES_OBSERVED', 'INSUFFICIENT_EVIDENCE', 'ISSUES_OBSERVED'):
    raise ValueError('unknown recording verdict: ' + str(verdict))
print(verdict)
PYEOF
) || fail "invalid full-burst timing report"
				last_health="$last_health recording_verdict=$review_verdict"
				case "$review_verdict" in
					ISSUES_OBSERVED)
						capture_failure "burst recovered but full-burst timing review found issues; see burst-$i-review.json" "$i" "$target"
						exit "$FAILURE_RC" ;;
					INSUFFICIENT_EVIDENCE)
						# The live window starts at resume, but render coverage
						# starts at the first submission. Keep collecting the same
						# burst without reinjecting keys or renewing the timeout.
						healthy=0 ;;
				esac
			fi
			if [ "$healthy" = "1" ]; then
				printf 'PASS %s\n' "$last_health" | tee -a "$RESULTS"
				break
			fi
		fi

		now=$(date +%s)
		if [ $((now - start_time)) -ge "$TIMEOUT_SEC" ]; then
			reason="recovery timeout"
			if [ "$review_verdict" = INSUFFICIENT_EVIDENCE ]; then
				reason="full-burst evidence timeout; see burst-$i-review.json"
			fi
			case " $last_health " in
				*" resume=0 "*) reason="resume marker never seen" ;;
				*" writes=0 "*) reason="audio producer did not resume" ;;
				*" video_span=0 "*|*" video_span=0.000 "*) reason="video is frozen after resume" ;;
			esac
			max_write_gap=$(printf '%s\n' "$last_health" | sed -n 's/.*max_write_gap=\([0-9][0-9]*\).*/\1/p')
			if [ -n "$max_write_gap" ] && [ "$max_write_gap" -gt "$WRITE_GAP_MAX_MS" ]; then
				reason="post-resume audio write gap (${max_write_gap}ms)"
			fi
			max_video_gap=$(printf '%s\n' "$last_health" | sed -n 's/.* max_video_gap=\([0-9][0-9]*\).*/\1/p')
			if [ -n "$max_video_gap" ] && [ "$max_video_gap" -gt "$VIDEO_GAP_MAX_MS" ]; then
				reason="post-resume video scheduling gap (${max_video_gap}ms)"
			fi
			underruns=$(printf '%s\n' "$last_health" | sed -n 's/.*underruns=\([0-9][0-9]*\).*/\1/p')
			if [ -n "$underruns" ] && [ "$underruns" -gt "$UNDERRUN_MAX" ]; then
				reason="AudioTrack underruns after resume (${underruns})"
			fi
			capture_failure "$reason; $last_health" "$i" "$target"
			exit "$FAILURE_RC"
		fi
		sleep "$(awk -v ms="$POLL_MS" 'BEGIN { printf "%.3f", ms / 1000 }')"
	done

	i=$((i + 1))
done

printf 'PASS: all %d resume rounds recovered within thresholds. Results: %s\n' "$COUNT" "$RESULTS"

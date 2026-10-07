#!/bin/bash
# Shared capture and configuration for device validation. Source after defaults.

# Nonzero remains mandatory when observations cannot prove continuity. Keep
# this separate from a measured playback fault in driver/campaign summaries.
stress_failure_status()
{
	FAILURE_RC=1
	FAILURE_STATUS=FAIL
	case " $1 " in
		*" verdict=INSUFFICIENT_EVIDENCE "*)
			FAILURE_RC=3
			FAILURE_STATUS=INSUFFICIENT_EVIDENCE ;;
		*" healthy=1 "*" recording_verdict=INSUFFICIENT_EVIDENCE "*)
			FAILURE_RC=3
			FAILURE_STATUS=INSUFFICIENT_EVIDENCE ;;
	esac
}

stress_configure()
{
	command -v python3 >/dev/null 2>&1 || fail 'python3 was not found'
	: "${REQUIRE_AUDIO_PRESENTATION:=1}" "${REQUIRE_RENDER_TIMING:=1}"
	: "${WRITE_GAP_MAX_MS:=250}" "${LATE_DROP_MAX:=0}" "${UNDERRUN_MAX:=0}"
	: "${STARVED_MAX:=0}" "${ARM_UNDERRUN:=0}" "${STABLE_MEDIA_MS:=2000}"
	export PLAYER_PID
	export STABLE_MEDIA_MS AV_DIFF_MAX_MS VIDEO_GAP_MAX_MS WRITE_GAP_MAX_MS
	export REQUIRE_AUDIO_PRESENTATION REQUIRE_RENDER_TIMING LATE_DROP_MAX UNDERRUN_MAX STARVED_MAX ARM_UNDERRUN
	# Export configured shell defaults as well as caller-provided overrides.
	for name in RESUME_LATENCY_MAX_MS SEEK_STARTUP_MAX_MS REBASE_MAX HI_SPEED_MIN WINDOW_GAP_MS HOLD_MIN_MS REQUIRE_FILTER; do
		if declare -p "$name" >/dev/null 2>&1; then export "$name"; fi
	done
	python3 - "$SCRIPT_DIR" <<'PY' || fail 'invalid analyzer configuration'
import sys
sys.path.insert(0, sys.argv[1])
from analyze_stress import config
config()
PY
	LOGCAT_KEEP='mode1_iec_occupancy_shadow:|mode2_occupancy_shadow:|audiotrack_write: wrote|video_sched_diag:|video_render_diag:|audio_present_diag:|playhead_delay:|playhead_streak:|put_time_calc:|late frame drop|AUDIO_STARVED:|AudioTrack underruns:|PCM resume (anchor|slew)|WALLCLOCK_RESET: by pause resume|audio_resume_route:|applying speed filter \[|VIDEO_SEEK_TARGET_READY:|VIDEO_SEEK_DROP|seek watchdog|Fatal signal|FATAL EXCEPTION|ANR in|ERROR_DEAD_OBJECT|AVOS_TEST_POLL|AVOS_TEST_BURST|chatty|dropped [0-9]+ lines|Unexpected EOF|android_sync: (pause start|resume |mode1 )|_stream_play_n_frames|stream_open:|stream_stop:|libavos_set_passthrough: mode=|audiotrack_set_output_params: resolved|compressed (short write|write failed)|continuing unit at|DEAD_OBJECT'
	LOGCAT_KEEP="$LOGCAT_KEEP|PCM resume applies paused correction="
	LOGCAT_KEEP="$LOGCAT_KEEP|SINK_REF_DEFERRED:"
	LOGCAT_KEEP="$LOGCAT_KEEP|at_speed_hw:"
	LOGCAT_KEEP="$LOGCAT_KEEP|android_sync anchor_diag|android_sync: (init render_offset|PCM startup correction)|pcm_startup_correction:|at_ledger:|startup_anchor_commit|audio_start_commit|heard_ts_diag:"
	LOGCAT_ADB_PID=''
	LOGCAT_PIPE=''
	STRESS_POLL_SEQUENCE=0
}

stress_start_logcat()
{
	# Do not clear device logs: the campaign collector spans phase boundaries.
	adb logcat -G 16M >/dev/null 2>&1 || true
	LOGCAT_PIPE="$OUTPUT_DIR/logcat.pipe"
	mkfifo "$LOGCAT_PIPE" || fail 'cannot create logcat pipe'
	adb logcat -T 1 -v epoch avos_player:D avos_test:I AndroidRuntime:E libc:F ActivityManager:E chatty:I '*:S' \
		> "$LOGCAT_PIPE" 2> "$OUTPUT_DIR/logcat-stderr.txt" &
	LOGCAT_ADB_PID=$!
	grep --line-buffered -E "$LOGCAT_KEEP" < "$LOGCAT_PIPE" > "$RAW_LOG" &
	LOGCAT_PID=$!
	sleep 1
	kill -0 "$LOGCAT_ADB_PID" 2>/dev/null && kill -0 "$LOGCAT_PID" 2>/dev/null || fail 'logcat collector did not start'
	stress_preflight
}

stress_snapshot()
{
	# A device-timestamped marker bounds silent tails without comparing host and
	# device clocks. It can overtake queued player logs; it is not a flush barrier.
	# Later snapshots retain late arrivals and the analyzer checks each stream's
	# order separately. Wait for delivery before copying the immutable segment.
	STRESS_POLL_SEQUENCE=$((STRESS_POLL_SEQUENCE + 1))
	poll_marker="AVOS_TEST_POLL_${$}_${STRESS_POLL_SEQUENCE}"
	kill -0 "$LOGCAT_ADB_PID" 2>/dev/null && kill -0 "$LOGCAT_PID" 2>/dev/null || fail 'logcat collector stopped'
	adb shell log -p i -t avos_test "$poll_marker" || fail 'could not write observation barrier'
	for attempt in 1 2 3 4 5 6 7 8 9 10; do
		if grep -q "$poll_marker" "$RAW_LOG"; then
			sed -n "${start_line},/${poll_marker}/p" "$RAW_LOG" > "$LATEST"
			return 0
		fi
		sleep 0.1
	done
	fail 'logcat observation barrier was not delivered (capture incomplete)'
}

stress_stop_logcat()
{
	for pid in "${LOGCAT_PID:-}" "${LOGCAT_ADB_PID:-}"; do
		[ -z "$pid" ] || kill "$pid" 2>/dev/null || true
	done
	wait 2>/dev/null || true
	[ -z "${LOGCAT_PIPE:-}" ] || rm -f "$LOGCAT_PIPE"
	if [ "$ARM_UNDERRUN" = 1 ]; then
		"$SCRIPT_DIR/av.sh" at_underrun 0 >/dev/null 2>&1 || true
	fi
}

# Require live playback and current diagnostic support before injecting keys.
stress_preflight()
{
	start_line=1
	stress_snapshot
	grep -q 'video_sched_diag:' "$LATEST" || fail 'no live playback diagnostics; start a video in a debug build'
	if [ "$REQUIRE_RENDER_TIMING" = 1 ]; then
		grep -q 'video_render_diag:' "$LATEST" || fail 'video_render_diag missing; install the diagnostic build or explicitly disable REQUIRE_RENDER_TIMING'
	fi
	if [ "${STRESS_MODE:-}" = speed ]; then
		if grep -Eq 'audiotrack_write: wrote.*passthrough=[1-9]' "$LATEST"; then
			fail 'speed validation requires decoded PCM; compressed passthrough is active'
		fi
		python3 - "$LATEST" <<'PYEOF' || fail 'speed validation must start at confirmed 1.0x'
import re
import sys
speeds = re.findall(r'put_time_calc:.*? speed=([0-9.]+)', open(sys.argv[1]).read())
raise SystemExit(0 if speeds and abs(float(speeds[-1]) - 1) < .01 else 1)
PYEOF
	fi
	if [ -z "${EXPECTED_PHASE_MS+x}" ] && grep -Eq 'audiotrack_write: wrote.*passthrough=1' "$LATEST"; then
		# Mode 1 owns a preserved static-clock phase. Measure changes from a
		# stable pre-action baseline; this does not calibrate physical lipsync.
		EXPECTED_PHASE_MS=$(python3 - "$LATEST" <<'PYEOF'
import re
import statistics
import sys
values = []
for line in open(sys.argv[1]):
    if 'video_render_diag:' in line:
        fields = dict(re.findall(r'(\w+)=([^\s]+)', line))
        if 0 <= int(fields['anchor_age_ms']) <= 100:
            values.append(int(fields['phase_ms']))
if len(values) < 3 or max(values) - min(values) > 80:
    raise SystemExit('mode-1 phase baseline is missing or unstable; allow playback to settle')
print(statistics.median(values))
PYEOF
) || fail 'could not establish a stable mode-1 phase baseline'
		export EXPECTED_PHASE_MS
		export PHASE_REFERENCE=preflight
	fi
	python3 - "$SCRIPT_DIR" "$OUTPUT_DIR/analyzer-config.json" <<'PYEOF'
import json
import sys
sys.path.insert(0, sys.argv[1])
from analyze_stress import config
with open(sys.argv[2], 'w') as handle:
    json.dump(config(), handle, indent=2, sort_keys=True)
    handle.write('\n')
PYEOF
}

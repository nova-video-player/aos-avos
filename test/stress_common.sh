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
	case "${PCM_PRESENTATION_CAPTURE:-0}" in 0|1) ;; *) fail 'PCM_PRESENTATION_CAPTURE must be 0 or 1' ;; esac
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
	LOGCAT_KEEP="$LOGCAT_KEEP|mode2_dynamic_clock(_enter|_ready|_fallback)?:|mode2_epoch_seed:|android_sync: mode2 "
	LOGCAT_KEEP="$LOGCAT_KEEP|SINK_REF_DEFERRED:"
	LOGCAT_KEEP="$LOGCAT_KEEP|at_speed_hw:"
	LOGCAT_KEEP="$LOGCAT_KEEP|pcm_present_observer:"
	LOGCAT_KEEP="$LOGCAT_KEEP|android_sync anchor_diag|android_sync: (init render_offset|PCM startup correction)|pcm_startup_correction:|at_ledger:|startup_anchor_commit|audio_start_commit|heard_ts_diag:"
	LOGCAT_ADB_PID=''
	LOGCAT_PIPE=''
	STRESS_POLL_SEQUENCE=0
}

stress_pcm_observer_ready()
{
	# A one-off ACK can be lost while periodic observer records still arrive.
	# Compare device log timestamps, not delivery order: queued old samples
	# must not confirm a new phase, and the marker may overtake player logs.
	awk -v marker="$PCM_OBSERVER_MARKER" '
		$1 ~ /^[0-9]+\.[0-9]+$/ {
			if (index($0, marker)) boundary = $1 + 0
			if ($0 ~ /pcm_present_observer: enabled seconds=600([[:space:]]|$)/ ||
			    $0 ~ /audio_present_diag:.* source=pcm_observer([[:space:]]|$)/)
				if ($1 + 0 > evidence) evidence = $1 + 0
		}
		END { exit !(boundary && evidence >= boundary) }
	' "$RAW_LOG"
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
	PCM_PRESENTATION_ARMED=0
	# Optional read-only compositor observer. One per phase also supports the
	# standalone drivers and speed-session wrapper without duplicate collectors.
	SURFACEFLINGER_PID=''
	if [ "${SURFACEFLINGER_CAPTURE:-0}" = 1 ]; then
		python3 "$SCRIPT_DIR/surfaceflinger_timing.py" collect \
			--output "$OUTPUT_DIR/surfaceflinger-samples.jsonl" --package "$PACKAGE" \
			--interval "${SURFACEFLINGER_INTERVAL_SEC:-2}" \
			--layer "${SURFACEFLINGER_LAYER:-}" \
			> "$OUTPUT_DIR/surfaceflinger-collector.log" 2>&1 &
		SURFACEFLINGER_PID=$!
	fi
	sleep 1
	kill -0 "$LOGCAT_ADB_PID" 2>/dev/null && kill -0 "$LOGCAT_PID" 2>/dev/null || fail 'logcat collector did not start'
	if [ "${PCM_PRESENTATION_CAPTURE:-0}" = 1 ]; then
		# Arm after collector startup. The broadcast can return before its log
		# reaches the host, especially on a busy device. Keep this setup wait
		# separate from playback recovery thresholds. Require an ACK or fresh
		# observer output; an adb broadcast success alone is not confirmation.
		# Lease expires even if host cleanup cannot run.
		PCM_OBSERVER_MARKER="AVOS_TEST_POLL_PCM_ARM_${$}"
		adb shell log -p i -t avos_test "$PCM_OBSERVER_MARKER" || \
			fail 'could not mark PCM observer setup in logcat'
		PCM_PRESENTATION_ARMED=1
		"$SCRIPT_DIR/av.sh" at_pcm_observe 600 > "$OUTPUT_DIR/pcm-observer-command.log" 2>&1 || \
			fail 'could not enable PCM presentation diagnostics'
		attempt=0
		while [ "$attempt" -lt 80 ]; do
			kill -0 "$LOGCAT_ADB_PID" 2>/dev/null && kill -0 "$LOGCAT_PID" 2>/dev/null || \
				fail 'logcat collector stopped while waiting for PCM observer acknowledgement'
			stress_pcm_observer_ready && break
			sleep 0.1
			attempt=$((attempt + 1))
		done
		stress_pcm_observer_ready || \
			fail 'PCM observer confirmation not captured within 8s (setup marker and fresh ACK or observer sample required); check logcat delivery and at_pcm_observe support (playback test not started)'
	fi
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
	# Stop before the wait below: the observer intentionally runs until signalled.
	if [ -n "${SURFACEFLINGER_PID:-}" ]; then
		kill "$SURFACEFLINGER_PID" 2>/dev/null || true
		wait "$SURFACEFLINGER_PID" 2>/dev/null || true
		SURFACEFLINGER_PID=''
	fi
	if [ "${PCM_PRESENTATION_ARMED:-0}" = 1 ]; then
		"$SCRIPT_DIR/av.sh" at_pcm_observe 0 >> "$OUTPUT_DIR/pcm-observer-command.log" 2>&1 || true
		PCM_PRESENTATION_ARMED=0
	fi
	for pid in "${LOGCAT_PID:-}" "${LOGCAT_ADB_PID:-}"; do
		[ -z "$pid" ] || kill "$pid" 2>/dev/null || true
	done
	wait 2>/dev/null || true
	[ -z "${LOGCAT_PIPE:-}" ] || rm -f "$LOGCAT_PIPE"
	if [ "${SURFACEFLINGER_CAPTURE:-0}" = 1 ]; then
		python3 "$SCRIPT_DIR/surfaceflinger_timing.py" report \
			--samples "$OUTPUT_DIR/surfaceflinger-samples.jsonl" --log "$RAW_LOG" \
			> "$OUTPUT_DIR/surfaceflinger-analysis.log" 2>&1 || \
			printf 'SurfaceFlinger report unavailable; see %s/surfaceflinger-*.log\n' "$OUTPUT_DIR" >&2
	fi
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

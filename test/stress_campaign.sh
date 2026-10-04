#!/bin/bash
# Resume/seek campaign with optional speed cycles. See doc/TEST.md.
# Captures continuous context and each phase's shared-analyzer verdict.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROUNDS=${1:-${ROUNDS:-2}}
RESUME_CYCLES=${RESUME_CYCLES:-20}
SEEK_CYCLES=${SEEK_CYCLES:-20}
SPEED_CYCLES=${SPEED_CYCLES:-0}
SPEED_HOLD_SEC=${SPEED_HOLD_SEC:-8}
export REQUIRE_RENDER_TIMING=${REQUIRE_RENDER_TIMING:-1}
TIMEOUT_SEC=${TIMEOUT_SEC:-8}
STABLE_MEDIA_MS=${STABLE_MEDIA_MS:-2000}
MIN_PAUSE_MS=${MIN_PAUSE_MS:-500}
MAX_PAUSE_MS=${MAX_PAUSE_MS:-3000}
MIN_PERCENT=${MIN_PERCENT:-5}
MAX_PERCENT=${MAX_PERCENT:-90}
REQUIRE_AUDIO_PRESENTATION=${REQUIRE_AUDIO_PRESENTATION:-1}
ARM_UNDERRUN=${ARM_UNDERRUN:-0}
CAPTURE_BUGREPORT=${CAPTURE_BUGREPORT:-0}
SESSION_LOGCAT=${SESSION_LOGCAT:-1}
SESSION_RAW=${SESSION_RAW:-0}
DRY_RUN=${DRY_RUN:-0}
PACKAGE=${PACKAGE:-org.courville.nova}
STAMP=$(date +%Y%m%d-%H%M%S)
OUTPUT_DIR=${OUTPUT_DIR:-"$REPO_DIR/campaign-session-$STAMP"}

SESSION_LOG="$OUTPUT_DIR/session.log"
SUMMARY_TSV="$OUTPUT_DIR/summary.tsv"
METRICS_TSV="$OUTPUT_DIR/metrics.tsv"
SIGNALS_TSV="$OUTPUT_DIR/session-signals.tsv"
CONTEXT="$OUTPUT_DIR/session-context.txt"
MANIFEST="$OUTPUT_DIR/manifest.txt"
SESSION_LOGCAT_PID=""
SESSION_ADB_PID=""
SESSION_PIPE=""
CAMPAIGN_FAILED=0
CAMPAIGN_RC=0

RESUME_SCRIPT="$SCRIPT_DIR/stress_resume_validate.sh"
SEEK_SCRIPT="$SCRIPT_DIR/stress_seek_validate.sh"
SPEED_SCRIPT="$SCRIPT_DIR/stress_speed_validate.sh"
SESSION_KEEP='mode1_iec_occupancy_shadow:|mode2_occupancy_shadow:|video_render_diag:|audio_present_diag:|playhead_delay:|playhead_streak:|PCM resume slew|AVOS_TEST_POLL|AVOS_TEST_BURST|chatty|dropped [0-9]+ lines|audiotrack_write: wrote|video_sched_diag:|put_time_calc:|late frame drop|AUDIO_STARVED:|AudioTrack underruns:|android_sync: PCM resume anchor|WALLCLOCK_RESET: by pause resume|audio_resume_route:|applying speed filter \[|VIDEO_SEEK_TARGET_READY:|SEEK_VIDEO_DROP|VIDEO_SEEK_DROP|seek watchdog|Fatal signal|FATAL EXCEPTION|ANR in|ERROR_DEAD_OBJECT|android_sync: (pause start|resume |mode1 )|_stream_play_n_frames|stream_open:|stream_stop:|libavos_set_passthrough: mode=|audiotrack_set_output_params: resolved|compressed (short write|write failed)|continuing unit at|Unexpected EOF|DEAD_OBJECT'

SESSION_KEEP="$SESSION_KEEP|PCM resume applies paused correction="
SESSION_KEEP="$SESSION_KEEP|SINK_REF_DEFERRED:"
SESSION_KEEP="$SESSION_KEEP|at_speed_hw:"

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

stop_session_logcat()
{
	for pid in "$SESSION_LOGCAT_PID" "$SESSION_ADB_PID"; do
		[ -n "$pid" ] && kill "$pid" 2>/dev/null
	done
	wait 2>/dev/null || true
	[ -z "$SESSION_PIPE" ] || rm -f "$SESSION_PIPE"
	SESSION_LOGCAT_PID=""
	SESSION_ADB_PID=""
	SESSION_PIPE=""
	return 0
}

cleanup()
{
	stop_session_logcat
}

collect_context()
{
	{
		printf 'collected_at=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
		printf 'script=%s\n' "$0"
		printf 'repo_dir=%s\n' "$REPO_DIR"
		printf 'git_head=%s\n' "$(git -C "$REPO_DIR" rev-parse --short HEAD 2>/dev/null || true)"
		printf 'git_branch=%s\n' "$(git -C "$REPO_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
		printf 'package=%s\n' "$PACKAGE"
		printf 'device_model=%s\n' "$(adb shell getprop ro.product.model 2>/dev/null | tr -d '\r')"
		printf 'device_release=%s\n' "$(adb shell getprop ro.build.version.release 2>/dev/null | tr -d '\r')"
		printf 'device_build=%s\n' "$(adb shell getprop ro.build.display.id 2>/dev/null | tr -d '\r')"
		printf 'app_version=%s\n' "$(adb shell dumpsys package "$PACKAGE" 2>/dev/null | sed -n 's/.*versionName=\([^ ]*\).*/\1/p' | head -n 1 | tr -d '\r')"
		printf '\n[prefs]\n'
		adb shell run-as "$PACKAGE" cat shared_prefs/org.courville.nova_preferences.xml 2>/dev/null | tr -d '\r' || true
	} > "$CONTEXT" 2>&1
}

start_session_logcat()
{
	[ "$SESSION_LOGCAT" = "1" ] || return 0
	adb logcat -G 16M >/dev/null 2>&1 || true
	if [ "$SESSION_RAW" = "1" ]; then
		adb logcat -T 1 -v epoch > "$OUTPUT_DIR/logcat-session.log" 2>&1 &
		SESSION_LOGCAT_PID=$!
	else
		SESSION_PIPE="$OUTPUT_DIR/logcat-session.pipe"
		rm -f "$SESSION_PIPE"
		mkfifo "$SESSION_PIPE" || fail "cannot create session logcat pipe"
		adb logcat -T 1 -v epoch avos_player:D avos_test:I chatty:I AndroidRuntime:E libc:F ActivityManager:E '*:S' \
			> "$SESSION_PIPE" 2> "$OUTPUT_DIR/logcat-session-stderr.txt" &
		SESSION_ADB_PID=$!
		grep --line-buffered -E "$SESSION_KEEP" < "$SESSION_PIPE" \
			> "$OUTPUT_DIR/logcat-session.log" 2>&1 &
		SESSION_LOGCAT_PID=$!
	fi
	sleep 1
	if ! kill -0 "$SESSION_LOGCAT_PID" 2>/dev/null; then
		fail "session logcat collector did not start"
	fi
}

# run_phase <name> <dir> <command...>
run_phase()
{
	phase_name=$1
	phase_dir=$2
	shift 2
	printf '\n=== phase %s ===\n' "$phase_name" | tee -a "$SESSION_LOG"
	printf 'start=%s dir=%s cmd=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$phase_dir" "$*" | tee -a "$SESSION_LOG"
	OUTPUT_DIR="$phase_dir" "$@" >> "$SESSION_LOG" 2>&1
	phase_rc=$?
	printf 'end=%s phase=%s rc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$phase_name" "$phase_rc" | tee -a "$SESSION_LOG"
	if [ "$phase_rc" -ne 0 ]; then
		CAMPAIGN_FAILED=1
		if [ "$phase_rc" -eq 2 ]; then CAMPAIGN_RC=2; elif [ "$CAMPAIGN_RC" -eq 0 ]; then CAMPAIGN_RC=1; fi
	fi
	return "$phase_rc"
}

aggregate_phase()
{
	phase_name=$1
	phase_dir=$2
	phase_rc=$3
	results_file="$phase_dir/results.txt"
	cycles=0
	passed=0
	failed=0
	if [ -f "$results_file" ]; then
		read -r cycles passed failed <<EOF
$(awk 'NF { t++; if ($1 == "PASS") p++; else if ($1 == "FAIL") f++ } END { printf "%d %d %d", t + 0, p + 0, f + 0 }' "$results_file")
EOF
	fi
	case "$phase_name" in
		resume-*) expected=$RESUME_CYCLES ;;
		seek-*) expected=$SEEK_CYCLES ;;
		speed-*) expected=$SPEED_CYCLES ;;
	esac
	if [ "$cycles" -eq "$expected" ] && [ "$passed" -eq "$expected" ] && [ "$failed" -eq 0 ] && [ "$phase_rc" -eq 0 ]; then
		status=PASS
	else
		status=FAIL
		CAMPAIGN_FAILED=1
		[ "$CAMPAIGN_RC" -ne 0 ] || CAMPAIGN_RC=1
	fi
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$phase_name" "$status" "$cycles" "$passed" "$failed" "$phase_rc" "$phase_dir" \
		>> "$SUMMARY_TSV"
	[ -f "$results_file" ] || return 0
	awk -v phase="$phase_name" '
		NF > 1 {
			for (i = 2; i <= NF; i++) {
				split($i, kv, "=")
				k = kv[1]
				v = kv[2]
				if (v ~ /^-?[0-9]+(\.[0-9]+)?$/) {
					if (!(k in seen) || v + 0 > max[k]) { max[k] = v + 0; seen[k] = 1 }
				}
			}
		}
		END { for (k in max) printf "%s\t%s\t%s\n", phase, k, max[k] }
	' "$results_file" | sort >> "$METRICS_TSV"
}

collect_session_signals()
{
	printf 'signal\tcount\n' > "$SIGNALS_TSV"
	[ -f "$OUTPUT_DIR/logcat-session.log" ] || return 0
	awk '
		/late frame drop/ { late++ }
		/AUDIO_STARVED:/ { starved++ }
		/AudioTrack underruns:/ {
			p = index($0, "delta=")
			if (p) {
				tail = substr($0, p + 6)
				neg = (substr(tail, 1, 1) == "-") ? 1 : 0
				sub(/^[+-]/, "", tail)
				sub(/[^0-9].*$/, "", tail)
				if (!neg && tail != "") underrun += tail + 0
			}
		}
		/put_time_calc:/ && /allow_reanchor=1/ { rebase++ }
		/android_sync: PCM resume anchor/ { pcm++ }
		/WALLCLOCK_RESET: by pause resume/ { wall++ }
		/audio_resume_route:/ { route++ }
		/VIDEO_SEEK_TARGET_READY:/ { seek_ready++ }
		/VIDEO_SEEK_DROP/ { seek_drop++ }
		END {
			printf "late_frame_drops\t%d\n", late + 0
			printf "AUDIO_STARVED\t%d\n", starved + 0
			printf "AudioTrack_underruns_delta\t%d\n", underrun + 0
			printf "put_time_calc_reanchors\t%d\n", rebase + 0
			printf "pcm_resume_anchors\t%d\n", pcm + 0
			printf "wallclock_resets\t%d\n", wall + 0
			printf "audio_resume_route\t%d\n", route + 0
			printf "video_seek_target_ready\t%d\n", seek_ready + 0
			printf "seek_video_drop\t%d\n", seek_drop + 0
		}
	' "$OUTPUT_DIR/logcat-session.log" >> "$SIGNALS_TSV"
}

write_summary_json()
{
	if ! command -v python3 >/dev/null 2>&1; then
		printf 'python3 not found; summary.json skipped\n' >> "$SESSION_LOG"
		return 0
	fi
	python3 - "$OUTPUT_DIR" > "$OUTPUT_DIR/summary.json" <<'PYEOF'
import json
import os
import sys

root = sys.argv[1]

def tsv(name):
    rows = []
    path = os.path.join(root, name)
    if not os.path.exists(path):
        return rows
    with open(path) as handle:
        for index, line in enumerate(handle):
            if index == 0:
                continue
            fields = line.rstrip("\n").split("\t")
            if fields and fields[0] != "":
                rows.append(fields)
    return rows

phases = []
for row in tsv("summary.tsv"):
    phases.append({
        "phase": row[0],
        "status": row[1],
        "cycles": int(row[2] or 0),
        "pass": int(row[3] or 0),
        "fail": int(row[4] or 0),
        "rc": int(row[5] or 0),
        "dir": row[6] if len(row) > 6 else "",
    })

metrics = []
for row in tsv("metrics.tsv"):
    if len(row) >= 3:
        metrics.append({"phase": row[0], "metric": row[1], "max": float(row[2])})

signals = {}
for row in tsv("session-signals.tsv"):
    if len(row) >= 2:
        signals[row[0]] = int(row[1])

summary = {
    "session_dir": root,
    "phase_count": len(phases),
    "phases": phases,
    "metrics": metrics,
    "session_signals": signals,
}
summary["physical_lipsync"] = "unmeasured"
summary["verdict"] = "FAIL" if (not phases or any(p["status"] != "PASS" for p in phases)) else "PASS"
print(json.dumps(summary, indent=2, sort_keys=True))
PYEOF
}

write_summary_md()
{
	if [ "$CAMPAIGN_FAILED" -eq 0 ]; then
		verdict=PASS
	else
		verdict=FAIL
	fi
	{
		printf '# Stress campaign summary\n\n'
		printf -- '- Session: `%s`\n' "$OUTPUT_DIR"
		printf -- '- Verdict: **%s**\n' "$verdict"
		printf -- '- Rounds: %s, resume cycles/phase: %s, seek cycles/phase: %s\n' \
			"$ROUNDS" "$RESUME_CYCLES" "$SEEK_CYCLES"
		printf -- '- Generated: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
		printf '\nA PASS means every phase harness reported success against its own\n'
		printf 'thresholds. The phase harnesses own the pass/fail and stutter\n'
		printf 'detection; this campaign only schedules them and aggregates evidence.\n'
		printf '\n## Phases\n\n'
		printf '| phase | status | cycles | pass | fail |\n'
		printf '|---|---|---|---|---|\n'
		awk -F'\t' 'NR > 1 { printf "| %s | %s | %s | %s | %s |\n", $1, $2, $3, $4, $5 }' "$SUMMARY_TSV"
		printf '\n## Maximum stutter proxies per phase\n\n'
		printf '| phase | metric | max |\n'
		printf '|---|---|---|\n'
		awk -F'\t' 'NR > 1 { printf "| %s | %s | %s |\n", $1, $2, $3 }' "$METRICS_TSV"
		printf '\n## Campaign-wide signal counts\n\n'
		printf '| signal | count |\n'
		printf '|---|---|\n'
		awk -F'\t' 'NR > 1 { printf "| %s | %s |\n", $1, $2 }' "$SIGNALS_TSV"
		printf '\n## How to review this session\n\n'
		printf '1. Read `summary.tsv` and `metrics.tsv` first; they are the distilled\n'
		printf '   per-phase verdict and the worst-case metric values.\n'
		printf '2. In each resume, seek or speed phase directory, read `results.txt`\n'
		printf '   (one line per cycle) and `current-*.log` for the segment\n'
		printf '   the harness last analysed. A failure also leaves `failure-*.log`,\n'
		printf '   `audio_flinger.txt`, `audio_policy.txt`, `activity_process.txt`,\n'
		printf '   `surfaceflinger.txt` and `screenshot.png` in the phase directory.\n'
		printf '3. Use `logcat-session.log` for cross-phase context. `session-signals.tsv`\n'
		printf '   counts the signature records over that whole log.\n'
		printf '\nThe shared analyzer checks media progress, observed audio counters, scheduled\n'
		printf 'video deadlines, late submissions and sustained internal A/V phase error.\n'
		printf 'See each phase analyzer-config.json for the exact thresholds.\n'
		printf 'Session signal counts are context: intentional pause/seek intervals are\n'
		printf 'not compared against continuous-playback thresholds.\n'
		printf 'Physical lipsync and displayed pixels are unmeasured; internal phase is\n'
		printf 'not a substitute for a synchronized external audio/video capture.\n'

	} > "$OUTPUT_DIR/summary.md"
}

write_manifest()
{
	{
		printf 'campaign_stamp=%s\n' "$STAMP"
		printf 'argv=%s\n' "$ORIG_ARGS"
		printf 'rounds=%s\n' "$ROUNDS"
		printf 'resume_cycles=%s\n' "$RESUME_CYCLES"
		printf 'seek_cycles=%s speed_cycles=%s speed_hold_sec=%s\n' "$SEEK_CYCLES" "$SPEED_CYCLES" "$SPEED_HOLD_SEC"
		printf 'timeout_sec=%s\n' "$TIMEOUT_SEC"
		printf 'stable_media_ms=%s\n' "$STABLE_MEDIA_MS"
		printf 'min_pause_ms=%s max_pause_ms=%s\n' "$MIN_PAUSE_MS" "$MAX_PAUSE_MS"
		printf 'min_percent=%s max_percent=%s\n' "$MIN_PERCENT" "$MAX_PERCENT"
		printf 'require_audio_presentation=%s\n' "$REQUIRE_AUDIO_PRESENTATION"
		printf 'arm_underrun=%s require_render_timing=%s\n' "$ARM_UNDERRUN" "$REQUIRE_RENDER_TIMING"
		printf 'continue_on_fail=%s\n' "$CONTINUE_ON_FAIL"
		printf 'capture_bugreport=%s\n' "$CAPTURE_BUGREPORT"
		printf 'session_logcat=%s session_raw=%s\n' "$SESSION_LOGCAT" "$SESSION_RAW"
		printf 'package=%s\n' "$PACKAGE"
		printf 'player_pid=%s\n' "$PLAYER_PID"
		printf '\nplan:\n'
		round=1
		while [ "$round" -le "$ROUNDS" ]; do
			printf '  round %s: resume phase -> resume-%s/, then seek phase -> seek-%s/\n' \
				"$round" "$round" "$round"
			if [ "$SPEED_CYCLES" -gt 0 ]; then printf '    speed phase -> speed-%s/\n' "$round"; fi
			round=$((round + 1))
		done
	} > "$MANIFEST"
}

ORIG_ARGS="$*"
CONTINUE_ON_FAIL=${CONTINUE_ON_FAIL:-0}

for number in "$ROUNDS" "$RESUME_CYCLES" "$SEEK_CYCLES" "$TIMEOUT_SEC" \
	"$STABLE_MEDIA_MS" "$MIN_PAUSE_MS" "$MAX_PAUSE_MS" "$MIN_PERCENT" \
	"$MAX_PERCENT" "$SPEED_CYCLES" "$SPEED_HOLD_SEC"; do
	is_uint "$number" || fail "arguments and thresholds must be non-negative integers"
done
[ "$ROUNDS" -gt 0 ] || fail "rounds must be greater than zero"
[ "$RESUME_CYCLES" -gt 0 ] || fail "resume_cycles must be greater than zero"
[ "$SEEK_CYCLES" -gt 0 ] || fail "seek_cycles must be greater than zero"
[ "$SPEED_HOLD_SEC" -gt 0 ] || fail "speed_hold_sec must be greater than zero"
[ "$TIMEOUT_SEC" -gt 0 ] || fail "timeout_sec must be greater than zero"
[ "$STABLE_MEDIA_MS" -gt 0 ] || fail "stable_media_ms must be greater than zero"
[ "$MAX_PAUSE_MS" -ge "$MIN_PAUSE_MS" ] || fail "invalid pause duration range"
[ "$MIN_PERCENT" -ge 0 ] && [ "$MAX_PERCENT" -le 100 ] && \
	[ "$MAX_PERCENT" -gt "$MIN_PERCENT" ] || fail "invalid seek percentage range"
[ "$ARM_UNDERRUN" -eq 0 ] || [ "$ARM_UNDERRUN" -eq 1 ] || fail "ARM_UNDERRUN must be 0 or 1"
[ "$SESSION_LOGCAT" -eq 0 ] || [ "$SESSION_LOGCAT" -eq 1 ] || fail "SESSION_LOGCAT must be 0 or 1"
[ "$SESSION_RAW" -eq 0 ] || [ "$SESSION_RAW" -eq 1 ] || fail "SESSION_RAW must be 0 or 1"
[ "$CONTINUE_ON_FAIL" -eq 0 ] || [ "$CONTINUE_ON_FAIL" -eq 1 ] || \
	fail "CONTINUE_ON_FAIL must be 0 or 1"

for boolean in "$REQUIRE_AUDIO_PRESENTATION" "$REQUIRE_RENDER_TIMING" "$CAPTURE_BUGREPORT" "$DRY_RUN"; do
	case "$boolean" in 0|1) ;; *) fail 'boolean options must be 0 or 1' ;; esac
done

plan()
{
	printf 'Stress campaign plan\n'
	printf '  rounds=%s resume_cycles=%s seek_cycles=%s timeout=%ss stable=%dms\n' \
		"$ROUNDS" "$RESUME_CYCLES" "$SEEK_CYCLES" "$TIMEOUT_SEC" "$STABLE_MEDIA_MS"
	printf '  pause=%s-%sms seek=%s-%s%%\n' \
		"$MIN_PAUSE_MS" "$MAX_PAUSE_MS" "$MIN_PERCENT" "$MAX_PERCENT"
	printf '  require_audio_presentation=%s arm_underrun=%s session_logcat=%s\n' \
		"$REQUIRE_AUDIO_PRESENTATION" "$ARM_UNDERRUN" "$SESSION_LOGCAT"
	printf '  speed_cycles=%s speed_hold=%ss require_render_timing=%s\n' "$SPEED_CYCLES" "$SPEED_HOLD_SEC" "$REQUIRE_RENDER_TIMING"
	printf '  output_dir=%s\n' "$OUTPUT_DIR"
	round=1
	while [ "$round" -le "$ROUNDS" ]; do
		printf '  round %s: %s -> resume-%s/, %s -> seek-%s/\n' \
			"$round" "$(basename "$RESUME_SCRIPT")" "$round" \
			"$(basename "$SEEK_SCRIPT")" "$round"
		if [ "$SPEED_CYCLES" -gt 0 ]; then printf '    speed phase: %s cycles -> speed-%s/\n' "$SPEED_CYCLES" "$round"; fi
		round=$((round + 1))
	done
}

if [ "$DRY_RUN" = "1" ]; then
	plan
	exit 0
fi

command -v adb >/dev/null 2>&1 || fail "adb was not found"
adb get-state >/dev/null 2>&1 || fail "no adb device is ready"
[ -x "$RESUME_SCRIPT" ] || fail "missing $RESUME_SCRIPT"
[ -x "$SEEK_SCRIPT" ] || fail "missing $SEEK_SCRIPT"
[ "$SPEED_CYCLES" -eq 0 ] || [ -x "$SPEED_SCRIPT" ] || fail "missing $SPEED_SCRIPT"
PLAYER_PID=$(adb shell pidof -s "$PACKAGE" 2>/dev/null | tr -d '\r')
[ -n "$PLAYER_PID" ] || fail "Nova is not running; start playback first"

mkdir -p "$OUTPUT_DIR" || fail "cannot create $OUTPUT_DIR"
: > "$SESSION_LOG"
printf 'phase\tstatus\tcycles\tpass\tfail\trc\tdir\n' > "$SUMMARY_TSV"
printf 'phase\tmetric\tmax\n' > "$METRICS_TSV"
trap cleanup EXIT
trap 'exit 130' INT TERM

collect_context
export REQUIRE_AUDIO_PRESENTATION ARM_UNDERRUN CAPTURE_BUGREPORT MIN_PERCENT MAX_PERCENT PACKAGE
adb exec-out screencap -p > "$OUTPUT_DIR/screenshot-start.png" 2>/dev/null || true
start_session_logcat

plan | tee -a "$SESSION_LOG"
printf '\nSession: %s\n\n' "$OUTPUT_DIR"

round=1
while [ "$round" -le "$ROUNDS" ]; do
	run_phase "resume-$round" "$OUTPUT_DIR/resume-$round" \
		"$RESUME_SCRIPT" "$RESUME_CYCLES" "$TIMEOUT_SEC" "$STABLE_MEDIA_MS" \
		"$MIN_PAUSE_MS" "$MAX_PAUSE_MS"
	resume_rc=$?
	aggregate_phase "resume-$round" "$OUTPUT_DIR/resume-$round" "$resume_rc"
	# Preserve the first route baseline across phases/rounds so cumulative drift
	# cannot be hidden by rebasing the test on each phase's starting error.
	if [ -z "${EXPECTED_PHASE_MS+x}" ] && [ -f "$OUTPUT_DIR/resume-$round/analyzer-config.json" ]; then
		EXPECTED_PHASE_MS=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["EXPECTED_PHASE_MS"])' "$OUTPUT_DIR/resume-$round/analyzer-config.json")
		export EXPECTED_PHASE_MS
		export PHASE_REFERENCE=campaign_initial
	fi
	if [ "$resume_rc" -ne 0 ] && [ "$CONTINUE_ON_FAIL" != "1" ]; then
		printf 'Stopping campaign: resume-%s failed (set CONTINUE_ON_FAIL=1 to continue).\n' \
			"$round" | tee -a "$SESSION_LOG"
		break
	fi

	run_phase "seek-$round" "$OUTPUT_DIR/seek-$round" \
		"$SEEK_SCRIPT" "$SEEK_CYCLES" "$TIMEOUT_SEC" "$STABLE_MEDIA_MS"
	seek_rc=$?
	aggregate_phase "seek-$round" "$OUTPUT_DIR/seek-$round" "$seek_rc"
	if [ "$seek_rc" -ne 0 ] && [ "$CONTINUE_ON_FAIL" != "1" ]; then
		printf 'Stopping campaign: seek-%s failed (set CONTINUE_ON_FAIL=1 to continue).\n' \
			"$round" | tee -a "$SESSION_LOG"
		break
	fi
	if [ "$SPEED_CYCLES" -gt 0 ]; then
		run_phase "speed-$round" "$OUTPUT_DIR/speed-$round" \
			"$SPEED_SCRIPT" "$SPEED_CYCLES" "$SPEED_HOLD_SEC"
		speed_rc=$?
		aggregate_phase "speed-$round" "$OUTPUT_DIR/speed-$round" "$speed_rc"
		if [ "$speed_rc" -ne 0 ] && [ "$CONTINUE_ON_FAIL" != 1 ]; then break; fi
	fi
	round=$((round + 1))
done

if [ -n "$SESSION_LOGCAT_PID" ] || [ -n "$SESSION_ADB_PID" ]; then
	stop_session_logcat
fi
adb exec-out screencap -p > "$OUTPUT_DIR/screenshot-end.png" 2>/dev/null || true
collect_session_signals

write_summary_md
write_summary_json
write_manifest

if [ "$CAMPAIGN_FAILED" -eq 0 ]; then
	printf '\nCAMPAIGN PASS: %s\nSummary: %s/summary.md\n' "$OUTPUT_DIR" "$OUTPUT_DIR"
	exit 0
fi
printf '\nCAMPAIGN FAIL: %s\nSummary: %s/summary.md\n' "$OUTPUT_DIR" "$OUTPUT_DIR"
exit "$CAMPAIGN_RC"

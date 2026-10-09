#!/bin/bash

set -euo pipefail

# Save the original directory where script was called
SCRIPT_DIR="$(pwd)"

# Trap Ctrl-C to display log file before exiting
trap 'echo ""; echo "Interrupted. Log file: ${LOG_FILE:-not started}"; exit 130' SIGINT

# Infer PROJECT_DIR from current script location
# This script is in native/avos/, so Video/ is two levels up
SCRIPT_LOCATION="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_LOCATION/../../Video" && pwd)"
# Detect device architecture
echo "📱 Detecting device architecture..."
ABI=$(adb shell getprop ro.product.cpu.abi | tr -d '\r')
if [ -z "$ABI" ]; then
    echo "❌ ERROR: Could not detect device ABI. Is the device connected?"
    exit 5
fi
echo "✅ Device ABI: $ABI"

case "$ABI" in
    arm64-v8a|armeabi-v7a|x86|x86_64) ;;
    *) echo "ERROR: Unsupported device ABI: $ABI"; exit 5 ;;
esac

BUILD_LOG="build.log"

# Find next available log file number
LOG_NUM=0
while [ -f "$SCRIPT_DIR/avos-$(printf "%02d" $LOG_NUM).log" ]; do
    LOG_NUM=$((LOG_NUM + 1))
done
LOG_FILE="$SCRIPT_DIR/avos-$(printf "%02d" $LOG_NUM).log"

echo "========================================="
echo "Nova Video Player - Build & Test Script"
echo "========================================="

# Go to project directory
echo "📁 Changing to project directory: $PROJECT_DIR"
if ! cd "$PROJECT_DIR"; then
    echo "❌ ERROR: Cannot change directory to $PROJECT_DIR"
    exit 1
fi

# Build APK
echo "🔨 Building APK with gradlew assembleNoamazonDebug..."
if ! ./gradlew assembleNoamazonDebug --offline > "$BUILD_LOG" 2>&1; then
    echo "❌ ERROR: Gradle build failed."
    echo "📄 Build log: $PROJECT_DIR/$BUILD_LOG"
    echo ""
    python3 "$SCRIPT_LOCATION/extract_errors.py" "$PROJECT_DIR/$BUILD_LOG" || true
    exit 2
fi
echo "✅ Build completed successfully"

# Select the current build output, not an older version left in the directory.
echo "🔍 Reading Gradle APK metadata..."
if ! APK_FILE=$(python3 - "$PROJECT_DIR/build/outputs/apk/noamazon/debug/output-metadata.json" "$ABI" <<'PY'
import json
from pathlib import Path
import sys

metadata = Path(sys.argv[1])
abi = sys.argv[2]
try:
    data = json.loads(metadata.read_text())
    if data.get('applicationId') != 'org.courville.nova' or data.get('variantName') != 'noamazonDebug':
        raise ValueError('unexpected application or build variant')
    matches = [entry for entry in data['elements']
               if entry.get('filters') == [{'filterType': 'ABI', 'value': abi}]]
    if not matches:
        # A universal build is valid for any of the supported device ABIs.
        matches = [entry for entry in data['elements'] if entry.get('filters') == []]
    if len(matches) != 1:
        raise ValueError(f'expected one APK for {abi}, found {len(matches)}')
    apk = (metadata.parent / matches[0]['outputFile']).resolve()
    if apk.parent != metadata.parent.resolve() or apk.suffix != '.apk' or not apk.is_file():
        raise ValueError(f'missing or invalid APK output: {apk}')
    print(apk)
except (OSError, ValueError, KeyError, TypeError) as error:
    print(f'Cannot select current APK: {error}', file=sys.stderr)
    sys.exit(1)
PY
); then
    echo "❌ ERROR: No unambiguous current APK for $ABI. Nothing installed."
    exit 3
fi
echo "✅ Found APK: $APK_FILE"

# Install APK
echo "📱 Installing APK to device..."
if ! adb install -r "$APK_FILE"; then
    echo "❌ ERROR: Failed to install APK on device."
    exit 4
fi
echo "✅ APK installed successfully"

echo "⏳ Waiting 1 seconds for system to settle..."
sleep 1

# Wake up device from screensaver/sleep
echo "⏰ Waking up device from screensaver..."
#adb shell input keyevent KEYCODE_WAKEUP 2>/dev/null
#sleep 1
# Press HOME to ensure device is active
adb shell input keyevent KEYCODE_HOME 2>/dev/null
sleep 1
echo "✅ Device awake"

adb shell am start -n org.courville.nova/com.archos.mediacenter.video.EntryActivity 2> /dev/null
echo "✅ Launch nova"

# Clear previous logcat buffer
echo "🧹 Clearing logcat buffer... and get logs"
echo "📝 Logging to: $LOG_FILE"
sleep 5
adb logcat -c
PLAYER_PID=$(adb shell pidof -s org.courville.nova | tr -d '\r')
if [[ ! "$PLAYER_PID" =~ ^[0-9]+$ ]]; then
    echo "ERROR: Nova is not running; cannot capture playback logs."
    exit 6
fi
adb logcat --pid="$PLAYER_PID" -v brief | grep --line-buffered avos_player | tee "$LOG_FILE"

exit 0

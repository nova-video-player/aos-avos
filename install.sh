#!/bin/bash

set -e  # Exit on unhandled errors

# Save the original directory where script was called
SCRIPT_DIR="$(pwd)"

# Trap Ctrl-C to display log file before exiting
trap 'echo ""; echo "🛑 Interrupted. Log file: avos-$(printf "%02d" $LOG_NUM).log"; exit 130' SIGINT

# Infer PROJECT_DIR from current script location
# This script is in native/avos/, so Video/ is two levels up
SCRIPT_LOCATION="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_LOCATION/../../Video" && pwd)"
#APK_PATTERN="build/outputs/apk/noamazon/debug/org.courville.nova-*-arm64-v8a-debug.apk"
APK_PATTERN="build/outputs/apk/noamazon/debug/org.courville.nova-*-armeabi-v7a-debug.apk"
BUILD_LOG="build.log"
VIDEO_PATH="file:///sdcard/Download/Silicon_Valley-S05E02-Reorientation-small.mkv"

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
echo "🔨 Building APK with gradlew aND..."
if ! ./gradlew aND --offline > "$BUILD_LOG" 2>&1; then
    echo "❌ ERROR: Gradle build failed."
    echo "📄 Build log: $PROJECT_DIR/$BUILD_LOG"
    echo ""
    cd "$SCRIPT_DIR"
    python3 ./extract_errors.py "$PROJECT_DIR/$BUILD_LOG"
    exit 2
fi
echo "✅ Build completed successfully"

# Find APK
echo "🔍 Searching for APK..."
APK_FILE=$(ls $APK_PATTERN 2>/dev/null | head -n 1)
if [ -z "$APK_FILE" ]; then
    echo "❌ ERROR: APK not found at $APK_PATTERN"
    exit 3
fi
echo "✅ Found APK: $APK_FILE"

# Install APK
echo "📱 Installing APK to device..."
if ! adb install -r "$APK_FILE" 2>&1 | grep -v "^Performing"; then
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
adb logcat -c; adb logcat --pid=$(adb shell pidof -s org.courville.nova) -v brief | grep --line-buffered avos_player | tee $LOG_FILE

exit 0

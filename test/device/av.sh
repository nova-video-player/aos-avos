#!/bin/bash
# Send one debug command; quote the remote shell independently of the host.
set -eu
PACKAGE=${PACKAGE:-org.courville.nova}
case "$PACKAGE" in ''|*[!a-zA-Z0-9_.]*) echo 'Invalid PACKAGE' >&2; exit 2 ;; esac
[ "$#" -gt 0 ] || { echo 'Usage: av.sh command [args...]' >&2; exit 2; }
for arg in "$@"; do
	case "$arg" in ''|*[!a-zA-Z0-9_.+-]*) echo 'Invalid debug command argument' >&2; exit 2 ;; esac
done
adb shell "am broadcast -a com.archos.mediacenter.AVSH -e cmd '$*' '$PACKAGE/com.archos.mediacenter.LibAvosReceiver'" > /dev/null

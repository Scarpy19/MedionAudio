#!/bin/bash
# Initialize the DSP after the OpenCore-injected kext registers.
# Codec routing is handled by the kext and HAL plugin, not by this script.
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
CTL="$DIR/medion_ctl"
LOCK=/tmp/medion_boot_init.lock

if ! mkdir "$LOCK" 2>/dev/null; then
    echo "medion_boot_init: already running"
    exit 0
fi
trap 'rmdir "$LOCK" 2>/dev/null || true' EXIT

for i in $(seq 1 60); do
    if ioreg -r -c MedionAudioDevice 2>/dev/null | grep -q MedionAudioDevice; then
        if "$CTL" init; then
            echo "medion_boot_init: DSP initialized; refreshing CoreAudio"
            killall coreaudiod 2>/dev/null || true
            exit 0
        fi
        echo "medion_boot_init: DSP init attempt $i failed; retrying"
    fi
    sleep 1
done
echo "medion_boot_init: device or DSP not ready after 60 attempts" >&2
exit 1

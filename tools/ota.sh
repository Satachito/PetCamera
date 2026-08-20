#!/usr/bin/env bash
# Push a freshly built firmware to the camera over Wi-Fi.
#
# The device writes the image to its inactive app slot and reboots into it. If
# that image fails to reach the point where it confirms itself, the bootloader
# returns to the previous one on the following boot — so a bad build costs a
# reboot, not a trip to fetch the device and a USB cable.
set -euo pipefail

HOST="${1:-tab5-petcam.local}"
BIN="${2:-build/tab5_petcam.bin}"

if [ ! -f "$BIN" ]; then
    echo "error: $BIN not found — run 'idf.py build' first" >&2
    exit 1
fi

# A failed build leaves the previous image sitting in build/, and uploading that
# looks like a successful deploy of code that was never compiled.
if [ -n "$(find main -newer "$BIN" -name '*.c' -o -newer "$BIN" -name '*.h' 2>/dev/null)" ]; then
    echo "error: sources in main/ are newer than $BIN." >&2
    echo "       The last build did not include them — run 'idf.py build' and check it succeeded." >&2
    exit 1
fi

before=$(curl -s --max-time 10 "http://$HOST/status" |
         python3 -c 'import json,sys;print(json.load(sys.stdin).get("partition","?"))' 2>/dev/null || echo "?")
echo "uploading $(du -h "$BIN" | cut -f1) to $HOST (currently running: $before)"

curl -fsS --max-time 180 -X POST \
     -H 'Content-Type: application/octet-stream' \
     --data-binary "@$BIN" \
     "http://$HOST/update"
echo

echo -n "waiting for it to come back"
for _ in $(seq 1 40); do
    sleep 3
    if now=$(curl -s --max-time 5 "http://$HOST/status" |
             python3 -c 'import json,sys;print(json.load(sys.stdin)["partition"])' 2>/dev/null); then
        echo
        echo "back up, running from $now"
        [ "$now" != "$before" ] && echo "slot changed as expected" || \
            echo "WARNING: same slot as before — the update may have been rolled back"
        exit 0
    fi
    echo -n .
done

echo
echo "error: did not come back within two minutes" >&2
exit 1

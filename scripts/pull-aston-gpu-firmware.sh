#!/usr/bin/env bash
# Reads the OnePlus Ace 3's Adreno 740 ZAP shader from the phone and prints the pin for scripts/data/aston-gpu-inputs.json.
# Read only on the phone: `adb shell ls` and `adb pull` of /vendor/firmware (or /vendor/firmware_mnt/image); nothing is written
# there, no root is needed (the files are world readable on stock builds). Android must be running with USB debugging on.
# The image is written to .cache/firmware/aston-gpu/qcom/sm8550/aston/a740_zap.mbn of this repository (ignored, never committed).
# usage: scripts/pull-aston-gpu-firmware.sh [adb executable]
set -euo pipefail
ADB=${1:-${ADB:-adb}}
DEV=$(cd "$(dirname "$0")/.." && pwd)
OUT="$DEV/.cache/firmware/aston-gpu/qcom/sm8550/aston"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"
"$ADB" get-state >/dev/null
for dir in /vendor/firmware /vendor/firmware_mnt/image; do
  if "$ADB" shell "ls $dir/a740_zap.mbn" >/dev/null 2>&1; then
    "$ADB" pull "$dir/a740_zap.mbn" "$TMP/a740_zap.mbn" >/dev/null
    cp "$TMP/a740_zap.mbn" "$OUT/a740_zap.mbn"
    echo "source $dir/a740_zap.mbn"
    break
  fi
  if "$ADB" shell "ls $dir/a740_zap.mdt" >/dev/null 2>&1; then
    for name in $("$ADB" shell "cd $dir && ls a740_zap.mdt a740_zap.b[0-9][0-9]" | tr -d '\r'); do
      "$ADB" pull "$dir/$name" "$TMP/$name" >/dev/null
    done
    python3 "$DEV/scripts/squash-mdt.py" "$TMP/a740_zap.mdt" "$OUT/a740_zap.mbn"
    echo "source $dir/a740_zap.mdt + $(cd "$TMP" && ls a740_zap.b* | tr '\n' ' ')"
    break
  fi
done
[ -f "$OUT/a740_zap.mbn" ] || { echo "no a740_zap.mbn or a740_zap.mdt under /vendor/firmware or /vendor/firmware_mnt/image" >&2; exit 1; }
bytes=$(wc -c < "$OUT/a740_zap.mbn")
sha=$(sha256sum "$OUT/a740_zap.mbn" | cut -d' ' -f1)
echo "file   $OUT/a740_zap.mbn"
echo "pin    \"bytes\": $bytes, \"sha256\": \"$sha\""

#!/usr/bin/env bash
# Clones the driver checkouts of drivers/submodules.lock at their pinned commits (Linux build host only).
# usage: scripts/fetch-aston-drivers.sh [device-repository]
set -euo pipefail
DEV=$(cd "${1:-$(dirname "$0")/..}" && pwd)
grep -v '^\s*#' "$DEV/drivers/submodules.lock" | while read -r path url commit; do
  [ -n "${path:-}" ] || continue
  dir="$DEV/$path"
  if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)" = "$commit" ]; then
    echo "ok    $path $commit"
    continue
  fi
  rm -rf "$dir"
  git init -q "$dir"
  git -C "$dir" remote add origin "$url"
  git -C "$dir" fetch -q --depth 1 origin "$commit"
  git -C "$dir" checkout -q FETCH_HEAD
  git -C "$dir" submodule update -q --init --recursive --depth 1
  echo "fetch $path $(git -C "$dir" rev-parse HEAD)"
done
# Ath12kWifi's own source fetch needs pattern-mode sparse checkouts (scripts/prefetch-ath12k-sources.py).
python3 "$DEV/scripts/prefetch-ath12k-sources.py" "$DEV/drivers/Ath12kWifi"

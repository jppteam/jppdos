#!/bin/bash
# jpp_deploy.sh — Flash firmware via esptool, then upload app artifacts via JPPD-SMP.
#
# Usage:
#   scripts/jpp_deploy.sh <port> [app_id ...]
#   scripts/jpp_deploy.sh <port> --all
#
# Arguments:
#   port       Serial port (e.g. /dev/ttyUSB0 or /dev/cu.usbserial-...)
#   app_id     One or more app IDs to upload after flashing (build/apps/<app_id>
#              must already exist — run `idf.py build` first). Omit to flash
#              firmware only. Pass --all to upload every app under build/apps/.
#
# Requires: esptool (pip install esptool), pyserial (pip install pyserial),
# and a completed `idf.py build` (reads build/flasher_args.json for the
# bootloader/partition-table/app offsets and flash settings, so it always
# matches the current build instead of hardcoding addresses here).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
FLASHER_ARGS="$BUILD_DIR/flasher_args.json"

if [ $# -lt 1 ]; then
  echo "Usage: $0 <port> [app_id ...] | $0 <port> --all" >&2
  exit 1
fi

PORT="$1"
shift
APP_IDS=("$@")

if [ ! -f "$FLASHER_ARGS" ]; then
  echo "Error: $FLASHER_ARGS not found. Run 'idf.py build' first." >&2
  exit 1
fi

# --- Derive the esptool write_flash command from flasher_args.json ---------
# Keeps offsets/flash settings in sync with whatever idf.py build produced,
# instead of duplicating them (and drifting) in this script.
# (Uses a plain read loop rather than mapfile/readarray for bash 3.2 on macOS.)
FLASH_ARGS=()
while IFS= read -r line; do
  FLASH_ARGS+=("$line")
done < <(python3 - "$FLASHER_ARGS" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    d = json.load(f)
for a in d["write_flash_args"]:
    print(a)
for offset, path in d["flash_files"].items():
    print(offset)
    print(path)
PY
)

# First 6 tokens are the --flash_mode/--flash_size/--flash_freq pairs.
WRITE_FLASH_ARGS=("${FLASH_ARGS[@]:0:6}")
FILE_ARGS=("${FLASH_ARGS[@]:6}")

# Resolve bin paths relative to build/
RESOLVED_FILE_ARGS=()
for ((i = 0; i < ${#FILE_ARGS[@]}; i += 2)); do
  offset="${FILE_ARGS[i]}"
  relpath="${FILE_ARGS[i + 1]}"
  RESOLVED_FILE_ARGS+=("$offset" "$BUILD_DIR/$relpath")
done

echo "== Flashing firmware to $PORT =="
python3 -m esptool \
  --chip esp32c6 \
  -p "$PORT" \
  -b 460800 \
  --before default_reset \
  --after hard_reset \
  write_flash \
  "${WRITE_FLASH_ARGS[@]}" \
  "${RESOLVED_FILE_ARGS[@]}"

echo
echo "== Firmware flashed. Waiting for device to boot =="
sleep 5

if [ ${#APP_IDS[@]} -eq 0 ]; then
  echo "No app_id given — firmware-only flash complete."
  exit 0
fi

if [ "${APP_IDS[0]}" = "--all" ]; then
  APP_IDS=()
  for dir in "$BUILD_DIR"/apps/*/; do
    [ -d "$dir" ] || continue
    APP_IDS+=("$(basename "$dir")")
  done
  if [ ${#APP_IDS[@]} -eq 0 ]; then
    echo "Error: --all given but no directories found under $BUILD_DIR/apps/" >&2
    exit 1
  fi
fi

for app_id in "${APP_IDS[@]}"; do
  echo
  echo "== Uploading app '$app_id' via JPPD-SMP =="
  python3 "$SCRIPT_DIR/jppd_upload.py" "$PORT" "$app_id"
done

echo
echo "All done."

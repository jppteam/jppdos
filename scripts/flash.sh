#!/bin/bash

# Flash and monitor the ESP-IDF firmware from Docker/host tooling.
# Usage: ./scripts/flash.sh [serial-port]
# If no port is provided, idf.py will auto-detect the connected device.

PORT="${1:-}"

if [ -n "$PORT" ]; then
  idf.py -p "$PORT" flash monitor
else
  idf.py flash monitor
fi

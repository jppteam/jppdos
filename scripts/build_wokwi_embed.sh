#!/usr/bin/env bash
# build_wokwi_embed.sh — build a Wokwi-ready merged.bin with one app embedded.
#
# Usage:
#   ./scripts/build_wokwi_embed.sh <path_to_app_dir>
#
# The app directory must contain manifest.json plus pre-compiled .mpy files.
# Outputs build/merged.bin with the app baked into the data_fs SPIFFS partition
# (offset 0x210000).  The app appears in the launcher alongside any SD apps
# and is launched normally when selected.
#
# To revert to a normal build afterwards:
#   docker compose run --rm build idf.py -DJPP_WOKWI_EMBED_APP_PATH="" build
#   (or: docker compose run --rm build idf.py fullclean && ... build)

set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <path_to_app_dir>" >&2
    exit 1
fi

APP_DIR="$(realpath "$1")"
REPO_ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"

if [ ! -f "${APP_DIR}/manifest.json" ]; then
    echo "Error: manifest.json not found in '${APP_DIR}'" >&2
    exit 1
fi

# The repo root is mounted as /project inside the container.
# Translate the host-absolute app path to its in-container equivalent.
REL="${APP_DIR#"${REPO_ROOT}/"}"
if [ "${REL}" = "${APP_DIR}" ]; then
    echo "Error: app dir '${APP_DIR}' is not inside the repo root '${REPO_ROOT}'" >&2
    exit 1
fi
CONTAINER_APP_DIR="/project/${REL}"

echo "Building Wokwi embedded image — app: ${APP_DIR}"
docker compose run --rm build \
    idf.py -DJPP_WOKWI_EMBED_APP_PATH="${CONTAINER_APP_DIR}" build

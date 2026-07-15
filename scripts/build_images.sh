#!/bin/bash
# build_images.sh — Build BOTH firmware images once per firmware version.
#
#   build/       production image     (CONFIG_JPP_LRV_PROVISIONING off)
#   build-prov/  provisioning image   (CONFIG_JPP_LRV_PROVISIONING on)
#
# The production image is the normal `build/` dir, so every existing
# script/workflow keeps working unchanged. The provisioning image is a separate
# build dir that layers sdkconfig.prov on top of sdkconfig.defaults, so its only
# difference is the write-once LRV provisioning path.
#
# scripts/prepare_device.py flashes build-prov/ first (to push the LRV blob and
# upload apps) and then build/ (the shipping image with the write path gone).
#
# Usage:
#   scripts/build_images.sh            # build both (docker, default)
#   scripts/build_images.sh --local    # build both with a local idf.py
#   scripts/build_images.sh --prod     # only the production image
#   scripts/build_images.sh --prov     # only the provisioning image

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

USE_DOCKER=1
DO_PROD=1
DO_PROV=1
for arg in "$@"; do
  case "$arg" in
    --local) USE_DOCKER=0 ;;
    --prod)  DO_PROV=0 ;;
    --prov)  DO_PROD=0 ;;
    *) echo "Unknown option: $arg" >&2; exit 1 ;;
  esac
done

# idf.py runner: docker (default) or local.
run_idf() {
  if [ "$USE_DOCKER" -eq 1 ]; then
    docker compose run --rm build idf.py "$@"
  else
    idf.py "$@"
  fi
}

if [ "$DO_PROD" -eq 1 ]; then
  echo "== Building PRODUCTION image -> build/ =="
  run_idf -B build build
fi

if [ "$DO_PROV" -eq 1 ]; then
  echo "== Building PROVISIONING image -> build-prov/ =="
  # The provisioning image must differ from production ONLY by
  # CONFIG_JPP_LRV_PROVISIONING. ESP-IDF writes ONE sdkconfig at the project
  # root by default and only applies SDKCONFIG_DEFAULTS when that file does not
  # yet exist — so a plain `-B build-prov` reuses the production sdkconfig and
  # silently drops the provisioning flag. Give this build its OWN sdkconfig
  # (SDKCONFIG=build-prov/sdkconfig) generated fresh from
  # sdkconfig.defaults + sdkconfig.prov. Remove any stale one so the fragment
  # is always re-applied.
  rm -f build-prov/sdkconfig
  run_idf -B build-prov \
          -D SDKCONFIG=build-prov/sdkconfig \
          -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.prov" \
          build
fi

echo
echo "Done. Images:"
[ "$DO_PROD" -eq 1 ] && echo "  production   : build/jppdos_idf.bin"
[ "$DO_PROV" -eq 1 ] && echo "  provisioning : build-prov/jppdos_idf.bin"
echo "Provision a fresh unit with: scripts/prepare_device.py --config scripts/mfg.toml"

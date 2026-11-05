#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_BUILD_DIR="${ROOT_DIR}/build-final"
BUILD_DIR="${DEFAULT_BUILD_DIR}"
BUILD_JOBS="${BUILD_JOBS:-8}"

mkdir -p "${BUILD_DIR}"
# shellcheck source=scripts/idf_env.sh
source "${ROOT_DIR}/scripts/idf_env.sh"

if [[ -f "${BUILD_DIR}/build.ninja" ]]; then
    cmake --build "${BUILD_DIR}" -j "${BUILD_JOBS}"
else
    idf.py -C "${ROOT_DIR}" -B "${BUILD_DIR}" build
fi

echo "Firmware: ${BUILD_DIR}/eegoread_firmware.bin"

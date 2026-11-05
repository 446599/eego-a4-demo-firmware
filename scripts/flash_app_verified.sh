#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${1:-/dev/cu.usbmodem1101}"
FLASH_BAUD="2500000"
VERIFY_READBACK="${VERIFY_READBACK:-0}"
BOOT_LOG_SECONDS="${BOOT_LOG_SECONDS:-22}"
BOOT_PORT_WAIT_SECONDS="${BOOT_PORT_WAIT_SECONDS:-15}"
READ_MAC_ATTEMPTS="${READ_MAC_ATTEMPTS:-3}"
DEFAULT_BUILD_DIR="${ROOT_DIR}/build-final"
BUILD_DIR="${DEFAULT_BUILD_DIR}"
IMAGE="${IMAGE:-${BUILD_DIR}/eegoread_firmware.bin}"
LOG_DIR="${ROOT_DIR}/flash_logs"
STAMP="$(date +%Y%m%d-%H%M%S)"
APP0_OFFSET="0x10000"
APP_PARTITION_SIZE=$((0x500000))

mkdir -p "${LOG_DIR}"
# shellcheck source=scripts/idf_env.sh
source "${ROOT_DIR}/scripts/idf_env.sh"

if [[ ! -f "${IMAGE}" ]]; then
    "${ROOT_DIR}/scripts/build_local.sh"
fi

if ! [[ "${READ_MAC_ATTEMPTS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "READ_MAC_ATTEMPTS must be a positive integer, got ${READ_MAC_ATTEMPTS}" >&2
    exit 2
fi
if ! [[ "${BOOT_PORT_WAIT_SECONDS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "BOOT_PORT_WAIT_SECONDS must be a positive integer, got ${BOOT_PORT_WAIT_SECONDS}" >&2
    exit 2
fi

IMAGE_SIZE="$(wc -c < "${IMAGE}" | tr -d '[:space:]')"
if (( IMAGE_SIZE == 0 || IMAGE_SIZE > APP_PARTITION_SIZE )); then
    echo "App image does not fit app0: ${IMAGE_SIZE} bytes (limit ${APP_PARTITION_SIZE})" >&2
    exit 2
fi

IMAGE_INFO_LOG="${LOG_DIR}/${STAMP}-app0-image-info.log"
python -m esptool --chip esp32s3 image-info "${IMAGE}" \
    | tee "${IMAGE_INFO_LOG}"
IMAGE_VERSION="$(awk -F: '/^App version:/ {sub(/^[[:space:]]*/, "", $2); print $2; exit}' "${IMAGE_INFO_LOG}")"
if [[ -z "${IMAGE_VERSION}" ]]; then
    echo "Could not determine App version from ${IMAGE}" >&2
    exit 2
fi
echo "App0 image accepted: ${IMAGE_SIZE} bytes version=${IMAGE_VERSION}"

MAC_LOG="${LOG_DIR}/${STAMP}-read-mac.log"
ACTUAL_MAC=""
for ((attempt = 1; attempt <= READ_MAC_ATTEMPTS; attempt++)); do
    echo "read-mac attempt ${attempt}/${READ_MAC_ATTEMPTS}" | tee -a "${MAC_LOG}"
    if python -m esptool --chip esp32s3 -p "${PORT}" read-mac \
        2>&1 | tee -a "${MAC_LOG}"; then
        ACTUAL_MAC="$(awk '/^MAC:/ {print tolower($2); exit}' "${MAC_LOG}")"
        if [[ -n "${ACTUAL_MAC}" ]]; then
            break
        fi
    fi

    if (( attempt < READ_MAC_ATTEMPTS )); then
        sleep 1
    fi
done
if [[ -z "${ACTUAL_MAC}" ]]; then
    echo "Could not read target MAC after ${READ_MAC_ATTEMPTS} attempts; see ${MAC_LOG}" >&2
    exit 2
fi
echo "Target identified: ${ACTUAL_MAC} (ESP32-S3)"

python -m esptool \
    --chip esp32s3 -p "${PORT}" -b "${FLASH_BAUD}" \
    --before default-reset --after hard-reset \
    write-flash "${APP0_OFFSET}" "${IMAGE}" \
    2>&1 | tee "${LOG_DIR}/${STAMP}-app0-write.log"

if [[ "${VERIFY_READBACK}" == "1" ]]; then
    READBACK="${LOG_DIR}/${STAMP}-app0-readback.bin"
    python -m esptool \
        --chip esp32s3 -p "${PORT}" -b "${FLASH_BAUD}" \
        --before default-reset --after hard-reset \
        read-flash "${APP0_OFFSET}" "${IMAGE_SIZE}" "${READBACK}" \
        2>&1 | tee "${LOG_DIR}/${STAMP}-app0-readback.log"
    shasum -a 256 "${IMAGE}" "${READBACK}"
    cmp "${IMAGE}" "${READBACK}"
    echo "Verified app0 on ${ACTUAL_MAC}: ${IMAGE_SIZE} bytes match exactly."
fi

BOOT_LOG="${LOG_DIR}/${STAMP}-boot.log"
python - "${PORT}" "${BOOT_LOG_SECONDS}" "${IMAGE_VERSION}" "${BOOT_PORT_WAIT_SECONDS}" <<'PY' | tee "${BOOT_LOG}"
import sys
import time

import serial


port = sys.argv[1]
duration = float(sys.argv[2])
expected_version = sys.argv[3]
port_wait_seconds = float(sys.argv[4])
port_deadline = time.monotonic() + port_wait_seconds
while True:
    try:
        device = serial.Serial(
            port, 115200, timeout=0.25, rtscts=False, dsrdtr=False
        )
        break
    except (OSError, serial.SerialException) as error:
        if time.monotonic() >= port_deadline:
            print(
                f"Boot serial port did not recover within {port_wait_seconds:.0f}s: {error}",
                file=sys.stderr,
            )
            raise SystemExit(3)
        time.sleep(0.25)

with device:
    device.reset_input_buffer()
    device.dtr = False
    device.rts = True
    time.sleep(0.12)
    device.rts = False

    deadline = time.monotonic() + duration
    chunks = []
    while time.monotonic() < deadline:
        data = device.read(4096)
        if data:
            chunks.append(data)
        else:
            time.sleep(0.05)

boot_log = b"".join(chunks).decode("utf-8", errors="replace")
print(boot_log, end="" if boot_log.endswith("\n") else "\n")
markers = ("Loaded app from partition at offset 0x10000",
           "EegoRead firmware boot", "firmware built ")
missing = [marker for marker in markers if marker not in boot_log]
version_seen = any(
    line.partition("App version:")[2].strip() == expected_version
    for line in boot_log.splitlines()
    if "App version:" in line
)
if not version_seen:
    missing.append("App version: " + expected_version)
if missing:
    print("Boot verification failed; missing: " + ", ".join(missing),
          file=sys.stderr)
    raise SystemExit(3)
PY
echo "App0 boot log verified on ${ACTUAL_MAC}: version=${IMAGE_VERSION} ${BOOT_LOG}"

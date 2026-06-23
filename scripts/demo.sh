#!/usr/bin/env bash
# Copyright (c) 2026 fiatsoft.com.
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

echo "[demo] ──────────────────────────────────────────────"
echo "[demo] ringbuf demo (self-contained)"
echo "[demo] ──────────────────────────────────────────────"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

MODULE="$ROOT_DIR/build/kernel/ringbuf.ko"
BUILD="$ROOT_DIR/build/user"

RINGBUF_DEV_NAME="${RINGBUF_DEV_NAME:-demo_$$}"
DEV_NAME="${DEV_NAME:-ringbuf_$RINGBUF_DEV_NAME}"
DEV_PATH="/dev/$DEV_NAME"

WRITER="$BUILD/tests/writer_ticks"
READER="$BUILD/tests/reader_mmap"

VERBOSE="${VERBOSE:-0}"

CREATED_DEV=0
MODULE_WAS_LOADED=0
CLEANED=0

cleanup() {
[[ $CLEANED -eq 1 ]] && return
  CLEANED=1

echo "[demo] cleaning up..."

if [[ $CREATED_DEV -eq 1 ]]; then
    echo "[demo] removing device $DEV_PATH"
    echo "${RINGBUF_DEV_NAME}" | sudo tee /sys/class/ringbuf/remove_device >/dev/null || true
fi

if [[ $MODULE_WAS_LOADED -eq 0 ]]; then
    echo "[demo] unloading module"
    sudo rmmod ringbuf || true
fi

}
trap cleanup EXIT INT TERM

wait_for_sysfs() {
local path="/sys/class/ringbuf/add_device"
for i in {1..50}; do
[[ -e "$path" ]] && return 0
sleep 0.05
done
echo "[demo] ❌ sysfs not ready: $path"
exit 1
}

# --------------------------------------------------
# Check binaries
# --------------------------------------------------

if [[ ! -x "$WRITER" || ! -x "$READER" ]]; then
echo "[demo] ❌ Missing test binaries. Run: make"
exit 1
fi

# --------------------------------------------------
# Load module
# --------------------------------------------------

if [[ -d /sys/module/ringbuf ]]; then
    echo "[demo] module already loaded"
else
    echo "[demo] loading module..."
    sudo insmod "$MODULE"
    wait_for_sysfs
fi

# --------------------------------------------------
# Create device
# --------------------------------------------------

if [[ ! -e "$DEV_PATH" ]]; then
echo "[demo] creating device: ${DEV_NAME#ringbuf_}"

  echo "name=${DEV_NAME#ringbuf_} size=4096 writer_policy=overwrite reader_policy=full" | sudo tee /sys/class/ringbuf/add_device

CREATED_DEV=1
sleep 0.2

fi

# --------------------------------------------------
# Permissions
# --------------------------------------------------

if [[ ! -r "$DEV_PATH" || ! -w "$DEV_PATH" ]]; then
sudo chmod 666 "$DEV_PATH" || true
fi

# --------------------------------------------------
# Run
# --------------------------------------------------

echo "[demo] starting..."

if [[ "$VERBOSE" -eq 1 ]]; then
"$WRITER" --dev "$DEV_PATH" &
else
"$WRITER" --dev "$DEV_PATH" >/dev/null 2>&1 &
fi
WRITER_PID=$!

sleep 0.2

"$READER" --dev "$DEV_PATH" || true

kill "$WRITER_PID" 2>/dev/null || true
wait "$WRITER_PID" 2>/dev/null || true

echo "[demo] done."

#!/usr/bin/env bash
# Copyright (c) 2026 fiatsoft.com.
# SPDX-License-Identifier: GPL-2.0-only
set -eu

SCRIPT_BASE="$(basename "$0")"
SCRIPT_BASE="${SCRIPT_BASE%.*}"

RINGBUF_NAME="${RINGBUF_NAME:-test_${SCRIPT_BASE}_$$}"
TEST_NAME="ringbuf_${RINGBUF_NAME}"
BYTES="${BYTES:-$((1024 * 1024))}"

DEV_NAME="${DEV_NAME:-$TEST_NAME}"
DEV_PATH="/dev/${DEV_NAME}"

cleanup() {
  if [ $NEW_DEV = 1 ]; then
    echo "$DEV_NAME" \
        | sudo tee /sys/class/ringbuf/remove_device >/dev/null 2>&1 || true
  fi        
}
trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

echo "[TEST] monotonic producer/consumer stream"
echo "[TEST] using device: $DEV_PATH"

NEW_DEV=0
if [ ! -e "$DEV_PATH" ]; then
    echo "[setup] creating device: $DEV_NAME"
    echo "name=$DEV_NAME" \
        | sudo tee /sys/class/ringbuf/add_device >/dev/null
    sleep 0.1    
    NEW_DEV=1
fi

[ ! -f "$DEV_PATH" ] || fail "device missing: $DEV_PATH"

./build/user/tests/monotonic_stream_consumer --bytes "$BYTES" --dev "$DEV_PATH" &

C_PID=$!

sleep 1

./build/user/tests/monotonic_stream_producer --bytes "$BYTES" --dev "$DEV_PATH"

wait "$C_PID"

echo "[OK] monotonic stream test passed"

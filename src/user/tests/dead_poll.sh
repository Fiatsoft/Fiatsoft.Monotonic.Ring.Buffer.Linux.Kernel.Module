#!/usr/bin/env bash
# Copyright (c) 2026 fiatsoft.com.
# SPDX-License-Identifier: GPL-2.0-only
set -e

SCRIPT_BASE="$(basename "$0")"
SCRIPT_BASE="${SCRIPT_BASE%.*}"
TEST_NAME="${TEST_NAME:-ringbuf_test_${SCRIPT_BASE}_$$}"
: "${DEV:=$TEST_NAME}"
DEV_PATH="/dev/${DEV#/dev/}"

INSERTED_MOD=0
CREATED_DEV=0
cleanup() {
  if [[ $CREATED_DEV -eq 1 ]]; then
    echo ${DEV#ringbuf_} | sudo tee /sys/class/ringbuf/remove_device >/dev/null 2>&1
  fi
  if [[ $INSERTED_MOD -eq 1 ]]; then
    sudo rmmod ringbuf
  fi
}
trap cleanup EXIT INT TERM

echo "[*] setup..."

# load module if needed
if ! lsmod | grep -q '^ringbuf'; then
    echo "[+] loading module"
    sudo insmod build/ringbuf.ko || true
    INSERTED_MOD=1
fi

# create device if missing
if [ ! -e "$DEV_PATH" ]; then
    echo "[+] creating device ${DEV}"
    echo "name=${DEV#ringbuf_} size=4194304 writer_policy=overwrite reader_policy=full" \
        | sudo tee /sys/class/ringbuf/add_device > /dev/null
    CREATED_DEV=1
    sleep 0.2
fi

# for i in $(seq 1 10000); do

    writer_rc=0
    reader_rc=0

    DMESG_START=$(sudo dmesg | wc -l)

    ./build/user/tests/writer_ticks --dev $DEV &
    WRITER_PID=$!
    ./build/user/tests/reader_poll --dev $DEV &
    READER_PID=$!

    sleep 0.5

    echo "$DEV" | sudo tee /sys/class/ringbuf/remove_device > /dev/null

    # pkill writer_ticks
    # pkill reader_poll
    kill "$WRITER_PID" 2>/dev/null || true
    kill "$READER_PID" 2>/dev/null || true

    wait "$WRITER_PID" || writer_rc=$?
    wait "$READER_PID" || reader_rc=$?

    FAIL=0
    echo "writer rc=$writer_rc"
    echo "reader rc=$reader_rc"

    if [[ -e "/dev/$DEV" ]]; then
        echo "[FAIL] device still exists"
        FAIL=1
    fi

    if sudo dmesg | tail -n +"$DMESG_START" | grep -E 'BUG:|WARNING:|Oops:|KASAN:'; then
        echo "[FAIL] kernel emitted warning"
        FAIL=1
    fi

    if [[ $FAIL -eq 0 ]]; then echo "[PASS]"; fi
    exit $FAIL

# done

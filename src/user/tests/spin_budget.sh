#!/usr/bin/env bash
# Copyright (c) 2026 fiatsoft.com.
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

SCRIPT_BASE="$(basename "$0")"
SCRIPT_BASE="${SCRIPT_BASE%.*}"
TEST_NAME="${TEST_NAME:-ringbuf_test_${SCRIPT_BASE}_$$}"
: "${DEV:=$TEST_NAME}"
DEV_PATH="/dev/$DEV"
: "${DURATION:=5}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CH_SPIN_POLICY="$ROOT/build/user/tools/ch_spin_policy"
THROUGHPUT="$ROOT/build/user/tools/throughput_benchmark"
LATENCY="$ROOT/build/user/tools/latency_benchmark"

echo "== Ringbuf Benchmark =="
echo "== setup =="

INSERTED_MOD=0
CREATED_DEV=0

if ! lsmod | grep -q '^ringbuf'; then
    echo "[$SCRIPT_BASE] loading module"
    sudo insmod "$ROOT/build/kernel/ringbuf.ko"
    INSERTED_MOD=1
fi

if [[ ! -c "$DEV_PATH" ]]; then
    echo "[$SCRIPT_BASE] creating device $DEV"
    echo "name=${DEV#ringbuf_} size=4194304 writer_policy=overwrite reader_policy=full" \
        | sudo tee /sys/class/ringbuf/add_device >/dev/null
    CREATED_DEV=1
    sleep 0.2
fi

echo "[$SCRIPT_BASE] using device: $DEV_PATH"
echo

run_test() {
    local mode="$1"
    local budget="$2"

    echo "---- mode=$mode budget=$budget ----"

    sudo "$CH_SPIN_POLICY" --dev "$DEV_PATH" "$mode"
    printf '%s\n' "$budget" | sudo tee /sys/class/ringbuf/spin_budget >/dev/null

    sleep 0.2

    echo "[throughput]"
    "$THROUGHPUT" --dev "$DEV" --duration "$DURATION"

    echo
    echo "[latency]"
    "$LATENCY" --dev "$DEV" --duration "$DURATION"

    echo
}

run_test busy 0
run_test adaptive 1024
run_test adaptive 4096
run_test sleep 0

echo "== cleanup =="

if [[ "$CREATED_DEV" -eq 1 ]]; then
    echo "[$SCRIPT_BASE] removing device"
    echo "${DEV#ringbuf_}" | sudo tee /sys/class/ringbuf/remove_device >/dev/null
fi

if [[ "$INSERTED_MOD" -eq 1 ]]; then
    echo "[$SCRIPT_BASE] unloading module"
    sudo rmmod ringbuf
fi

echo "== done =="

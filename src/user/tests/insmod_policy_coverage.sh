#!/usr/bin/env bash
# Copyright (c) 2026 fiatsoft.com.
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

SCRIPT_BASE="$(basename "$0")"
SCRIPT_BASE="${SCRIPT_BASE%.*}"
RINGBUF_NAME="${RINGBUF_NAME:-test_${SCRIPT_BASE}_$$}"
TEST_NAME="ringbuf_${RINGBUF_NAME}"
DEVICE_NAME="/dev/$TEST_NAME"

while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes)
            AUTO_YES=1
            ;;
        *)
            echo "unknown arg: $1"
            exit 1
            ;;
    esac
    shift
done

if [ "${CI:-0}" = "1" ]; then
    AUTO_YES=1
fi

# Colors
RED="$(printf '\033[31m')"
GREEN="$(printf '\033[32m')"
YELLOW="$(printf '\033[33m')"
RESET="$(printf '\033[0m')"

PASS_COUNT=0
FAIL_COUNT=0

echo "[TEST] Running insmod coverage matrix"

if lsmod | awk '$1=="ringbuf" {found=1} END {exit !found}'; then
    echo "ringbuf module already loaded."

    if [[ "${AUTO_YES:-0}" -eq 1 ]]; then
        ans="Y"
        echo "[auto-yes] unloading existing module"
    else
        printf "unload and run ${SCRIPT_BASE} tests [Y/n] "
        read ans || ans="n"
    fi

    case "$ans" in
        ""|Y|y)
            echo "Unloading existing module..."
            if ! sudo rmmod ringbuf; then
                echo "Failed to unload module. Is it in use?"
                exit 1
            fi
            ;;
        *)
            echo "Aborting."
            exit 1
            ;;
    esac
fi

echo "[TEST] using device: $RINGBUF_NAME"
echo

run_cmd() {
    echo "$@"
    "$@"
}

cleanup() {
    set +e
    [ -n "${CONS_PID:-}" ] && kill "$CONS_PID" 2>/dev/null
    wait "$CONS_PID" 2>/dev/null
    sudo rmmod ringbuf 2>/dev/null
}
trap cleanup EXIT

for r in fresh full; do
    for w in block overwrite drop; do
        for s in busy sleep adaptive; do

            echo
            echo "=== r=$r w=$w s=$s ==="

            if ! run_cmd sudo insmod build/kernel/ringbuf.ko \
                device_names=$RINGBUF_NAME \
                reader_policies=$r \
                writer_policies=$w \
                spin_policies=$s \
                num_devices=1; then
                echo "${RED}[FAIL] insmod failed${RESET}"
                FAIL_COUNT=$((FAIL_COUNT+1))
                continue
            fi

            ./build/user/tests/monotonic_stream_consumer \
                --bytes 128 \
                --dev $DEVICE_NAME &
            CONS_PID=$!

            sleep 0.05

            if ! run_cmd ./build/user/tests/monotonic_stream_producer \
                --bytes 256 \
                --dev $DEVICE_NAME; then
                echo "${RED}[FAIL] producer failed${RESET}"
                FAIL_COUNT=$((FAIL_COUNT+1))
                cleanup
                continue
            fi

            CONS_RC=""

            # Watchdog
            (
                sleep 2
                if kill -0 "$CONS_PID" 2>/dev/null; then
                    echo "${RED}[FAIL] consumer stuck → killing${RESET}"
                    kill "$CONS_PID" 2>/dev/null
                fi
            ) &
            WATCHDOG_PID=$!

            # Parent shell owns the real wait
            if wait "$CONS_PID"; then
                CONS_RC=0
            else
                CONS_RC=$?
            fi

            # Cleanup watchdog
            kill "$WATCHDOG_PID" 2>/dev/null || true
            wait "$WATCHDOG_PID" 2>/dev/null || true

            if [[ -n "$CONS_RC" && "$CONS_RC" -eq 0 ]]; then
                echo "${GREEN}[PASS]${RESET}"
                PASS_COUNT=$((PASS_COUNT+1))
            else
                FAIL_COUNT=$((FAIL_COUNT+1))
            fi

            CONS_PID=""

            if ! sudo rmmod ringbuf 2>/dev/null; then
                echo "${YELLOW}[WARN] rmmod failed, forcing cleanup${RESET}"
                cleanup
            fi
        done
    done
done

echo
echo "========================================"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo "${RED}FAILURES: $FAIL_COUNT${RESET}"
else
    echo "FAILURES: 0"
fi
echo "${GREEN}PASSES:   $PASS_COUNT${RESET}"
echo "========================================"

# Make CI-friendly
[ "$FAIL_COUNT" -eq 0 ]

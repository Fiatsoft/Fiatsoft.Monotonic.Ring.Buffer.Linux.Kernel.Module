#!/usr/bin/env bash
# Copyright (c) 2026 fiatsoft.com.
# SPDX-License-Identifier: GPL-2.0-only

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

PRODUCER="${PRODUCER:-$ROOT/build/user/tests/overwrite_producer}"
CONSUMER="${CONSUMER:-$ROOT/build/user/tests/overwrite_consumer}"
# DRAIN="${DRAIN:-$ROOT/build/user/tests/reader_duration}"
DRAIN="${DRAIN:-true}"


TRIALS="${TRIALS:-20}"
DURATION="${DURATION:-5}"
N_SLEEP="${N_SLEEP:0}"

TMPDIR="${TMPDIR:-$ROOT/build/user/tests/run_and_collect_tmp}"
mkdir -p "$TMPDIR"

cleanup() {
    pkill -P $$ 2>/dev/null || true
}
trap cleanup EXIT INT TERM

pass=0
fail=0

for i in $(seq 1 "$TRIALS"); do
    echo "=== Trial $i/$TRIALS ==="

    # echo "$DRAIN \
    #     --dev $DEV \
    #     --duration 10
    $DRAIN
    
    CONSUMER_OUT="$TMPDIR/consumer.$i.out"
    PRODUCER_OUT="$TMPDIR/producer.$i.out"

    rm -f "$CONSUMER_OUT" "$PRODUCER_OUT"

    echo $CONSUMER \
        --dev "$DEV" \
        --duration "$DURATION" 
    $CONSUMER \
        --dev "$DEV" \
        --duration "$DURATION" \
        >"$CONSUMER_OUT" 2>&1 &

    C_PID=$!

    sleep 0.1

    echo $PRODUCER \
        --dev "$DEV" \
        --duration "$DURATION" \
        --nsleep "$N_SLEEP"
    $PRODUCER \
        --dev "$DEV" \
        --duration "$DURATION" \
        --nsleep "$N_SLEEP" \
        >"$PRODUCER_OUT" 2>&1

    wait "$C_PID"
    RC=$?

    if grep -q "FATAL" "$CONSUMER_OUT"; then
        echo "  ❌ invariant failure"
        fail=$((fail+1))
        continue
    fi

    if grep -q "Device or resource busy" "$CONSUMER_OUT"; then
        echo "  ❌ claim failed"
        fail=$((fail+1))
        continue
    fi

    pass=$((pass+1))
done

echo
echo "====================="
echo "Trials : $TRIALS"
echo "Pass   : $pass"
echo "Fail   : $fail"
echo "====================="
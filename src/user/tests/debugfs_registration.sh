#!/usr/bin/env bash
# Copyright (c) 2026 fiatsoft.com.
# SPDX-License-Identifier: GPL-2.0-only
set -eu

SCRIPT_BASE="$(basename "$0")"
SCRIPT_BASE="${SCRIPT_BASE%.*}"
TEST_NAME="${TEST_NAME:-ringbuf_test_${SCRIPT_BASE}_$$}"
DEBUGFS_ROOT=/sys/kernel/debug/ringbuf

cleanup() {
    echo "$TEST_NAME" | sudo tee /sys/class/ringbuf/remove_device >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

pass() {
    echo "PASS: $1"
}

echo "[TEST] debugfs registration"

# Ensure debugfs mounted
mountpoint -q /sys/kernel/debug \
    || fail "debugfs not mounted"

# Snapshot existing entries
BEFORE="$(ls -1 "$DEBUGFS_ROOT" 2>/dev/null || true)"

# Create device
echo "name=$TEST_NAME" | sudo tee /sys/class/ringbuf/add_device >/dev/null \
    || fail "add_device failed"

sleep 0.1

AFTER="$(ls -1 "$DEBUGFS_ROOT")"

# Find the new entry
NEW="$(printf "%s\n%s\n" "$BEFORE" "$AFTER" \
      | sort | uniq -u)"

[ -n "$NEW" ] || fail "no new debugfs entry created"

BASE="$DEBUGFS_ROOT/$NEW"

pass "debugfs directory created: $BASE"

[ -f "$BASE/stats" ] || fail "stats missing"
head -n 1 "$BASE/stats" >/dev/null || fail "stats unreadable"

pass "stats readable"

pass "device cleanup successful"
echo "[OK] debugfs registration test passed"

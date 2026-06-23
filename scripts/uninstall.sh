#!/usr/bin/env bash
# Copyright (c) 2026 fiatsoft.com.
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

echo "[uninstall] ──────────────────────────────────────────────"
echo "[uninstall] ringbuf uninstall"
echo "[uninstall] ──────────────────────────────────────────────"


if lsmod | grep -q '^ringbuf'; then
    sudo rmmod ringbuf
    echo "[uninstall] module unloaded"
else
    echo "[uninstall] module not loaded"
fi


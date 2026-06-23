#!/usr/bin/env bash
# Copyright (c) 2026 fiatsoft.com.
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

MODULE="$ROOT_DIR/build/kernel/ringbuf.ko"
CONFIG="${1:-}"

echo "[setup] ──────────────────────────────────────────────"
echo "[setup] ringbuf setup"
echo "[setup] ──────────────────────────────────────────────"

# --------------------------------------------------

# Load module

# --------------------------------------------------

if lsmod | grep -q '^ringbuf'; then
echo "[setup] module already loaded"
else
echo "[setup] loading module..."
sudo insmod "$MODULE"
fi

# --------------------------------------------------

# Apply config if provided

# --------------------------------------------------

if [[ -n "$CONFIG" ]]; then
if [[ ! -f "$CONFIG" ]]; then
echo "[setup] ❌ config not found: $CONFIG"
exit 1
fi

echo "[setup] applying config: $CONFIG"
sudo python3 "$ROOT_DIR/tools/hot_configure_ringbuf.py" "$CONFIG"

else
echo "[setup] no config provided"
fi

# --------------------------------------------------

# Check devices

# --------------------------------------------------

if compgen -G "/dev/ringbuf_*" > /dev/null; then
echo "[setup] devices detected:"
ls -1 /dev/ringbuf_*
else
echo "[setup] ⚠️  no devices found"
echo
echo "You can create one manually:"
echo "  echo 'name=test size=4096' | sudo tee /sys/class/ringbuf/add_device"
echo
echo "Or use the helper tool:"
echo "  sudo python3 tools/hot_configure_ringbuf.py tools/ringbuf.example.json"
fi

echo "[setup] done."

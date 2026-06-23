#!/usr/bin/env python3
# Copyright (c) 2026 fiatsoft.com.
# SPDX-License-Identifier: GPL-2.0-only
import json
import sys
import subprocess

cfg = json.load(open(sys.argv[1]))
defaults = cfg.get('defaults', {})
devs = cfg.get('devices', [])

for d in devs:
    name = d.get("name")
    size = d.get("size", defaults.get("size", 4096))
    claim = d.get("reader_claim", defaults.get("reader_claim", "manual"))
    reader_policy = d.get("reader_policy", defaults.get("reader_policy", "default"))
    writer_policy = d.get("writer_policy", defaults.get("writer_policy", "default"))
    device_id = d.get("device_id", defaults.get("device_id", -1))

    cmd = (
        f"name={name} size={size} reader_claim={claim} "
        f"reader_policy={reader_policy} writer_policy={writer_policy} "
        f"device_id={device_id}"
    )

    print(f"→ Adding device: {cmd}")

    result = subprocess.run(
        ["sudo", "tee", "/sys/class/ringbuf/add_device"],
        input=cmd,       # pass string
        text=True,       # tells subprocess to encode automatically
        capture_output=True
    )

    # Output any kernel/sysfs response
    if result.stdout.strip():
        print("stdout:", result.stdout.strip())
    if result.stderr.strip():
        print("stderr:", result.stderr.strip())
    if result.returncode != 0:
        print(f"Command failed (exit {result.returncode})")
        
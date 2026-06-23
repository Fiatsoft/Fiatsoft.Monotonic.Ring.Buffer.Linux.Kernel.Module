#!/usr/bin/env python3
# Copyright (c) 2026 fiatsoft.com.
# SPDX-License-Identifier: GPL-2.0-only
import json
import subprocess
import sys
import os

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <config.json> [--dry-run]")
        sys.exit(1)

    cfg_path = sys.argv[1]
    dry_run = "--dry-run" in sys.argv

    if not os.path.isfile(cfg_path):
        print(f"Error: {cfg_path} does not exist.")
        sys.exit(1)

    with open(cfg_path) as f:
        cfg = json.load(f)

    defaults = cfg.get("defaults", {})
    devices = cfg.get("devices", [])

    if not devices:
        print("Error: No devices defined in JSON.")
        sys.exit(1)

    # Collect module parameters
    num_devices = len(devices)
    names = []
    sizes = []
    reader_claims = []
    reader_policies = []
    writer_policies = []
    device_minors = []

    for d in devices:
        names.append(d.get("name", f"dev{len(names)}"))
        sizes.append(str(d.get("size", 4096)))
        reader_claims.append(d.get("reader_claim", defaults.get("reader_claim", "auto")))
        reader_policies.append(d.get("reader_policy", defaults.get("reader_policy", "fresh")))
        writer_policies.append(d.get("writer_policy", defaults.get("writer_policy", "block")))
        device_minors.append(str(d.get("device_id", "-1")))

    # Build insmod command
    cmd = [
        "sudo", "insmod", "build/ringbuf.ko",
        f"num_devices={num_devices}",
        f"device_names={','.join(names)}",
        f"device_sizes={','.join(sizes)}",
        f"reader_claims={','.join(reader_claims)}",
        f"reader_policies={','.join(reader_policies)}",
        f"writer_policies={','.join(writer_policies)}",
        f"device_minors={','.join(device_minors)}"
    ]

    print("Module insmod command:")
    print(" ".join(cmd))

    if not dry_run:
        print("\nInserting module...")
        subprocess.run(cmd, check=True)
        print("Module inserted successfully.")

if __name__ == "__main__":
    main()

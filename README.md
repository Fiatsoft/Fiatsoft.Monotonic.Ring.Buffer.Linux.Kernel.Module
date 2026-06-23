[![Fiatsoft](docs/Fiatsoft.png)](https://fiatsoft.com)

## **Fiatsoft.Monotonic.Ring.Buffer.Linux.Kernel.Module**

Hosted at https://github.com/Fiatsoft/Fiatsoft.Monotonic.Ring.Buffer.Linux.Kernel.Module

**Project Status**: **Beta release.**  

## Shared Memory-Mapped Ring Buffer  

[![Kernel](https://img.shields.io/badge/kernel-5.10-blue.svg)]()
[![License](https://img.shields.io/badge/license-GPLv2-lightgrey.svg)]()

A low-latency Linux kernel ring-buffer framework for shared-memory IPC. Each device supports one claimed reader and one active writer, plus read-only observational projections through debugfs. It supports mmap-based access, configurable fresh/full reader modes, block/overwrite writer modes, runtime spin-policy mutation, hot add/remove, multiple devices, and a documented ioctl ABI.

---

## What is this?

`Fiatsoft.Monotonic.Ring.Buffer.Linux.Kernel.Module` or `ringbuf` is a configurable Linux kernel module that exposes shared-memory ring buffers as character devices under `/dev`. It is designed for low-latency producer/consumer workloads where ownership, backpressure, and observability need to stay explicit.

Unlike many Linux IPC primitives that hardcode a single contention model or synchronization strategy, `ringbuf` exposes these behaviors as configurable per-device policies.

Each device (at least 64 devices on most systems) exposes one active writer, one claimed reader, read-only observational views and can independently select:

### Reader policies

* `fresh`

  * readers attach at the live producer position
  * optimized for live streams and telemetry

* `full`

  * readers begin from the oldest retained unread region
  * optimized for replay-style consumption

### Writer policies

* `block`

  * producers stall until space becomes available
  * preserves unread data

* `overwrite`

  * unread data may be replaced under pressure
  * prioritizes forward progress and bounded latency

* `drop` *(planned)*

  * producers discard new writes when full
  * preserves existing unread regions

### Spin / wait policies

* `busy`

  * aggressive low-latency spinning
  * minimizes scheduler interaction

* `adaptive`

  * spins briefly before sleeping
  * balances latency and CPU consumption
    * accepts dynamic spin-budget value by run-time 'hot' sysfs value-store

* `sleep`

  * fully scheduler-driven blocking waits
  * minimizes idle CPU utilization

The goal is to provide a reusable shared-memory IPC primitive flexible enough to support:

* market-data pipelines
* telemetry ingestion
* logging systems
* replay systems
* low-jitter producer/consumer workloads
* instrumentation pipelines
* latency-sensitive services

all from one kernel module while preserving explicit ownership, visibility, and backpressure semantics through a stable ioctl ABI.

---

## Architecture overview

Designed primarily for a single claimed-reader, single-writer pipeline, with passive observational readers through debugfs.

```text
                 FIATSOFT.MONOTONIC.RING.BUFFER.LINUX.KERNEL.MODULE                           
                                                                                  
      Primary Writer                ringbuf.ko                Claimed reader      
   ┌─────────────────┐      ┌────────────────────────┐     ┌──────────────────┐   
   │ write()         │      │ circular buffer        │     │ read()           │   
   │ writev()        │      │ head / tail / data     │ ┌───┼ mmap             │   
   │ ioctl(INFO2)    │      │                        │ │   │ poll             │   
   │                 │      │ ─────────────────────  │ │   │ ioctl(CLAIM0)    │   
   │                 │      │                        │ ├───┼ ioctl(ADVANCE1)  │   
   │                 │      │ Devices:               │ │   │ ioctl(INFO2)     │   
   │                 │      │ /dev/                  │ ├───┼ ioctl(CH_SPIN5) ←┼──┐
   │                 │ ─────┼─→   ringbuf_msft     ←─┼─┘   │  Spin policies:  │  │
   │                 │      │     ringbuf_ibm        │     │   * busy         │  │
   │                 │      │     ringbuf_orcl       │     │   * adaptive     │  │
   │                 │      │ [  up to 64+ total   ] │     │   * sleep        │  │
   │                 │      │                        │     │                  │  │
   └─────────────────┘      │                        │     └──────────────────┘  │
                            │ ────────────────────── │                           │
                            │                        │                           │
                            │ DebugFS: /sys/kernel/  │     Observational readers │
                            │ debug/ringbuf/         │     ┌──────────────────┐  │
                            │   ringbuf_msft/tap   ←─┼─────┼ cat tap > log0   │  │
                            │   ringbuf_msft/dump  ←─┼────┬┼ hexdump -C       │  │
                            │   ringbuf_msft/info    │    └┼ dd │ nc          │  │
                            │   ringbuf_msft/stats ←─┼───┐ └──────────────────┘  │
                            │                        │   │                       │
                            │                        │   │ Performance monitors  │
                            │                        │   │ ┌──────────────────┐  │
                            │                        │   └─┼ cat stats │ _HA  ┼──┘
                            │                        │     └──────────────────┘   
                            └────────────────────────┘                            
  └────────────────────┘   └──────────────────────────┘   └────────────────────┘  
       USER-SPACE                  KERNEL-SPACE                  USER-SPACE       
└─────────────────────────────────────────────────────────────────────────────────┘
                                   LINUX v5.10+                                   
```

### Key features

* Shared-memory ring buffers exposed as `/dev/ringbuf_*` character devices
* `mmap()` support for user-space access to device-backed memory
* Exclusive claimed-reader ownership
* Exclusive active-writer ownership
* Read-only debugfs observers
* Reader modes: `fresh` and `full`
* Writer modes: `block`, `overwrite`, and planned `drop`
* Spin modes: `busy`, `adaptive`, and `sleep`
* Runtime spin-policy mutation through ioctl
* Hot add/remove of devices through sysfs
* Multiple independent devices per module instance
* Debugfs projections for observability and testing
* User-space validation, stress, and benchmark tools

### Limitations

* One claimed reader per device
* One active writer per device
* Additional writer opens fail with `-EBUSY`
* Observational debugfs projections are for inspection, not replay guarantees
* 64-bit logical positions make wraparound impractical under normal deployment lifetimes, provided capacity remains below the signed half-range invariant used by the code
* Build/test scripts currently assume a GNU/Linux environment with Bash and sudo.

---

## Quick start

For a minimal end-to-end demo:

```bash
chmod +x scripts/demo.sh
./scripts/demo.sh
```

For interactive configuration and provisioning:

```bash
sudo ./scripts/setup.sh
```

For scripted configuration:

```bash
python3 src/user/tools/hot_configure_ringbuf.py --help
```

---

## Build

```bash
make
```

Building requires the Linux kernel headers/development package for the
currently running kernel, along with a C toolchain.

### Ubuntu / Debian

```bash
sudo apt update

sudo apt install \
    build-essential \
    linux-headers-$(uname -r)
```

### Fedora

```bash
sudo dnf update
sudo reboot

sudo dnf install \
    gcc \
    make \
    kernel-devel \
    kernel-headers
```

### Arch Linux

```bash
sudo pacman -Syu

sudo pacman -S \
    base-devel \
    linux-headers
```

Verify the kernel build tree exists:

```bash
ls -ld /lib/modules/$(uname -r)/build
```

Then build:

```bash
make
```

Build output lives under `build/`, including the kernel module and user-space tools/tests.

---

## Loading the module

Devices can be provisioned either at module insertion time or dynamically after load.
 
### Quick demo

For a minimal end-to-end demonstration:

```bash
chmod +x scripts/demo.sh
./scripts/demo.sh
```

### Setup script

Run:

```bash
sudo ./scripts/setup.sh
```

or pass a JSON configuration file:

```bash
sudo ./scripts/setup.sh tools/ringbuf.example.json
```

The setup script:

* loads the module
* applies optional device configuration
* shows any created devices

### Module parameters

The module supports defaults and per-device arrays for initial configuration. Unless otherwise noted, list-valued parameters are comma-separated and must contain exactly one entry per device. `device_sizes` are byte counts; the module rounds each size to the next supported power of two and may clamp them to implementation limits. Policy values are expressed as readable names in module parameters and user tools. Debug output may also show the corresponding numeric ABI values

| Parameter               | Type / form              | Accepted values                         | Notes                                                                      |
| ----------------------- | ------------------------ | --------------------------------------- | -------------------------------------------------------------------------- |
| `device_names`          | comma-separated strings  | Any valid device names                  | One name per device. Names become `/dev/ringbuf_<name>`.                   |
| `device_sizes`          | comma-separated integers | Positive byte sizes                     | One size per device. Rounded internally as needed.                         |
| `device_minors`         | comma-separated integers | Valid minor numbers                     | Optional explicit minor assignment.                                        |
| `reader_policies`       | comma-separated strings  | `fresh`, `full`                         | One policy per device.                                                     |
| `writer_policies`       | comma-separated strings  | `block`, `overwrite`, `drop`            | One policy per device.                                                     |
| `spin_policies`         | comma-separated strings  | `busy`, `adaptive`, `sleep`             | One policy per device.                                                     |
| `device_nodes`          | comma-separated integers | NUMA node indexes, or `-1` if supported | One NUMA node value per device.                                            |
| `num_devices`           | integer                  | `1` or greater                          | Number of devices to create from the parameter arrays. 
| `default_reader_policy` | string                   | `fresh`, `full`                         | Default reader mode for devices created without an explicit reader policy. |
| `default_writer_policy` | string                   | `block`, `overwrite`, `drop`            | Default writer mode.                                                       |
| `default_spin_policy`   | string                   | `busy`, `adaptive`, `sleep`             | Default blocking/wait policy.                                              |

#### Reader policies

| Policy  | Meaning                                     |
| ------- | ------------------------------------------- |
| `full`  | Reader sees entire unread window            |
| `fresh` | Reader is advanced to newest data on attach |

#### Writer policies

| Policy      | Meaning                         |
| ----------- | ------------------------------- |
| `block`     | Writers wait for free space     |
| `overwrite` | Old unread data may be replaced |

#### Spin policies

| Policy     | Meaning                |
| ---------- | ---------------------- |
| `busy`     | Busy-spin continuously |
| `adaptive` | Busy-spin, then sleep  |
| `sleep`    | Sleep immediately      |

#### Examples

```bash
sudo insmod build/kernel/ringbuf.ko
```

```bash
sudo insmod build/kernel/ringbuf.ko \
  device_names=msft,ibm \
  device_sizes=4194304,4096 \
  reader_policies=full,full \
  writer_policies=overwrite,block \
  spin_policies=sleep,adaptive \
  device_minors=59,60
```

---

 ## Runtime Interfaces

`ringbuf` exposes three runtime surfaces:

| Surface            | Path                        | Purpose                                      |
| ------------------ | --------------------------- | -------------------------------------------- |
| Device path    | `/dev/ringbuf_*`            | Read/write access to each ring buffer device |
| Control path       | `/sys/class/ringbuf`        | Module-level and device-level configuration  |
| Observability path | `/sys/kernel/debug/ringbuf` | Read-only inspection and validation views    |

Only one writer may hold the device open at a time. Additional write opens fail with `-EBUSY`

### Device Interface

| ioctl         | Purpose                    |
| ------------- | -------------------------- |
| `CLAIM`       | Acquire reader ownership   |
| `RELEASE`     | Release reader ownership   |
| `INFO`        | Query runtime state        |
| `ADVANCE`     | Advance reader progression |
| `SPIN_POLICY` | Change device spin policy  |
| `FLUSH`       | Flush device contents      |

See general examples [below](#examples).

#### Runtime policy mutation

Spin policy may be changed at runtime by the claimed reader.

Policy changes affect future waits only; threads already blocked
continue using the policy active when the wait began.

Adaptive spinning is governed by the module-wide `spin_budget`
sysfs setting.

### Control Interface (`/sys/class/ringbuf`)

This class directory contains module-wide controls and per-device metadata.

| Entry                              | Scope       | Purpose                                       |
| ---------------------------------- | ----------- | --------------------------------------------- |
| `add_device`                       | module-wide | Create a new device dynamically               |
| `remove_device`                    | module-wide | Remove an existing device                     |
| `default_spin_policy`              | module-wide | Default spin policy for newly created devices |
| `spin_budget`                      | module-wide | Adaptive spin duration threshold              |
| `major`                            | module-wide | Major number assigned to the device class     |
| `ringbuf_<name>/dev`               | per-device  | Major/minor for the device node               |
| `ringbuf_<name>/device_alloc_node` | per-device  | NUMA allocation node for that device          |
| `ringbuf_<name>/last_add_message`  | per-device  | Most recent device-allocation note            |
| `ringbuf_<name>/power`             | per-device  | Standard device power-management hook         |
| `ringbuf_<name>/subsystem`         | per-device  | Standard sysfs linkage                        |
| `ringbuf_<name>/uevent`            | per-device  | Standard hotplug/event interface              |

#### Examples

**Hot Device Creation**: `echo 'name=test size=4096' | sudo tee /sys/class/ringbuf/add_device`  
**Graceful Device Destruction**: `echo 'test' | sudo tee /sys/class/ringbuf/remove_device`

### Observability Interface (`/sys/kernel/debug/ringbuf`)

These debugfs entries are read-only observational views.

| Entry   | Purpose                                                     |
| ------- | ----------------------------------------------------------- |
| `tap`   | Snapshot of the unread window visible to the claimed reader |
| `dump`  | Full raw backing-memory snapshot for inspection             |
| `info`  | Device state and policy summary                             |
| `stats` | Runtime counters and throughput/latency-related metrics     |

---

## Examples

See [above](#device-creation--destruction) for examples to create devices.

### Lossless transfer

Note use of block writer-policy.

```bash
sudo insmod build/kernel/ringbuf.ko \
  device_names=device0 \
  reader_policies=fresh \
  writer_policies=block

./build/user/tests/writer_ticks --dev /dev/ringbuf_device0 & #background writer
./build/user/tests/reader_mmap --dev /dev/ringbuf_device0
```

Sample output:

```text
TICK-42 PRICE=101.23
TICK-43 PRICE=101.24
...
```

### Claim a reader

```c
#include <sys/ioctl.h>
#include "ringbuf.h"

int fd = open("/dev/ringbuf_msft", O_RDONLY);

if (ioctl(fd, RBUF_IOC_CLAIM) != 0) {
    perror("CLAIM");
    return 1;
}
```

### Manual backpressure control

```c
ioctl(fd, RBUF_IOC_CLAIM);
ioctl(fd, RBUF_IOC_INFO, &info);
write(fd, payload, len);
ioctl(fd, RBUF_IOC_RELEASE);
```

And in your writer:

```c
ioctl(fd, RBUF_IOC_INFO, &info);
uint64_t free_space = info.cap - (info.head - info.tail);
if (free_space < MIN_THRESHOLD) {
    /* Handle backpressure manually:
       skip frames, compress data, or throttle producer. */
}
write(fd, payload, len);
```

### Change spin policy at runtime

```c
int policy = RBUF_SPIN_ADAPTIVE;

if (ioctl(fd, RBUF_IOC_SPIN_POLICY, &policy) != 0) {
    perror("SPIN_POLICY");
}
```

Available policies: see [above](#spin-policies). 

Alternatively, use `ch_spin_policy` utility:
```
./build/user/tools/ch_spin_policy \
    --dev /dev/ringbuf_msft \
    --policy adaptive
```

### Change spin budget

```bash
echo "msft 5000" | sudo tee /sys/class/ringbuf/spin_budget
```

The present `/sys/class/ringbuf/spin_budget` is the only means to influence the spin-budgeting for a device, which is global/module-wide; a per device `ring_dev.spin_budget` is planned.

### Query device state

```c
struct rbuf_info info;

if (ioctl(fd, RBUF_IOC_INFO, &info) == 0) {
    printf("cap=%llu\n",
           (unsigned long long)info.cap);
}
```

---

## Unload the module

Run:

```bash
sudo ./scripts/uninstall.sh
```

or unload manually:

```bash
sudo rmmod ringbuf
```

Confirm cleanup:

```bash
dmesg | tail -n 10
```

---

## Benchmark notes

Preliminary measurements on development hardware indicate microsecond-scale end-to-end message latency with low scheduler interaction in mmap-based configurations.

Benchmark tooling is included under `build/user/tools/`.

Dedicated bare-metal benchmarking remains planned as a final validation step.

---

## Design notes

`ringbuf` is a low-latency shared-memory IPC primitive with explicit ownership and visibility rules.

### Reader model

Each device supports one exclusive claimed reader.

The claimed reader owns consumption semantics:

* advancing the tail
* visibility progression
* policy control operations

Additional processes may inspect the device through debugfs projections (`tap`, `dump`) without affecting device state.

Observational readers are intentionally non-authoritative and do not participate in synchronization or backpressure.

#### Reader classes

Ringbuf distinguishes between:

* claimed readers
* observational readers

A claimed reader owns consumption semantics and advances the shared tail.

Observational readers may inspect device state without affecting producer/consumer progress.

This separation allows live diagnostics and traffic inspection without interfering with the primary data path.

---

### Writer model

Writers append data using standard file-descriptor I/O.

When the ring is full, behavior is policy-driven:

* block
* overwrite
* busy / adaptive / sleep waiting policy when applicable

This keeps backpressure explicit instead of hidden inside the kernel.

---

## Technical notes

Assuming a device `ringbuf_device0`:

* only one reader may hold an active claim at a time
* other processes can inspect `/sys/kernel/debug/ringbuf/ringbuf_device0/tap` and `dump`
* the reader claim is released automatically on `close()` or process exit
* writers block or overwrite depending on `writer_policy`
* spin policy is configured per device and can be changed by the claimed reader when the device is idle

For deeper details, see [further reading](#further-reading).

---

## Testing

The test suite is indexed in `src/user/tests/README.md`.

Tests cover:

* functional read/write paths
* monotonic and overwrite behavior
* poll and mmap behavior
* debugfs projections
* device lifecycle and teardown
* policy coverage
* latency and throughput measurement

## Project status

**Beta release**. The core kernel module, ioctl ABI, mmap path, policy framework, and validation suite are working across multiple Linux distributions. Some benchmark polish and extended portability work remain in progress.

### Validation

| Distribution          | Kernel | Build | Demo | Validation |
| --------------------- | ------ | ----- | ---- | ---------- |
| Ubuntu 24.04.4        | 6.8.x  | PASS  | PASS | PASS       |
| Xubuntu 24.04.2       | 6.8.x  | PASS  | PASS | PASS       |
| Debian 13.5           | 6.12.x | PASS  | PASS | PASS       |
| Fedora 44             | 6.19.x | PASS  | PASS | PASS       |
| EndeavourOS live ISO  |  6.15+ | PASS  | PASS | PASS       |

#### Validation Coverage

| Validation Area              | Included? |
| ---------------------------- | --------- |
| Module build                 | Yes       |
| Module load/unload           | Yes       |
| Dynamic device creation      | Yes       |
| Dynamic device removal       | Yes       |
| Reader claim semantics       | Yes       |
| mmap readers                 | Yes       |
| Blocking writers             | Yes       |
| Overwrite writers            | Yes       |
| Runtime spin-policy mutation | Yes       |
| DebugFS projections          | Yes       |
| Poll/select integration      | Yes       |
| Latency benchmark tooling    | Yes       |
| Throughput benchmark tooling | Yes       |

---

## Troubleshooting

| Symptom                                      | Likely cause                                                       | Fix                                                            |
| -------------------------------------------- | ------------------------------------------------------------------ | -------------------------------------------------------------- |
| `open: No such device or address`            | Device was not created or was removed                              | Check `dmesg` and `/dev/ringbuf_*`                             |
| `ioctl(CLAIM): Device or resource busy`      | Another reader already owns the claim                              | Release the existing claim or stop the owning process          |
| `Permission denied` on read/ioctl            | Claim-required path or wrong file mode                             | Open the device with the correct mode and claim it if required |
| `/sys/kernel/debug/ringbuf/...` missing      | debugfs not mounted or module not initialized                      | Mount debugfs and check module logs                            |
| Unexpected wraparound or projection mismatch | Comparing logical and physical snapshots as if they were identical | Use the documented debugfs semantics                           |

---

## Roadmap

Planned work includes:

* additional benchmark runs on dedicated hardware
* drop-on-full semantics
* per-device adaptive spin budgets
* backpressure metrics
* watermark wakeups
* batched reads/writes
* runtime reader/writer policy reconfiguration
* additional contention and NUMA validation
* more observability tools
* deeper documentation of concurrency invariants and ABI details

---

## Further reading

* [`ABI.md`](docs/ABI.md) — ioctl/sysfs/debugfs contracts
* [`CONCURRENCY.md`](docs/CONCURRENCY.md) — memory ordering and guarantees
* [`src/user/README.md`](src/user/README.md) — test and tool index

---

## License

GPL v2 License — see [LICENSE.md](LICENSE.md) for licensing information.

Copyright (c) 2026 `fiatsoft.com.`.

Author contact: See [GitHub profile](https://github.com/Fiatsoft)
# ABI surface

Ringbuf exposes three primary user-visible interfaces:

* character-device operations under `/dev/ringbuf_*`
* sysfs management and provisioning
* debugfs observational projections

The ABI is intentionally small and explicit.

---

# Device interface

Devices are exposed as character devices:

```text
/dev/ringbuf_<name>
```

Supported operations include:

* `open()`
* `close()`
* `read()`
* `write()`
* `writev()`
* `poll()`
* `mmap()`
* `ioctl()`

The module is designed around mmap-assisted shared-memory access with standard file-descriptor semantics.

---

# Reader ownership model

Each device permits one exclusive claimed reader.

The claimed reader owns:

* tail advancement
* consumption semantics
* policy control operations

Claim ownership is automatically released when the owning file descriptor is closed or the process exits.

Concurrent observational readers may inspect projections without participating in synchronization.

# Writer ownership model

Each device permits one exclusive active writer.

The active writer is acquired implicitly on `open(O_WRONLY)` and released on `close()` or process exit.

Additional write opens fail with `-EBUSY`.

The write path assumes a single owner; concurrent writer correctness is not supported in this release.

---

# IOCTL interface

The ioctl ABI is intentionally narrow.

Representative operations include:

| ioctl                  | Purpose                            |
| ---------------------- | ---------------------------------- |
| `RBUF_IOC_CLAIM`       | Acquire exclusive reader ownership |
| `RBUF_IOC_ADVANCE`     | Advance reader progression         |
| `RBUF_IOC_INFO`        | Query runtime state                |
| `RBUF_IOC_SPIN_POLICY` | Change per-device spin policy      |

The claimed reader exclusively owns progression operations.

Per-device spin policy modification is permitted only while the device is idle.

---

# Sysfs interface

Sysfs is used for:

* module-level configuration
* runtime device creation/removal
* default policy management

Representative entries:

```text
/sys/class/ringbuf/
```

Examples:

| Entry                 | Purpose                    |
| --------------------- | -------------------------- |
| `add_device`          | Create device dynamically  |
| `remove_device`       | Remove device gracefully   |
| `default_spin_policy` | Global default spin policy |
| `spin_budget`         | Adaptive spin threshold    |

Device provisioning occurs either:

* at module insertion time through module parameters
* dynamically through sysfs

---

# DebugFS interface

DebugFS projections are observational only.

Representative entries:

```text
/sys/kernel/debug/ringbuf/ringbuf_<name>/
```

| Projection | Semantics                       |
| ---------- | ------------------------------- |
| `tap`      | Snapshot of unread visible data |
| `dump`     | Snapshot of backing memory      |
| `info`     | Runtime device metadata         |
| `stats`    | Counters and diagnostic state   |

These projections are intended for:

* diagnostics
* validation
* testing
* observability

They are not synchronization primitives.

`dump` is not guaranteed to preserve logical ordering after wrap.

---

# Stability notes

The ABI is intentionally conservative.

Major behavioral guarantees are documented before optimization-oriented changes.

Future revisions may extend:

* observability tooling
* benchmark interfaces
* optional queue semantics

without changing the core ownership model.

# Test and tool index

The project includes:

- functional validation tools
- stream integrity tests
- overwrite stress tests
- concurrency stress tests
- observability checks
- benchmarking utilities

Build output is generated under:

```text
build/user/tests/
build/user/tools/
````

---

# Functional validation

| Test                | Purpose                               |
| ------------------- | ------------------------------------- |
| `reader_basic.c`    | Basic read-path validation            |
| `reader_manual.c`   | mmap/manual advancement reader        |
| `reader_poll.c`     | poll-driven read validation           |
| `reader_duration.c` | Duration-oriented liveness validation |
| `writer_ticks.c`    | Tick-stream producer                  |
| `emit_bytes.c`      | Pipe/stdin byte emission utility      |
| `emit_stdin.c`      | Interactive stdin writer              |

---

# Stream protocol tests

These tests operate on ordered producer streams and validate:

* ordering
* continuity
* overwrite recovery
* mmap visibility
* gap detection

## Binary monotonic stream protocol

The monotonic stream tests use naturally aligned little-endian
`uint64_t` sequence counters.

These tests assume:

* aligned 8-byte record boundaries
* complete `uint64_t` observations
* monotonically increasing producer values

The protocol is intentionally lightweight and optimized for stress
testing rather than portable framed transport semantics.

Under overwrite pressure, consumers may observe discontinuities or
require stream resynchronization.

| Test                          | Purpose                                |
| ----------------------------- | -------------------------------------- |
| `monotonic_stream_producer.c` | Generate monotonic uint64_t stream     |
| `monotonic_stream_consumer.c` | Validate ordering and gap detection    |
| `monotonic_stream.sh`         | Automated monotonic validation harness |
| `overwrite_producer.c`        | Generate overwrite-tolerant stream     |
| `overwrite_consumer.c`        | Validate overwrite recovery behavior   |
| `mmap_overwrite_consumer.c`   | Validate mmap overwrite visibility     |

## Sequential text stream protocol

The sequential stream tests validate ordered textual delivery under
normal operating conditions.

These tests operate on delimiter-oriented byte streams rather than fixed
binary records.

| Test             | Purpose                               |
| ---------------- | ------------------------------------- |
| `seq_producer.c` | Generate sequential textual stream    |
| `seq_consumer.c` | Validate textual sequential integrity |

## Expected behavior by policy

| Policy             | Expected behavior                                                            |
| ------------------ | ---------------------------------------------------------------------------- |
| `block`            | Strict ordered delivery                                                      |
| `overwrite`        | Loss and discontinuities possible                                            |
| `overwrite + mmap` | Resync events and monotonicity violations possible near overwrite boundaries |

## Remarks

Overwrite-mode tests intentionally prioritize overwrite pressure and
recovery behavior over framed transport guarantees.

Because overwrite may occur mid-record, consumers recovering from
arbitrary offsets cannot always distinguish torn observations from valid
sequence discontinuities.

This may produce elevated resync counts or approximate gap estimates
during stress testing.

Production protocols should use framed records or integrity markers for
deterministic recovery semantics.

The mmap overwrite tests validate continued survivability and observable
stream progression rather than strict ordered delivery guarantees.

---

# Concurrency and stress tests

| Test                     | Purpose                             |
| ------------------------ | ----------------------------------- |
| `multi_writers_stress.c` | Concurrent writer stress validation |
| `multi-writer.sh`        | Multiwriter orchestration harness   |
| `spin_policy.c`          | Spin-policy behavior comparison     |
| `spin_budget.sh`         | Adaptive spin-budget comparison     |

These tests focus on:

* concurrent writer behavior
* overwrite survivability
* scheduler interaction
* wakeup semantics
* policy mutation stability

---

# DebugFS and observability tests

| Test                      | Purpose                        |
| ------------------------- | ------------------------------ |
| `debugfs_registration.sh` | debugfs registration lifecycle |
| `debugfs_read.c`          | Projection semantic validation |
| `dead_poll.sh`            | Poll teardown behavior         |
| `dead_remove.sh`          | Device removal while active    |

---

# Benchmark tools

| Tool                     | Purpose                               |
| ------------------------ | ------------------------------------- |
| `latency_benchmark.c`    | End-to-end latency measurement        |
| `throughput_benchmark.c` | Throughput and lag measurement        |
| `pressure_benchmark.c`   | Scheduler/contention pressure harness |

---

# ABI and inspection tools

| Tool                | Purpose                                        |
| ------------------- | ---------------------------------------------- |
| `ioctl_abi.c`       | Print ioctl ABI constants                      |
| `ioctl_info.c`      | Runtime device inspection                      |
| `offset_check.c`    | Shared-memory structure offsets                |
| `inode_challenge.c` | Verify device inode and character-device state |

---

# Common helpers

| File               | Purpose                    |
| ------------------ | -------------------------- |
| `common/helpers.c` | Shared utility helpers     |
| `common/helpers.h` | Shared helper declarations |

---

# Notes

The suite intentionally mixes:

* behavioral validation
* lifecycle validation
* concurrency stress testing
* observability validation
* benchmarking utilities

Some tools are intended primarily for development diagnostics and
benchmark iteration rather than strict pass/fail automation.

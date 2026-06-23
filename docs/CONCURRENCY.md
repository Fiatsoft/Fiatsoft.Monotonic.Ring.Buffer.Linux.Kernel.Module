# Concurrency model

Ringbuf is designed primarily for:

* one claimed reader
* one primary producer pipeline

while permitting:

* concurrent non-serialized writers
* concurrent observational readers

The design favors explicit ownership and visibility semantics over transparent synchronization.

---

# Reader ownership

Each device supports one exclusive claimed reader.

The claimed reader exclusively owns:

* advancement of shared consumption state
* progression visibility
* policy-control operations

This keeps queue ownership unambiguous.

The claim is attached to the owning file descriptor and released automatically on close or process termination.

---

# Observational readers

Observational readers intentionally do not participate in synchronization.

Examples include:

* debugfs `tap`
* debugfs `dump`
* inspection tooling

Observational interfaces exist to permit:

* live diagnostics
* traffic inspection
* debugging
* benchmarking

without perturbing the primary producer/consumer path.

---

# Writer model

Concurrent writers are permitted.

Writers are intentionally not globally serialized.

This means:

* ordering between writers is not guaranteed
* write visibility depends on reservation/publication timing
* applications requiring total ordering must serialize externally

This is a deliberate design decision.

The module prioritizes:

* low synchronization overhead
* explicit backpressure behavior
* predictable ownership semantics

rather than transparent multiproducer ordering guarantees.

---

# Memory ordering

The implementation relies on:

* atomic operations
* release/acquire visibility semantics
* explicit progression ownership

The steady-state data path avoids mutexes.

Writers publish visibility explicitly.

Readers advance progression explicitly.

Synchronization assumptions are intentionally narrow and localized.

---

# Backpressure semantics

Backpressure behavior is explicit and policy-driven.

Representative behaviors include:

* block
* overwrite
* adaptive spinning
* sleep/yield waiting

The module avoids hidden wakeup or implicit queue-management semantics.

Applications remain responsible for higher-level flow-control policy.

---

# Spin policy ownership

Spin policy is stored per device.

Only the claimed reader may modify device spin policy.

Policy changes are permitted only while the device is idle.

This restriction avoids undefined wakeup interactions during active writer contention.

---

# Non-goals

The project intentionally does not currently guarantee:

* multiproducer ordering
* multi-reader progression
* replay-safe observational snapshots
* distributed synchronization
* transactional queue semantics

The project is an explicit shared-memory IPC primitive rather than a generalized messaging framework.

---

# Design intent

The concurrency model favors:

* explicit ownership
* observability
* low synchronization overhead
* debuggability
* predictable backpressure

over transparent abstraction.

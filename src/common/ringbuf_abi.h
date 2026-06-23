/* Copyright (c) 2026 fiatsoft.com.
 * SPDX-License-Identifier: GPL-2.0-only */

/* ringbuf_abi.h */
#ifndef RINGBUF_ABI_H
#define RINGBUF_ABI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <linux/cache.h>
#define RBUF_CACHELINE_ALIGNED ____cacheline_aligned
#define rbuf_log pr_info
#else
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#define RBUF_CACHELINE_ALIGNED
#define rbuf_log printf
#endif

#define RBUF_ABI_VERSION 1

#define RBUF_MASTER_LIST(X) \
    X(RBUF_READER_FRESH,     "fresh",     SERIES_READER_POLICY, 0)   \
    X(RBUF_READER_FULL,      "full",      SERIES_READER_POLICY, 1)   \
    X(RBUF_WRITER_BLOCK,     "block",     SERIES_WRITER_POLICY, 100) \
    X(RBUF_WRITER_OVERWRITE, "overwrite", SERIES_WRITER_POLICY, 101) \
    X(RBUF_WRITER_DROP,      "drop",      SERIES_WRITER_POLICY, 102) \
    X(RBUF_SPIN_BUSY,        "busy",      SERIES_SPIN_POLICY,   200) \
    X(RBUF_SPIN_SLEEP,       "sleep",     SERIES_SPIN_POLICY,   201) \
    X(RBUF_SPIN_ADAPTIVE,    "adaptive",  SERIES_SPIN_POLICY,   202)

#define SERIES_READER_POLICY 1
#define SERIES_WRITER_POLICY 2
#define SERIES_SPIN_POLICY   3

#define DEFAULT_READER_POLICY RBUF_READER_FRESH
#define DEFAULT_WRITER_POLICY RBUF_WRITER_BLOCK
#define DEFAULT_SPIN_POLICY   RBUF_SPIN_BUSY

enum rbuf_reader_policy {
#define X(id, str, cat, val) IF_READER(cat)(id = val)
#define IF_READER(cat) _IF_READER(cat)
#define _IF_READER(cat) IF_READER_##cat
#define IF_READER_1(x) x,
#define IF_READER_2(x)
#define IF_READER_3(x)

    RBUF_MASTER_LIST(X)

#undef X
#undef IF_READER
#undef _IF_READER
#undef IF_READER_1
#undef IF_READER_2
#undef IF_READER_3

    RBUF_READER_POLICY_MAX
};

enum rbuf_writer_policy {
#define X(id, str, cat, val) IF_WRITER(cat)(id = val)
#define IF_WRITER(cat) _IF_WRITER(cat)
#define _IF_WRITER(cat) IF_WRITER_##cat
#define IF_WRITER_1(x)
#define IF_WRITER_2(x) x,
#define IF_WRITER_3(x)

    RBUF_MASTER_LIST(X)

#undef X
#undef IF_WRITER
#undef _IF_WRITER
#undef IF_WRITER_1
#undef IF_WRITER_2
#undef IF_WRITER_3

    RBUF_WRITER_POLICY_MAX
};

enum rbuf_spin_policy {
#define X(id, str, cat, val) IF_SPIN(cat)(id = val)
#define IF_SPIN(cat) _IF_SPIN(cat)
#define _IF_SPIN(cat) IF_SPIN_##cat
#define IF_SPIN_1(x)
#define IF_SPIN_2(x)
#define IF_SPIN_3(x) x,

    RBUF_MASTER_LIST(X)

#undef X
#undef IF_SPIN
#undef _IF_SPIN
#undef IF_SPIN_1
#undef IF_SPIN_2
#undef IF_SPIN_3

    RBUF_SPIN_POLICY_MAX
};

static inline bool rbuf_valid_value(int category, int val)
{
#define X(id, str, cat, num) \
    if ((cat) == category && (num) == val) \
        return true;

    RBUF_MASTER_LIST(X)

#undef X

    return false;
}

const char* rbuf_to_str(int val);
int rbuf_from_str(const char *s, int category);
 
struct rbuf_info {
    uint32_t abi_version;
    uint64_t size;
    uint64_t cap;
    uint64_t head;
    uint64_t tail;

    uint32_t writer_policy;
    uint32_t reader_policy;
    uint32_t spin_policy;
    uint32_t numa_node;
    uint32_t flags;
    
    uint32_t reserved[8];
};

struct rbuf_shm {

    /*
     * head/tail are monotonically increasing logical byte positions.
     *
     * Correctness invariant:
     *
     *      0 <= (head - tail) <= cap
     *
     * Physical indices are derived via:
     *
     *      index = pos & mask
     *
     * This avoids the classic ring-buffer ABA ambiguity caused by
     * index wraparound.
     *
     * Correctness is preserved across u64 overflow provided:
     *
     *      cap < 2^63
     *
     * and head/tail are interpreted modulo 2^64.
     */
    uint64_t head; /* total bytes ever written (monotonic) */
    uint64_t tail; /* total bytes logically consumed/dropped */
    uint8_t  data[];  /* ring data */
};
// much slower:
// struct rbuf_shm {
//     uint64_t head RBUF_CACHELINE_ALIGNED; /* total bytes ever written (monotonic) */
//     uint64_t tail RBUF_CACHELINE_ALIGNED; /* total bytes logically consumed/dropped */
//     uint8_t  data[];                      /* ring data */
// };
// slightly slower:
// struct rbuf_shm {
//     uint64_t head RBUF_CACHELINE_ALIGNED;
//     uint8_t pad1[64 - sizeof(uint64_t)];
//
//     uint64_t tail RBUF_CACHELINE_ALIGNED;
//     uint8_t pad2[64 - sizeof(uint64_t)];
//     uint8_t  data[];                      /* ring data */
// };
#define OFF_HEAD   offsetof(struct rbuf_shm, head)
#define OFF_TAIL   offsetof(struct rbuf_shm, tail)
#define OFF_DATA   offsetof(struct rbuf_shm, data)

#endif
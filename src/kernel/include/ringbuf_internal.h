/* Copyright (c) 2026 fiatsoft.com.
 * SPDX-License-Identifier: GPL-2.0-only */

/* kernel/include/ringbuf_internal.h */

#ifndef RINGBUF_INTERNAL_H
#define RINGBUF_INTERNAL_H

#include <linux/cdev.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

#include "ringbuf_abi.h"
#include "ringbuf.h"

#ifndef MAX_NAME_LEN
#define MAX_NAME_LEN 80
#endif

extern void ring_dev_release(struct kref *ref);

struct ring_dev {
    struct cdev cdev;
    struct device *device;
    char name[MAX_NAME_LEN];
    void *vmem;           /* vmalloc region base */
    size_t size;          /* total mapped byte size = sizeof(rbuf_shm) + capacity */
    u32 mask;             /* capacity = mask + 1 */
    u32 capacity;         /* number of data entries (power-of-two) */
    struct rbuf_shm *shm; /* pointer into vmem */

    enum rbuf_reader_policy reader_policy;
    enum rbuf_writer_policy writer_policy;

    int minor;

    char last_add_message[128];
    int alloc_node;

    atomic64_t stat_writes;
    atomic64_t stat_reads;
    atomic64_t stat_drops;
    atomic64_t stat_max_used;
    atomic64_t stat_bytes_written;
    atomic64_t stat_bytes_read;
    atomic64_t stat_copy_ns;
    atomic64_t stat_copy_calls;
    atomic64_t stat_flushes;
    atomic64_t stat_bytes_flushed;

    struct dentry *dbgdir;

    spinlock_t lock;
    wait_queue_head_t readq, writeq;
    enum rbuf_spin_policy spin_policy;
    atomic_t writers_waiting;
    struct kref refcount;
    bool dead;

    struct ring_reader_ctx *reader_fd; /* NULL when no reader claimed */
    struct ring_writer_ctx *writer_fd;
    struct ring_tap_ctx *tap_fd;
    struct ring_dump_ctx *dump_fd;

} __aligned(64);

struct ring_reader_ctx {
    struct ring_dev *dev;
    uint64_t reader_pos;
    pid_t owner_pid;                   /* for diagnostics only */
    enum rbuf_spin_policy spin_policy; /* per-FD spin behavior */
    bool claimed;                      /* true when this FD owns the reader */
    bool dead;
};

struct ring_writer_ctx {
    struct ring_dev *dev;
    pid_t owner_pid;
};
struct ring_tap_ctx {
    struct ring_dev *r;
    u64 start_pos;
    u64 end_pos;
    u64 pos;
};
struct ring_dump_ctx {
    struct ring_dev *r;
    u64 start;
    u64 end;
};

struct ringbuf_state {
    uint64_t head;
    uint64_t tail;
    uint8_t claimed;
    uint64_t total_size;
    uint64_t data_capacity;
    pid_t owner_pid;
};

void ring_fill_info(struct ring_dev *r, struct rbuf_info *info);

/* helper to get capacity */
static inline u64 cap_of(struct ring_dev *r) { return r ? r->capacity : 0; }

#define rbuf_dbg(fmt, ...) \
    do { if (verbose) pr_info("ringbuf: " fmt, ##__VA_ARGS__); } while (0)

#endif
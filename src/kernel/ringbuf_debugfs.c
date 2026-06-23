// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "ringbuf_internal.h"
#include "ringbuf_debugfs.h"

static struct dentry *ring_debugfs_root;

static int ring_stats_show(struct seq_file *m, void *v)
{
    struct ring_dev *r = m->private;

    u64 ns = atomic64_read(&r->stat_copy_ns);
    u64 calls = atomic64_read(&r->stat_copy_calls);
    u64 bytes_written = atomic64_read(&r->stat_bytes_written);
    u64 bytes_read = atomic64_read(&r->stat_bytes_read);
    u64 bytes_io_total = bytes_written + bytes_read;
    u64 flushes = atomic64_read(&r->stat_flushes);
    u64 bytes_flushed = atomic64_read(&r->stat_bytes_flushed);

    seq_printf(m, "name: %s\n", r->name);
    seq_printf(m, "minor: %d\n", r->minor);
    seq_printf(m, "writes: %lld\n", (long long)atomic64_read(&r->stat_writes));
    seq_printf(m, "reads: %lld\n", (long long)atomic64_read(&r->stat_reads));
    seq_printf(m, "drops: %lld\n", (long long)atomic64_read(&r->stat_drops));
    seq_printf(m, "bytes_written: %lld\n", bytes_written);
    seq_printf(m, "bytes_read: %lld\n", bytes_read);
    seq_printf(m, "max_used: %lld\n", (long long)atomic64_read(&r->stat_max_used));
    seq_printf(m, "capacity: %u\n", r->capacity);
    seq_printf(m, "head: %llu\n", smp_load_acquire(&r->shm->head));
    seq_printf(m, "tail: %llu\n", smp_load_acquire(&r->shm->tail));
    seq_printf(m, "last_add_message: %s\n", r->last_add_message);
    seq_printf(m, "alloc_node: %d\n", r->alloc_node);
    seq_printf(m, "copy_calls: %llu\n", calls);
    seq_printf(m, "copy_time_ns: %llu\n", ns);
    if (calls)
        seq_printf(m, "avg_copy_ns: %llu\n", ns / calls);
    seq_printf(m, "bytes_io_total: %llu\n", bytes_io_total);
    if (bytes_io_total) {
        u64 ns_per_byte_x1000 = (ns * 1000) / bytes_io_total;
        seq_printf(m, "ns_per_byte: %llu.%03llu\n",
                ns_per_byte_x1000 / 1000,
                ns_per_byte_x1000 % 1000);
    }
    seq_printf(m, "flushes: %llu\n", flushes);
    seq_printf(m, "bytes_flushed: %llu\n", bytes_flushed);
    return 0;
}

static int ring_stats_open(struct inode *inode, struct file *file)
{
    return single_open(file, ring_stats_show, inode->i_private);
}

static const struct file_operations ring_stats_fops = {
    .open    = ring_stats_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};

/* ---------- info  ---------- */

char *get_binary_str(uint32_t val, char *buf) {
    int i;

    for (i = 31; i >= 0; i--) {
        // Shift val right by i positions and check the LSB
        buf[31 - i] = ((val >> i) & 1) ? '1' : '0';
    }
    buf[32] = '\0';

    return buf;
}

static int ring_info_show(struct seq_file *m, void *v)
{
    struct ring_dev *r = m->private;
    struct rbuf_info info;
    char bin_str_buf[33];

    ring_fill_info(r, &info);

    seq_printf(m, "size=%llu\n", info.size);
    seq_printf(m, "cap=%llu\n", info.cap);
    seq_printf(m, "head=%llu\n", info.head);
    seq_printf(m, "tail=%llu\n", info.tail);
    seq_printf(m, "writer_policy=%u (%s)\n", info.writer_policy, rbuf_to_str(info.writer_policy));
    seq_printf(m, "reader_policy=%u (%s)\n", info.reader_policy, rbuf_to_str(info.reader_policy));
    seq_printf(m, "spin_policy=%u (%s)\n", info.spin_policy, rbuf_to_str(info.spin_policy));
    seq_printf(m, "numa_node=%d\n", info.numa_node);
    seq_printf(m, "flags=0x%08x (b%s)\n", info.flags, get_binary_str(info.flags, bin_str_buf));
    seq_printf(m, "reserved[0]=0x%08x (b%s)\n", info.reserved[0], get_binary_str(info.reserved[0], bin_str_buf));
    seq_printf(m, "reserved[1]=0x%08x (b%s)\n", info.reserved[1], get_binary_str(info.reserved[1], bin_str_buf));
    seq_printf(m, "reserved[2]=0x%08x (b%s)\n", info.reserved[2], get_binary_str(info.reserved[2], bin_str_buf));
    seq_printf(m, "reserved[3]=0x%08x (b%s)\n", info.reserved[3], get_binary_str(info.reserved[3], bin_str_buf));
    seq_printf(m, "reserved[4]=0x%08x (b%s)\n", info.reserved[4], get_binary_str(info.reserved[4], bin_str_buf));
    seq_printf(m, "reserved[5]=0x%08x (b%s)\n", info.reserved[5], get_binary_str(info.reserved[5], bin_str_buf));
    seq_printf(m, "reserved[6]=0x%08x (b%s)\n", info.reserved[6], get_binary_str(info.reserved[6], bin_str_buf));
    seq_printf(m, "reserved[7]=0x%08x (b%s)\n", info.reserved[7], get_binary_str(info.reserved[7], bin_str_buf));
    return 0;
}

static int ring_info_open(struct inode *inode, struct file *file)
{
    return single_open(file, ring_info_show, inode->i_private);
}

static const struct file_operations ring_info_fops = {
    .open    = ring_info_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};

static int tap_open(struct inode *inode, struct file *f)
{
    struct ring_dev *r = inode->i_private;
    struct ring_tap_ctx *ctx;
    // u64 head, tail, avail;
    u64 head, head1, head2, tail, avail;

    if (!r || !r->shm)
        return -ENODEV;
    
    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    kref_get(&r->refcount);
    
    /*
    * CONTRACT BOUNDARY: Take a stable snapshot of (head, tail).
    * We only guarantee monotonic consistency, not atomicity.
    */
    do {
        head1 = smp_load_acquire(&r->shm->head);
        tail  = smp_load_acquire(&r->shm->tail);
        head2 = smp_load_acquire(&r->shm->head);
    } while (head1 != head2);
    head = head2;
    
    if (head < tail) {
        kref_put(&r->refcount, ring_dev_release);
        kfree(ctx);
        return -EOVERFLOW;
    }

    avail = head - tail;
    if (avail > r->capacity)
        avail = r->capacity;

    ctx->r = r;
    ctx->start_pos = head - avail;
    ctx->end_pos   = head;
    ctx->pos   = ctx->start_pos;

    f->private_data = ctx;

    return 0;
}

static ssize_t tap_read(struct file *f,
                        char __user *ubuf,
                        size_t len,
                        loff_t *ppos)
{
    struct ring_tap_ctx *ctx = f->private_data;
    struct ring_dev *r = ctx->r;
    struct rbuf_shm *shm = r->shm;
    u32 cap = r->capacity;

    u64 start = ctx->start_pos;
    u64 end   = ctx->end_pos;
    u64 total = end - start;

    if (*ppos >= total)
        return 0;

    size_t copied = 0;

    while (copied < len && (*ppos + copied) < total) {
        u64 pos = start + *ppos + copied;
        u32 idx = pos & (cap - 1);

        if (copy_to_user(ubuf + copied, &shm->data[idx], 1))
            return copied ? copied : -EFAULT;

        copied++;
    }

    *ppos += copied;
    return copied;
}

static int tap_release(struct inode *inode, struct file *f)
{
    struct ring_tap_ctx *ctx = f->private_data;

    if (ctx) {
        kref_put(&ctx->r->refcount, ring_dev_release);
        kfree(ctx);
    }

    return 0;
}

static const struct file_operations ring_tap_fops = {
    .open    = tap_open,
    .read    = tap_read,
    .release = tap_release,
    .llseek  = default_llseek,
};

/* ---------- dump context ---------- */

static int dump_open(struct inode *inode, struct file *f)
{
    struct ring_dev *r = inode->i_private;
    struct rbuf_shm *shm = r->shm;

    struct ring_dump_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    kref_get(&r->refcount);

    u64 head = smp_load_acquire(&shm->head);
    u64 cap  = cap_of(r);

    ctx->r = r;
    ctx->start = (head >= cap) ? (head - cap) : 0;
    ctx->end   = head;

    f->private_data = ctx;
    return 0;
}

static ssize_t dump_read(struct file *filp,
                         char __user *ubuf,
                         size_t len,
                         loff_t *ppos)
{
    struct ring_dump_ctx *ctx = filp->private_data;
    struct ring_dev *r = ctx->r;
    struct rbuf_shm *shm = r->shm;
    u32 cap = cap_of(r);

    u64 total = ctx->end - ctx->start;

    if (*ppos >= total)
        return 0;

    size_t copied = 0;

    while (copied < len && (*ppos + copied) < total) {
        u64 pos = ctx->start + *ppos + copied;
        u32 idx = pos & (cap - 1);

        if (copy_to_user(ubuf + copied, &shm->data[idx], 1))
            return copied ? copied : -EFAULT;

        copied++;
    }

    *ppos += copied;
    return copied;
}

static int dump_release(struct inode *inode, struct file *f)
{
    struct ring_dump_ctx *ctx = f->private_data;

    if (ctx) {
        kref_put(&ctx->r->refcount, ring_dev_release);
        kfree(ctx);
    }

    return 0;
}

static const struct file_operations ring_dump_fops = {
    .open  = dump_open,
    .read  = dump_read,
    .release = dump_release,
    .llseek  = default_llseek
}; 

/* ===================== DEBUGFS ROOT ===================== */

int ringbuf_debugfs_init(void)
{
    ring_debugfs_root = debugfs_create_dir("ringbuf", NULL);
    if (IS_ERR(ring_debugfs_root)) {
        int ret = PTR_ERR(ring_debugfs_root);
        ring_debugfs_root = NULL;
        return ret;
    }
    return 0;
}

void ringbuf_debugfs_exit(void)
{
    debugfs_remove_recursive(ring_debugfs_root);
    ring_debugfs_root = NULL;
}

/* ===================== PER DEVICE ===================== */

void ringbuf_debugfs_add(struct ring_dev *r)
{
    if (!ring_debugfs_root || !r)
        return;

    r->dbgdir = debugfs_create_dir(r->name, ring_debugfs_root);
    if (!r->dbgdir)
        return;

    debugfs_create_file(
        "stats",
        0444,
        r->dbgdir,
        r,
        &ring_stats_fops
    );

    debugfs_create_file(
        "tap",
        0400,
        r->dbgdir,
        r,
        &ring_tap_fops
    );

    debugfs_create_file(
        "dump",
        0400,
        r->dbgdir,
        r,
        &ring_dump_fops
    );

    debugfs_create_file(
        "info",
        0400,
        r->dbgdir,
        r,
        &ring_info_fops
    );
}

void ringbuf_debugfs_remove(struct ring_dev *r)
{
    if (!r || !r->dbgdir)
        return;

    debugfs_remove_recursive(r->dbgdir);
    r->dbgdir = NULL;
}

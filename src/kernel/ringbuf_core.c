// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

/*
 * kernel/ringbuf_core.c - SPSC lock-free ring buffer with NUMA, per-device feedback.
 */

#include <linux/module.h>
#include <linux/init.h>

#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>

#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>

#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/atomic.h>

#include <linux/string.h>
#include <linux/version.h>
#include <linux/moduleparam.h>
#include <linux/sysfs.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/numa.h>
#include <linux/signal.h>
#include <linux/sched.h>

#include "ringbuf.h"
#include "ringbuf_internal.h"
#include "ringbuf_debugfs.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("fiatsoft.com");
MODULE_DESCRIPTION("ringbuf - SPSC ring with NUMA, per-device policies/feedback, telemetry, runtime configuration and support for extra unserialized-writers/observer-projections");
MODULE_VERSION("0.5");

#define RBUF_FIATSOFT_PROJECT_ID "Fiatsoft.Monotonic.Ring.Buffer.Linux.Kernel.Module"

/* compile-time constants */
#ifndef MAX_DEVICES
#define MAX_DEVICES 64
#endif
#define DEVICE_BASENAME "ringbuf"

/* MODULE PARAMS */
static int num_devices = 0;
module_param(num_devices, int, 0444);
MODULE_PARM_DESC(num_devices, "Number of preconfigured devices (0 => none)");

static char *device_names[MAX_DEVICES];
static int device_names_count = 0;
module_param_array(device_names, charp, &device_names_count, 0444);
MODULE_PARM_DESC(device_names, "Per-device names (suffixes)");

static int device_minors[MAX_DEVICES];
static int device_minors_count = 0;
module_param_array(device_minors, int, &device_minors_count, 0444);
MODULE_PARM_DESC(device_minors, "Per-device minor IDs (-1 for auto)");

static unsigned int device_sizes[MAX_DEVICES];
static int device_sizes_count = 0;
module_param_array(device_sizes, uint, &device_sizes_count, 0444);
MODULE_PARM_DESC(device_sizes, "Per-device sizes (bytes) - comma-separated");

/* policies */
static char *reader_policies[MAX_DEVICES];
static char *writer_policies[MAX_DEVICES];
static char *spin_policies[MAX_DEVICES];
static int reader_policies_count;
static int writer_policies_count;
static int spin_policies_count;
module_param_array(reader_policies, charp, &reader_policies_count, 0444);
MODULE_PARM_DESC(reader_policies, "Reader policies per device");
module_param_array(writer_policies, charp, &writer_policies_count, 0444);
MODULE_PARM_DESC(writer_policies, "Writer policies per device");
module_param_array(spin_policies, charp, &spin_policies_count, 0444);
MODULE_PARM_DESC(spin_policies, "Spin policies per device");

static char *default_reader_policy = "fresh";
static char *default_writer_policy = "block";
module_param(default_reader_policy, charp, 0644);
MODULE_PARM_DESC(default_reader_policy, "Default global reader policy: fresh|full");
module_param(default_writer_policy, charp, 0644);
MODULE_PARM_DESC(default_writer_policy, "Default global writer policy: block|overwrite|drop");

static char *default_spin_policy = "adaptive";
module_param(default_spin_policy, charp, 0644);
MODULE_PARM_DESC(default_spin_policy, "Spin policy for writer when full: busy|adaptive|sleep");
static unsigned int spin_budget = 10000;
module_param(spin_budget, uint, 0644);
MODULE_PARM_DESC(spin_budget, "Spin iterations for adaptive policy");

/* devnode convenience */
static int dev_mode = 0660;
static int dev_gid = 0;
module_param(dev_mode, int, 0444);
MODULE_PARM_DESC(dev_mode, "Device node mode (octal), e.g. 0660");
module_param(dev_gid, int, 0444);
MODULE_PARM_DESC(dev_gid, "Device node gid (0 = leave to udev)");

/* default NUMA node if none specified (-1 => caller) */
static int default_numa_node = -1;
module_param(default_numa_node, int, 0444);
MODULE_PARM_DESC(default_numa_node, "Default NUMA node to allocate rings (-1 => caller node)");
static int device_nodes[MAX_DEVICES];
static int device_nodes_count;
module_param_array(device_nodes, int, &device_nodes_count, 0444);

/* prototypes for sysfs */
static ssize_t add_device_store(const struct class *class,
                                const struct class_attribute *attr,
                                const char *buf, size_t count);
static ssize_t remove_device_store(const struct class *class,
                                   const struct class_attribute *attr,
                                   const char *buf, size_t count);
static ssize_t major_show(const struct class *class,
                          const struct class_attribute *attr,
                          char *buf);
static CLASS_ATTR_RO(major);
static CLASS_ATTR_WO(add_device);
static CLASS_ATTR_WO(remove_device);

static bool verbose = false;
module_param(verbose, bool, 0644);

static struct ring_dev *ring_table[MAX_DEVICES];
static dev_t ring_dev_first;
static struct class *ring_class;
static DEFINE_MUTEX(minor_table_lock);
// static struct dentry *ring_debugfs_root;

bool ringbuf_unloading;

/* remap vmalloc area using kernel helper when available */
static int remap_vmalloc_area(struct vm_area_struct *vma, void *vmem)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 26)
    if (remap_vmalloc_range(vma, vmem, 0) == 0)
        return 0;
#endif
    /* fallback: map page by page (rare on modern kernels) */
    {
        unsigned long len = vma->vm_end - vma->vm_start;
        unsigned long off;
        for (off = 0; off < len; off += PAGE_SIZE)
        {
            struct page *pg = vmalloc_to_page((char *)vmem + off);
            if (!pg)
                return -ENODEV;
            unsigned long pfn = page_to_pfn(pg);
            unsigned long chunk = min((unsigned long)PAGE_SIZE, len - off);
            if (remap_pfn_range(vma, vma->vm_start + off, pfn, chunk, vma->vm_page_prot))
                return -EAGAIN;
        }
        return 0;
    }
}

/* small pow2 helpers */
static inline unsigned int next_pow2(unsigned int x)
{
    if (x <= 1)
        return 1;
    return 1U << fls(x - 1);
}

/* forward prototypes for fops */
static int ring_open(struct inode *inode, struct file *file);
static int ring_release(struct inode *inode, struct file *file);
static ssize_t ring_read(struct file *filp, char __user *ubuf, size_t len, loff_t *off);
static ssize_t ring_write(struct file *filp, const char __user *ubuf, size_t len, loff_t *off);
static __poll_t ring_poll(struct file *file, poll_table *wait);
static int ring_mmap(struct file *filp, struct vm_area_struct *vma);
static long ring_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

bool ring_has_space(struct ring_dev *r);

static const struct file_operations ring_fops = {
    .owner = THIS_MODULE,
    .open = ring_open,
    .release = ring_release,
    .read = ring_read,
    .write = ring_write,
    .mmap = ring_mmap,
    .poll = ring_poll,
    .unlocked_ioctl = ring_ioctl,
};

/* per-device sysfs: last_add_message */
static ssize_t last_add_message_show(struct device *dev,
                                     struct device_attribute *attr,
                                     char *buf)
{
    struct ring_dev *r = dev_get_drvdata(dev);
    if (!r)
        return scnprintf(buf, PAGE_SIZE, "none\n");
    return scnprintf(buf, PAGE_SIZE, "%s\n", r->last_add_message);
}
static DEVICE_ATTR_RO(last_add_message);

/* per-device sysfs: alloc_node */
static ssize_t device_alloc_node_show(struct device *dev,
                                      struct device_attribute *attr,
                                      char *buf)
{
    struct ring_dev *r = dev_get_drvdata(dev);
    if (!r)
        return scnprintf(buf, PAGE_SIZE, "-1\n");
    return scnprintf(buf, PAGE_SIZE, "%d\n", r->alloc_node);
}
static DEVICE_ATTR_RO(device_alloc_node);

static int wait_for_space_policy(struct ring_dev *r)
{
    u64 head, tail, used;
    u64 cap;
    unsigned int i;

    if (!r || !r->shm)
        return -EINVAL;
    
    cap = cap_of(r);

    switch (r->spin_policy)
    {

        /* ===================== BUSY ===================== */
        case RBUF_SPIN_BUSY:
        {
            unsigned long spins = 0;

            for (;;)
            {

                if (!(++spins & 0x1FFFF))
                {
                    cond_resched(); 
                }
                if (signal_pending(current))
                    return -EINTR;
                if (READ_ONCE(r->dead))
                    return -ESHUTDOWN;

                head = READ_ONCE(r->shm->head);
                tail = READ_ONCE(r->shm->tail);
                used = head - tail;

                if (used < cap)
                    return 0;

                cpu_relax();

            }
        }

        /* ===================== ADAPTIVE ===================== */
        case RBUF_SPIN_ADAPTIVE:
            for (;;)
            {

                /* bounded spin phase */
                for (i = 0; i < spin_budget; ++i)
                {

                    if (signal_pending(current))
                        return -EINTR;
                    if (READ_ONCE(r->dead))
                        return -ESHUTDOWN;

                    head = READ_ONCE(r->shm->head);
                    tail = READ_ONCE(r->shm->tail);
                    used = head - tail;

                    if (used < cap)
                        return 0;

                    cpu_relax();
                }

                cond_resched();

                /* fallback: real sleep */
                atomic_inc(&r->writers_waiting);
                if (wait_event_interruptible(
                        r->writeq,
                        ({
                            u64 h = smp_load_acquire(&r->shm->head);
                            u64 t = smp_load_acquire(&r->shm->tail);
                            READ_ONCE(r->dead) || (h - t) < cap;
                        })))
                {
                    atomic_dec(&r->writers_waiting);
                    return -EINTR;
                }
                atomic_dec(&r->writers_waiting);
            }

        /* ===================== SLEEP ===================== */
        case RBUF_SPIN_SLEEP:
        default: {
            int ret;
            atomic_inc(&r->writers_waiting);
            ret = wait_event_interruptible(
                r->writeq,
                ({
                    u64 h = smp_load_acquire(&r->shm->head);
                    u64 t = smp_load_acquire(&r->shm->tail);
                    READ_ONCE(r->dead) || (h - t) < cap;
                })
            );
            atomic_dec(&r->writers_waiting);
            if (ret)
                return -EINTR;
            return 0;
        }
    }
    return 0;
}

static inline void ring_update_max_used(struct ring_dev *r, u64 used_now)
{
    u64 cur = atomic64_read(&r->stat_max_used);

    while (used_now > cur) {
        u64 old = atomic64_cmpxchg(&r->stat_max_used, cur, used_now);
        if (old == cur)
            break;
        cur = old;
    }
}

static ssize_t ring_write(struct file *filp,
                          const char __user *ubuf,
                          size_t len,
                          loff_t *off)
{
    u64 start = ktime_get_ns();
    struct ring_writer_ctx *fd_ctx = filp->private_data;
    struct ring_dev *r;
    if (!fd_ctx)
        return -EINVAL;
    r = fd_ctx->dev;
    if (!r)
        return -ENODEV;

    size_t written = 0;
    size_t chunk_size = 256;
    int copy_calls = 0;
    int out_ret = -EFAULT;

    if (!r || !r->shm)
        return -ENODEV;

    if (unlikely(READ_ONCE(r->dead)))
        return -ESHUTDOWN;

    const u64 cap = cap_of(r);
    const u64 mask = r->mask;

    
    while (written < len)
    {
        u64 head = smp_load_acquire(&r->shm->head);
        u64 tail = smp_load_acquire(&r->shm->tail);
        u64 pos = head;
        u64 used = head - tail;
        u64 new_tail, new_head;
        
        size_t chunk = min(len - written, chunk_size);
        u64 free = cap - used;
        u64 to_write;

        /* ================= OVERWRITE ================= */
        if (r->writer_policy == RBUF_WRITER_OVERWRITE)
        {
            if (chunk > cap)
                chunk = cap;

            if (chunk > free)
                atomic64_inc(&r->stat_drops);

            to_write = chunk;
        }

        /* ================= BLOCKING ================= */
        else
        {
            /* wait until space OR dead */
            while (used >= cap)
            {
                if (READ_ONCE(r->dead))
                    return written ? written : -ESHUTDOWN;

                if (wait_for_space_policy(r))
                {
                    out_ret = -EINTR;
                    goto out;
                }

                /* 🔑 RE-CHECK AFTER WAKE */
                head = smp_load_acquire(&r->shm->head);
                tail = smp_load_acquire(&r->shm->tail);
                used = head - tail;
            }

            free = cap - used;
            to_write = min_t(u64, chunk, free);
        }

        /* defensive: must make forward progress */
        if (!to_write)
            continue;

        /* ================= COPY ================= */
        u64 index = head & mask;
        u64 first = min_t(u64, to_write, cap - index);
        u64 second = to_write - first;

        if (copy_from_user(&r->shm->data[index],
                           ubuf + written,
                           first))
            goto out;
        copy_calls++;

        if (second) {
            if (copy_from_user(&r->shm->data[0],
                               ubuf + written + first,
                               second))
                goto out;
            copy_calls++;
        }

        /* ================= COMMIT ================= */
        new_head = pos + to_write;

        if (r->writer_policy == RBUF_WRITER_OVERWRITE)
        {
            new_tail = tail;

            if (new_head - tail > cap)
                new_tail = new_head - cap;

            if (new_tail != tail)
                smp_store_release(&r->shm->tail, new_tail);
        }

        smp_store_release(&r->shm->head, new_head);
        written += to_write;
        atomic64_add(to_write, &r->stat_bytes_written);
        ring_update_max_used(r, new_head - new_tail);
        /* wake readers */
        if (waitqueue_active(&r->readq))
            wake_up_interruptible(&r->readq);
    }

out:
    {
        u64 end = ktime_get_ns();
        atomic64_add(end - start, &r->stat_copy_ns);
        atomic64_inc(&r->stat_writes);
        atomic64_add(copy_calls, &r->stat_copy_calls);
    }
    return written ? written : out_ret;
}

static ssize_t ring_read(struct file *filp,
                         char __user *ubuf,
                         size_t len,
                         loff_t *off)
{
    struct ring_reader_ctx *fd_ctx = filp->private_data;
    struct ring_dev *r;
    size_t copied = 0;

    if (!fd_ctx || !fd_ctx->claimed)
        return -EPERM;

    r = fd_ctx->dev;
    if (!r || !r->shm)
        return -ENODEV;

    if (READ_ONCE(r->dead))
        return -ESHUTDOWN;

    const u64 mask = r->mask;

    while (copied < len) {
        u64 head = smp_load_acquire(&r->shm->head);
        u64 tail = smp_load_acquire(&r->shm->tail);

        /* overwrite recovery */
        if (fd_ctx->reader_pos < tail)
            fd_ctx->reader_pos = tail;
        
        if (fd_ctx->reader_pos >= head) {
            if (copied)
                break;

            if (wait_event_interruptible(
                    r->readq,
                    READ_ONCE(r->dead) ||
                    smp_load_acquire(&r->shm->head) > fd_ctx->reader_pos))
                return -EINTR;

            if (READ_ONCE(r->dead))
                return -ESHUTDOWN;
                
            continue;
        }

        u64 index = fd_ctx->reader_pos & mask;

        if (put_user(r->shm->data[index], ubuf + copied))
            return copied ? copied : -EFAULT;

        fd_ctx->reader_pos++;
        copied++;
    }

    /* destructive semantics */
    smp_store_release(&r->shm->tail, fd_ctx->reader_pos);

    if (waitqueue_active(&r->writeq))
        wake_up_interruptible(&r->writeq);

    atomic64_add(copied, &r->stat_bytes_read);
    atomic64_inc(&r->stat_reads);

    return copied;
}

static __poll_t ring_poll(struct file *file, poll_table *wait)
{
    struct ring_reader_ctx *fd_ctx = file->private_data;
    struct ring_dev *r;
    __poll_t mask = 0;

    if (!fd_ctx)
        return EPOLLERR;

    r = fd_ctx->dev;
    if (!r || !r->shm)
        return EPOLLERR;

    if (READ_ONCE(r->dead))
        return POLLERR | POLLHUP;

    poll_wait(file, &r->readq, wait);
    poll_wait(file, &r->writeq, wait);

    u64 head = smp_load_acquire(&r->shm->head);
    u64 tail = smp_load_acquire(&r->shm->tail);
    u64 cap = cap_of(r);

    u64 used = min(head - tail, cap);

    if (head > fd_ctx->reader_pos)
        mask |= POLLIN | POLLRDNORM;

    if (used < cap)
        mask |= POLLOUT | POLLWRNORM;

    return mask;
}

static int ring_open(struct inode *inode, struct file *filp)
{
    struct ring_dev *r = container_of(inode->i_cdev, struct ring_dev, cdev);
    if (READ_ONCE(r->dead))
        return -ESHUTDOWN;

    if ((filp->f_mode & FMODE_READ) &&
        (filp->f_mode & FMODE_WRITE))
        return -EINVAL;

    /* Writers: store the device pointer directly (legacy / fast path) */
    if (filp->f_mode & FMODE_WRITE)
    {
        struct ring_writer_ctx *fd_ctx;

        fd_ctx = kzalloc(sizeof(*fd_ctx), GFP_KERNEL);
        if (!fd_ctx)
            return -ENOMEM;

        fd_ctx->dev = r;
        fd_ctx->owner_pid = task_tgid_vnr(current);
        
        spin_lock(&r->lock);
        if (r->writer_fd) {
            spin_unlock(&r->lock);
            kfree(fd_ctx);
            return -EBUSY;
        }
        r->writer_fd = fd_ctx;
        // fd_ctx->claimed = true;
        spin_unlock(&r->lock);

        filp->private_data = fd_ctx;
        kref_get(&r->refcount);
        return 0;
    }

    /* Readers: allocate per-FD context */
    if (filp->f_mode & FMODE_READ)
    {
        struct ring_reader_ctx *fd_ctx;

        fd_ctx = kzalloc(sizeof(*fd_ctx), GFP_KERNEL);
        if (!fd_ctx)
            return -ENOMEM;

        fd_ctx->dev = r;
        fd_ctx->spin_policy = RBUF_SPIN_ADAPTIVE;
        fd_ctx->claimed = false;
        fd_ctx->owner_pid = task_tgid_vnr(current);

        if (r->reader_policy == RBUF_READER_FULL)
            fd_ctx->reader_pos = smp_load_acquire(&r->shm->tail);
        else
            fd_ctx->reader_pos = smp_load_acquire(&r->shm->head);

        filp->private_data = fd_ctx;
        rbuf_dbg("%s:%s:%d: context written to reader: fd_ctx=%p", __FILE__, __func__, __LINE__, fd_ctx);
        rbuf_dbg("%s:%s:%d: context written to reader: fd_ctx->dev=%p", __FILE__, __func__, __LINE__, fd_ctx->dev);
        rbuf_dbg("%s:%s:%d: context written to reader: fd_ctx is not garbage: fd_ctx->owner_pid=%d", __FILE__, __func__, __LINE__, fd_ctx->owner_pid);
        rbuf_dbg("%s:%s:%d: context written to reader: filp=%p", __FILE__, __func__, __LINE__, filp);
        rbuf_dbg("%s:%s:%d: context written to reader: filp->private_data=%p", __FILE__, __func__, __LINE__, filp->private_data);
        
        kref_get(&r->refcount);
        return 0;
    }

    return -EINVAL;
}

static int ring_release(struct inode *inode, struct file *filp)
{
    if (filp->f_mode & FMODE_WRITE) {
        struct ring_writer_ctx *fd_ctx = filp->private_data;
        struct ring_dev *r;

        if (!fd_ctx)
            return 0;

        r = fd_ctx->dev;
        if (!r)
            return 0;

        spin_lock(&r->lock);

        if (r->writer_fd == fd_ctx)
            r->writer_fd = NULL;

        spin_unlock(&r->lock);

        wake_up_all(&r->readq);
        wake_up_all(&r->writeq);

        kref_put(&r->refcount, ring_dev_release);

        filp->private_data = NULL;

        kfree(fd_ctx);

        return 0;
    }

    if (filp->f_mode & FMODE_READ) {
        struct ring_reader_ctx *fd_ctx = filp->private_data;
        struct ring_dev *r;

        if (!fd_ctx)
            return 0;

        r = fd_ctx->dev;
        if (!r)
            return 0;

        spin_lock(&r->lock);
        if (r->reader_fd == fd_ctx)
            r->reader_fd = NULL;
        spin_unlock(&r->lock);
            
        wake_up_all(&r->readq);
        wake_up_all(&r->writeq);

        kref_put(&r->refcount, ring_dev_release);

        filp->private_data = NULL;

        kfree(fd_ctx);

        return 0;
    }
    return 0;
}

/* mmap to expose the vmalloc'd region to userspace */
static int ring_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct ring_reader_ctx *fd_ctx;
    struct ring_dev *r;
    if (!(filp->f_mode & FMODE_READ))
        return -ENOTTY;
    fd_ctx = filp->private_data;
    if (!fd_ctx)
        return -EBADF;
    r = fd_ctx->dev;
    unsigned long size = vma->vm_end - vma->vm_start;
    if (!r || !r->vmem)
        return -ENODEV;
    if (size < r->size)
        return -EINVAL;
    rbuf_dbg("ring_mmap: r=%p vmem=%p size=%lu\n", r, r->vmem, r->size);
    rbuf_dbg("ring_mmap: requested len=%lu, ring_size=%zu\n", vma->vm_end - vma->vm_start, r->size);
    rbuf_dbg("sizeof(struct rbuf_shm) header=%zu\n", offsetof(struct rbuf_shm, data));
    rbuf_dbg("mmap: shm=%px size=%zu\n", r->shm, size);
    return remap_vmalloc_area(vma, r->vmem);
}

static inline bool ring_has_data(struct ring_dev *r)
{
    /*
     * Data is available if writer head != reader reserved tail.
     * Caller must hold r->lock OR otherwise ensure consistency.
     */
    return r->shm->head != r->shm->tail;
}

static bool ring_is_idle(struct ring_dev *r)
{
    if (!r || !r->shm)
        return false;

    /*
     * No writers currently blocked in
     * policy-sensitive wait paths.
     *
     * Threads already executing a wait policy
     * continue using the previously selected
     * behavior until re-entry.
     */
    if (atomic_read(&r->writers_waiting) != 0)
        return false;

    return true;
}

void ring_fill_info(struct ring_dev *r, struct rbuf_info *info)
{
    info->abi_version = RBUF_ABI_VERSION;
    info->size = r->size;
    info->cap = r->capacity;
    info->head = smp_load_acquire(&r->shm->head);
    info->tail = smp_load_acquire(&r->shm->tail);
    info->reader_policy = r->reader_policy;
    info->writer_policy = r->writer_policy;
    info->spin_policy = r->spin_policy;
    info->flags = 0; // info->flags = r->flags;
    info->numa_node = r->alloc_node;
    memset(info->reserved, 0, sizeof(info->reserved));
}

/* IOCTLs */
static long ring_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct ring_reader_ctx *fd_ctx;
    struct ring_dev *r;
    struct rbuf_info info;

    if (!(file->f_mode & FMODE_READ))
        return -ENOTTY;
    fd_ctx = file->private_data;
    if (!fd_ctx)
        return -EBADF;

    r = fd_ctx->dev;

    switch (cmd)
    {
        case RBUF_IOC_CLAIM: {
            unsigned long flags;

            if (!fd_ctx)
                return -EBADF;

            spin_lock_irqsave(&r->lock, flags);
            if (r->reader_fd)
            {
                /* already taken */
                spin_unlock_irqrestore(&r->lock, flags);
                if (r->reader_fd == fd_ctx)
                    return 0; /* already ours */
                return -EBUSY;
            }
            r->reader_fd = fd_ctx;
            fd_ctx->claimed = true;

            u64 head = smp_load_acquire(&r->shm->head);
            if (r->reader_policy == RBUF_READER_FULL)
            {
                /*
                * FULL: preserve unread history.
                * Reader starts at current global tail.
                * DO NOT modify shm->tail.
                */
                fd_ctx->reader_pos = smp_load_acquire(&r->shm->tail);
            }
            else
            {
                /*
                * FRESH (attach-to-head semantics)
                * Reader drops history and starts live.
                * Global tail must move to head.
                */
                fd_ctx->reader_pos = head;
                smp_store_release(&r->shm->tail, head);
            }

            spin_unlock_irqrestore(&r->lock, flags);
            wake_up_interruptible(&r->writeq);
            return 0;
        }

        case RBUF_IOC_RELEASE:
        {
            struct ring_reader_ctx *fd_ctx = file->private_data;
            unsigned long flags;

            if (!fd_ctx)
                return -EBADF;

            spin_lock_irqsave(&r->lock, flags);
            if (r->reader_fd != fd_ctx)
            {
                spin_unlock_irqrestore(&r->lock, flags);
                return -EPERM; /* not owner */
            }
            r->reader_fd = NULL;
            fd_ctx->claimed = false;
            spin_unlock_irqrestore(&r->lock, flags);
            wake_up_interruptible(&r->writeq);
            return 0;
        }

        case RBUF_IOC_INFO:
        {
            /* NOTE: ioctl is only allowed on reader FDs (file->f_mode & FMODE_READ is required above).
            * We already have fd_ctx and r computed earlier.
            */
            if (!r)
                return -ENODEV;
            ring_fill_info(r, &info);
            if (copy_to_user((void __user *)arg, &info, sizeof(info)))
                return -EFAULT;
            return 0;
        }

        case RBUF_IOC_ADVANCE:
        {
            uint64_t new_pos;
            if (copy_from_user(&new_pos, (void __user *)arg, sizeof(new_pos)))
                return -EFAULT;
            if (!fd_ctx->claimed)
                return -EPERM;
            if (new_pos > smp_load_acquire(&r->shm->head))
                return -EINVAL;
            fd_ctx->reader_pos = new_pos;
            smp_store_release(&r->shm->tail, new_pos);
            wake_up_interruptible(&r->writeq);
            return 0;
        }

        case RBUF_IOC_QUERY_STATE:
        {
            struct ringbuf_state st;
            struct ring_reader_ctx *reader;
            unsigned long flags;

            memset(&st, 0, sizeof(st));

            spin_lock_irqsave(&r->lock, flags);

            st.total_size = r->size;
            st.data_capacity = r->capacity;
            st.head = r->shm->head;
            st.tail = r->shm->tail;

            reader = r->reader_fd;
            st.claimed = !!reader;
            st.owner_pid = reader ? reader->owner_pid : 0;

            spin_unlock_irqrestore(&r->lock, flags);

            if (copy_to_user((void __user *)arg, &st, sizeof(st)))
                return -EFAULT;
            return 0;
        }

        case RBUF_IOC_SPIN_POLICY:
        {
            int user_pol;
            if (copy_from_user(&user_pol,
                            (void __user *)arg, sizeof(user_pol)))
                return -EFAULT;

            if (!rbuf_valid_value(SERIES_SPIN_POLICY, user_pol))
                return -EINVAL;

            /* Only the claimed reader may modify device policy */
            if (file->private_data != r->reader_fd)
                return -EPERM;

            /* Only when no writer activity is in-flight */
            if (!ring_is_idle(r)) {
                return -EBUSY;
            }

            WRITE_ONCE(r->spin_policy, user_pol);

            
            /*
            * Encourage sleeping writers to re-evaluate
            * wait conditions under the new policy.
            */
            if (waitqueue_active(&r->writeq))
                wake_up_interruptible(&r->writeq);

            return 0;
        }

        case RBUF_IOC_FLUSH:
        {
            unsigned long flags;
            u64 head, tail, flushed;

            spin_lock_irqsave(&r->lock, flags);

            head = smp_load_acquire(&r->shm->head);
            tail = smp_load_acquire(&r->shm->tail);
            flushed = head - tail;

            smp_store_release(&r->shm->tail, head);

            if (fd_ctx && fd_ctx->claimed && fd_ctx->dev == r)
                fd_ctx->reader_pos = head;

            spin_unlock_irqrestore(&r->lock, flags);

            atomic64_inc(&r->stat_flushes);
            atomic64_add(flushed, &r->stat_bytes_flushed);

            wake_up_interruptible(&r->readq);
            wake_up_interruptible(&r->writeq);
            return 0;
        }

        default:
            return -ENOTTY;
    }
}

static int parse_policy(const char *value,
                        int category,
                        int default_value,
                        const char *field)
{
    int ret;

    if (!value || !value[0])
        return default_value;

    ret = rbuf_from_str(value, category);

    if (ret < 0) {
        pr_err("ringbuf: invalid %s '%s'\n",
               field, value);
        return ret;
    }

    return ret;
}

static struct ring_dev *ring_find_by_name(const char *canonical)
{
    int i;

    for (i = 0; i < MAX_DEVICES; ++i) {
        if (ring_table[i] &&
            strcmp(ring_table[i]->name, canonical) == 0)
            return ring_table[i];
    }

    return NULL;
}

static int ring_alloc_minor(void)
{
    int i;

    for (i = 0; i < MAX_DEVICES; ++i) {
        if (!ring_table[i])
            return i;
    }

    return -ENOSPC;
}


/* ---------- create/destroy and sysfs handlers ---------- */

static int ring_create_one(const char *name,
                           unsigned int req_size,
                           int numa_node,
                           int rpol,
                           int wpol,
                           int spol,
                           int device_id)
{
    struct ring_dev *r = NULL;
    int minor = -1;
    int ret = 0;
    dev_t devno;
    char canonical[MAX_NAME_LEN];
    u32 data_cap;
    u32 cap_pow2;
    int node_to_use = numa_node;

    if (!name || !*name || strlen(name) >= MAX_NAME_LEN)
        return -EINVAL;

    if (req_size < PAGE_SIZE)
        req_size = PAGE_SIZE;

    data_cap = (u32)req_size;

    if (node_to_use < 0)
        node_to_use = numa_node_id();

    cap_pow2 = next_pow2(data_cap);
#ifdef DATA_CAP
    if (cap_pow2 > (u32)DATA_CAP) {
        pr_warn("ringbuf: requested %u > DATA_CAP(%u); clamping to %u\n",
                (unsigned int)data_cap,
                (unsigned int)DATA_CAP,
                (unsigned int)DATA_CAP);
        cap_pow2 = (u32)DATA_CAP;
    }
#endif
    if (cap_pow2 == 0)
        return -EINVAL;

    if (rpol < 0 || rpol >= RBUF_READER_POLICY_MAX)
        return -EINVAL;
    if (wpol < 0 || wpol >= RBUF_WRITER_POLICY_MAX)
        return -EINVAL;
    if (spol < 0 || spol >= RBUF_SPIN_POLICY_MAX)
        return -EINVAL;

    r = kzalloc(sizeof(*r), GFP_KERNEL);
    if (!r)
        return -ENOMEM;

    atomic64_set(&r->stat_writes, 0);
    atomic64_set(&r->stat_reads, 0);
    atomic64_set(&r->stat_drops, 0);
    atomic64_set(&r->stat_max_used, 0);
    atomic64_set(&r->stat_bytes_written, 0);
    atomic64_set(&r->stat_bytes_read, 0);
    atomic64_set(&r->stat_copy_ns, 0);
    atomic64_set(&r->stat_copy_calls, 0);
    atomic64_set(&r->stat_flushes, 0);
    atomic64_set(&r->stat_bytes_flushed, 0);

    r->dbgdir = NULL;
    spin_lock_init(&r->lock);
    init_waitqueue_head(&r->readq);
    init_waitqueue_head(&r->writeq);
    r->reader_fd = NULL;
    kref_init(&r->refcount);
    atomic_set(&r->writers_waiting, 0);

    r->capacity = cap_pow2;
    r->mask = cap_pow2 - 1;
    r->size = PAGE_ALIGN(sizeof(struct rbuf_shm) + (size_t)r->capacity);

#ifdef CONFIG_NUMA
#ifdef HAVE_VMALLOC_NODE
    r->vmem = vmalloc_node(r->size, node_to_use);
#else
    r->vmem = vmalloc_user(r->size);
#endif
#else
    r->vmem = vmalloc_user(r->size);
#endif

    if (!r->vmem) {
        pr_err("ringbuf: vmalloc failed\n");
        ret = -ENOMEM;
        goto out_free;
    }

    memset(r->vmem, 0, r->size);
    r->shm = (struct rbuf_shm *)r->vmem;

    scnprintf(canonical, sizeof(canonical), "%s_%s", DEVICE_BASENAME, name);
    scnprintf(r->name, sizeof(r->name), "%s", canonical);
    r->alloc_node = node_to_use;

    r->reader_policy = rpol;
    r->writer_policy = wpol;
    r->spin_policy = spol;

    mutex_lock(&minor_table_lock);

    if (ring_find_by_name(canonical)) {
        pr_err("ringbuf: device '%s' already exists\n",
            canonical);
        ret = -EEXIST;
        goto out_unlock_free;
    }

    if (device_id >= 0) {
        if (device_id >= MAX_DEVICES) {
            pr_err("ring_create_one: invalid device_id: %d is greater than current limit (%d)\n",
                   device_id, MAX_DEVICES);
            ret = -EINVAL;
            goto out_unlock_free;
        }
        if (ring_table[device_id]) {
            pr_err("ring_create_one: device_id %d already in use by %s\n",
                   device_id, ring_table[device_id]->name);
            ret = -EEXIST;
            goto out_unlock_free;
        }
        minor = device_id;
    } else {
        minor = ring_alloc_minor();
        if (minor < 0) {
            pr_err("ringbuf: no free minors\n");
            ret = -ENOSPC;
            goto out_unlock_free;
        }
    }

    r->minor = minor;
    devno = MKDEV(MAJOR(ring_dev_first), minor);

    cdev_init(&r->cdev, &ring_fops);
    r->cdev.owner = THIS_MODULE;
    ret = cdev_add(&r->cdev, devno, 1);
    if (ret) {
        pr_err("ringbuf: cdev_add failed (%d)\n", ret);
        goto out_unlock_free;
    }

    r->device = device_create(ring_class, NULL, devno, NULL, "%s", r->name);
    if (IS_ERR(r->device)) {
        ret = PTR_ERR(r->device);
        pr_err("ringbuf: device_create failed (%d)\n", ret);
        cdev_del(&r->cdev);
        goto out_unlock_free;
    }

    smp_store_release(&r->shm->head, 0);
    smp_store_release(&r->shm->tail, 0);

    scnprintf(r->last_add_message, sizeof(r->last_add_message),
              (cap_pow2 != (u32)req_size) ? "rounded %u -> %u" : "ok",
              (unsigned int)req_size, (unsigned int)cap_pow2);

    dev_set_drvdata(r->device, r);

    ret = device_create_file(r->device, &dev_attr_last_add_message);
    if (ret)
        pr_warn("%s: device_create_file(last_add_message) failed: %d\n",
                r->name, ret);

    ret = device_create_file(r->device, &dev_attr_device_alloc_node);
    if (ret)
        pr_warn("%s: device_create_file(device_alloc_node) failed: %d\n",
                r->name, ret);

    ringbuf_debugfs_add(r);

    ring_table[minor] = r;
    mutex_unlock(&minor_table_lock);

    pr_info("ringbuf: created /dev/%s (major=%d minor=%d r->capacity=%u node=%d, r->size=%zu, cap_of(r)=%llu), r->reader_policy=%d, r->writer_policy=%d, r->spin_policy=%d)\n",
            r->name, MAJOR(devno), MINOR(devno), r->capacity, r->alloc_node, r->size, cap_of(r), r->reader_policy, r->writer_policy, r->spin_policy);

    return 0;

out_unlock_free:
    mutex_unlock(&minor_table_lock);
out_free:
    if (r) {
        if (r->vmem)
            vfree(r->vmem);
        kfree(r);
    }
    return ret;
}

void ring_dev_release(struct kref *ref)
{
    struct ring_dev *r = container_of(ref, struct ring_dev, refcount);
    if (WARN_ON(!r))
        return;
    if (WARN_ON(r->dead == false))
        pr_warn("releasing non-dead device?\n");
        
    pr_info("ringbuf: freeing %s\n", r->name);

    if (r->dbgdir)
        debugfs_remove_recursive(r->dbgdir);

    pr_info("ringbuf: device_destroy(%s)\n", r->name);
    if (r->device)
        device_destroy(ring_class, MKDEV(MAJOR(ring_dev_first), r->minor));

    cdev_del(&r->cdev);

    if (r->vmem)
        vfree(r->vmem);

    kfree(r);
}

static int ring_destroy_one(const char *name)
{
    struct ring_dev *r = NULL;
    int i;

    mutex_lock(&minor_table_lock);

    for (i = 0; i < MAX_DEVICES; ++i) {
        if (ring_table[i] && strcmp(ring_table[i]->name, name) == 0) {
            r = ring_table[i];
            ring_table[i] = NULL; // detach first
            break;
        }
    }
    
    if (!r) {
        mutex_unlock(&minor_table_lock);
        return -ENOENT;
    }

    mutex_unlock(&minor_table_lock);

    WRITE_ONCE(r->dead, true);
    wake_up_all(&r->readq);
    wake_up_all(&r->writeq);

    pr_info("ringbuf: logically removed %s\n", name);

    /* drop base ref */
    kref_put(&r->refcount, ring_dev_release);

    return 0;
}

/* trim helper */
static void trim_ws(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

/* expects 'name=foo size=4096 node=1' or 'foo' */
static ssize_t add_device_store(const struct class *class,
                                const struct class_attribute *attr,
                                const char *buf, size_t count)
{
    char tmp[256];
    char key[64], val[128];
    char name_in[64] = {0};
    unsigned int size = PAGE_SIZE;
    int node = -1;
    char rpol[32] = "";
    char wpol[32] = "";
    char spol[32] = "";
    int device_id = -1;
    char *p, *tok;
    int ret;

    if (count == 0 || count >= sizeof(tmp))
        return -EINVAL;

    memcpy(tmp, buf, count);
    tmp[count] = '\0';
    trim_ws(tmp);

    if (strchr(tmp, '=') == NULL) {
        strscpy(name_in, tmp, sizeof(name_in));
    } else {
        p = tmp;
        while ((tok = strsep(&p, " \t")) != NULL) {
            if (!tok[0])
                continue;

            if (sscanf(tok, "%63[^=]=%127s", key, val) != 2)
                continue;

            if (!strcmp(key, "name")) {
                strscpy(name_in, val, sizeof(name_in));
            } else if (!strcmp(key, "size")) {
                if (kstrtou32(val, 10, &size) || size == 0) {
                    pr_warn("ringbuf: hot add-device: invalid size '%s'; using PAGE_SIZE\n",
                            val);
                    size = PAGE_SIZE;
                }
            } else if (!strcmp(key, "node")) {
                if (kstrtoint(val, 10, &node)) {
                    pr_warn("ringbuf: hot add-device: invalid node '%s'; using -1\n",
                            val);
                    node = -1;
                }
            } else if (!strcmp(key, "reader_policy")) {
                strscpy(rpol, val, sizeof(rpol));
            } else if (!strcmp(key, "writer_policy")) {
                strscpy(wpol, val, sizeof(wpol));
            } else if (!strcmp(key, "spin_policy")) {
                strscpy(spol, val, sizeof(spol));
            } else if (!strcmp(key, "device_minor")) {
                u32 tmp_minor;

                if (kstrtou32(val, 10, &tmp_minor) || tmp_minor >= MAX_DEVICES) {
                    pr_warn("ringbuf: hot add-device: invalid device_minor '%s'; auto-assigning\n",
                            val);
                    device_id = -1;
                } else {
                    device_id = (int)tmp_minor;
                }
            }
        }
    }

    if (!name_in[0])
        return -EINVAL;

    if (!strncmp(name_in, DEVICE_BASENAME "_", strlen(DEVICE_BASENAME "_")))
        memmove(name_in,
                name_in + strlen(DEVICE_BASENAME "_"),
                strlen(name_in) - strlen(DEVICE_BASENAME "_") + 1);

    ret = ring_create_one(name_in, size, node,
                          parse_policy(rpol[0] ? rpol : default_reader_policy, SERIES_READER_POLICY, DEFAULT_READER_POLICY, "default_reader_policy"),
                          parse_policy(wpol[0] ? wpol : default_writer_policy, SERIES_WRITER_POLICY, DEFAULT_WRITER_POLICY, "default_writer_policy"),
                          parse_policy( spol[0] ? spol : default_spin_policy, SERIES_SPIN_POLICY, DEFAULT_SPIN_POLICY, "default_spin_policy"),
                          device_id);

    if (ret) {
        pr_err("ringbuf: add_device_store: ring_create_one(%s,size=%u,node=%d) failed: %d\n",
               name_in, size, node, ret);
        return ret;
    }

    return count;
}

static ssize_t remove_device_store(const struct class *class,
                                   const struct class_attribute *attr,
                                   const char *buf, size_t count)
{
    char tmp[128];
    char name_in[80];
    char suffix[80];

    if (count == 0 || count >= sizeof(tmp))
        return -EINVAL;
    memcpy(tmp, buf, count);
    tmp[count] = '\0';
    trim_ws(tmp);
    strim(tmp);

    if (sscanf(tmp, "%79s", name_in) != 1)
        return -EINVAL;

    if (!strncmp(name_in, DEVICE_BASENAME "_", strlen(DEVICE_BASENAME "_")))
        strscpy(suffix, name_in + strlen(DEVICE_BASENAME "_"), sizeof(suffix));
    else
        strscpy(suffix, name_in, sizeof(suffix));

    {
        char fullname[MAX_NAME_LEN];
        scnprintf(fullname, sizeof(fullname), DEVICE_BASENAME "_%s", suffix);
        return ring_destroy_one(fullname) ? -EINVAL : (ssize_t)count;
    }
}

static ssize_t major_show(const struct class *class,
                          const struct class_attribute *attr,
                          char *buf)
{
    return sysfs_emit(buf, "%d\n", MAJOR(ring_dev_first));
}

/* devnode callback to set default mode */
static char *ringbuf_devnode(const struct device *dev, umode_t *mode)
{
    if (mode)
        *mode = dev_mode;
    return NULL;
}

/* class-level sysfs to change default_spin_policy/spin_budget */
static ssize_t default_spin_policy_show(const struct class *class, const struct class_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%s\n", default_spin_policy);
}
static ssize_t default_spin_policy_store(
    const struct class *class,
    const struct class_attribute *attr,
    const char *buf,
    size_t count)
{
    char tmp[sizeof(default_spin_policy)];
    int mode;
    size_t len = min(count, sizeof(tmp) - 1);

    memcpy(tmp, buf, len);
    tmp[len] = '\0';

    trim_ws(tmp);

    mode = rbuf_from_str(tmp, SERIES_SPIN_POLICY);

    if (mode < 0)
        return -EINVAL;

    strscpy(default_spin_policy,
        tmp,
        sizeof(default_spin_policy));

    pr_info("ringbuf: default_spin_policy set to %s (%d)\n",
            tmp, mode);

    return count;
}
static CLASS_ATTR_RW(default_spin_policy);

static ssize_t spin_budget_show(const struct class *class, const struct class_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%u\n", spin_budget);
}
static ssize_t spin_budget_store(const struct class *class, const struct class_attribute *attr, const char *buf, size_t count)
{
    unsigned int v;
    if (kstrtouint(buf, 10, &v))
        return -EINVAL;
    spin_budget = v;
    pr_info("ringbuf: spin_budget set to %u\n", spin_budget);
    return count;
}
static CLASS_ATTR_RW(spin_budget);

/* init / exit */
static int __init ringbuf_init(void)
{
    int i, ret = 0;

    /* --- char device region --- */
    ret = alloc_chrdev_region(&ring_dev_first, 0, MAX_DEVICES, DEVICE_BASENAME);
    if (ret)
    {
        pr_err("ringbuf: alloc_chrdev_region failed (%d)\n", ret);
        return ret;
    }

    /* --- class --- */
    ring_class = class_create(DEVICE_BASENAME);
    if (IS_ERR(ring_class))
    {
        ret = PTR_ERR(ring_class);
        unregister_chrdev_region(ring_dev_first, MAX_DEVICES);
        return ret;
    }

    ring_class->devnode = ringbuf_devnode;

    ret = class_create_file(ring_class, &class_attr_major);
    ret = class_create_file(ring_class, &class_attr_add_device);
    ret = class_create_file(ring_class, &class_attr_remove_device);
    ret = class_create_file(ring_class, &class_attr_default_spin_policy);
    ret = class_create_file(ring_class, &class_attr_spin_budget);

    /* --- debugfs sub-system --- */
    ret = ringbuf_debugfs_init();
    if (ret)
        pr_warn("ringbuf: debugfs disabled\n");

    /* --- init table --- */
    for (i = 0; i < MAX_DEVICES; ++i)
        ring_table[i] = NULL;

    /* --- compute device count --- */
    int count = num_devices;

    if (device_sizes_count > count)
        count = device_sizes_count;
    if (device_names_count > count)
        count = device_names_count;
    if (device_minors_count > count)
        count = device_minors_count;
    if (reader_policies_count > count)
        count = reader_policies_count;
    if (writer_policies_count > count)
        count = writer_policies_count;
    if (spin_policies_count > count)
        count = spin_policies_count;

    if (count > MAX_DEVICES)
        count = MAX_DEVICES;

    /* --- validate device_minors --- */
    if (device_minors_count > 0)
    {
        int *used = kcalloc(MAX_DEVICES, sizeof(int), GFP_KERNEL);
        if (!used)
        {
            ret = -ENOMEM;
            goto fail;
        }

        for (i = 0; i < device_minors_count && i < count; ++i)
        {
            int id = device_minors[i];
            if (id >= 0)
            {
                if (id >= MAX_DEVICES)
                {
                    pr_err("ringbuf: device_minors[%d]=%d out of range (%d max)\n", i, id, MAX_DEVICES);
                    ret = -EINVAL;
                    kfree(used);
                    goto fail;
                }
                if (used[id])
                {
                    pr_err("ringbuf: duplicate device_id: %d\n", id);
                    ret = -EINVAL;
                    kfree(used);
                    goto fail;
                }
                used[id] = 1;
            }
        }
        kfree(used);
    }

    /* --- create devices --- */
    for (i = 0; i < count; ++i)
    {
        char namebuf[64];

        /* name */
        if (i < device_names_count && device_names[i] && strlen(device_names[i]) > 0)
            scnprintf(namebuf, sizeof(namebuf), "%s", device_names[i]);
        else
            scnprintf(namebuf, sizeof(namebuf), "ringbuf%d", i);

        /* size */
        unsigned int size = (i < device_sizes_count) ? device_sizes[i] : PAGE_SIZE;

        /* minor */
        int requested_minor;
        if (i < device_minors_count)
        {
            if (device_minors[i] < -1 || device_minors[i] >= MAX_DEVICES)
            {
                pr_err("ringbuf: %s:%s:%d: invalid requested device_minors %d; must be -1 or [0..%d)", __FILE__, __func__, __LINE__, device_minors[i], MAX_DEVICES);
                ret = -EINVAL;
                goto fail;
            }
            requested_minor = device_minors[i];
        }
        else
            requested_minor = -1;

        int rpol = DEFAULT_READER_POLICY;
        if (i < reader_policies_count && reader_policies[i]) {
            rpol = parse_policy(reader_policies[i],
                                SERIES_READER_POLICY,
                                DEFAULT_READER_POLICY,
                                "reader_policy");
            if (rpol < 0)
                goto fail;
        }

        int wpol = DEFAULT_WRITER_POLICY;
        if (i < writer_policies_count && writer_policies[i]) {
            wpol = parse_policy(writer_policies[i],
                                SERIES_WRITER_POLICY,
                                DEFAULT_WRITER_POLICY,
                                "writer_policy");
            if (wpol < 0)
                goto fail;
        }

        int spol = DEFAULT_SPIN_POLICY;
        if (i < spin_policies_count && spin_policies[i]) {
            spol = parse_policy(spin_policies[i],
                                SERIES_SPIN_POLICY,
                                DEFAULT_SPIN_POLICY,
                                "spin_policy");
            if (spol < 0)
                goto fail;
        }

        int node = default_numa_node;
        if (i < device_nodes_count)
            node = device_nodes[i];

        /* create */
        ret = ring_create_one(namebuf, size, node, rpol, wpol, spol, requested_minor);

        if (ret)
        {
            pr_err("ringbuf: failed create %s (%d)\n",
                   namebuf, ret);
            goto fail;
        }
    }

    pr_info("ringbuf: loaded (%d devices)\n", count);
    return 0;

fail:
    for (i = 0; i < MAX_DEVICES; ++i)
        if (ring_table[i])
            ring_destroy_one(ring_table[i]->name);

    ringbuf_debugfs_exit();

    if (ring_class)
        class_destroy(ring_class);

    unregister_chrdev_region(ring_dev_first, MAX_DEVICES);

    return ret;
}

static void __exit ring_exit(void)
{
    int i;

    ringbuf_unloading = true;

    for (i = 0; i < MAX_DEVICES; ++i) {
        struct ring_dev *r = ring_table[i];

        if (!r)
            continue;

        ring_table[i] = NULL; // detach

        WRITE_ONCE(r->dead, true);
        wake_up_all(&r->readq);
        wake_up_all(&r->writeq);

        kref_put(&r->refcount, ring_dev_release);
    }

    ringbuf_debugfs_exit();

    class_remove_file(ring_class, &class_attr_add_device);
    class_remove_file(ring_class, &class_attr_remove_device);
    class_remove_file(ring_class, &class_attr_major);
    class_remove_file(ring_class, &class_attr_default_spin_policy);
    class_remove_file(ring_class, &class_attr_spin_budget);

    if (ring_class)
        class_destroy(ring_class);

    unregister_chrdev_region(ring_dev_first, MAX_DEVICES);

    pr_info("ringbuf: module unloaded\n");
}
module_init(ringbuf_init);
module_exit(ring_exit);

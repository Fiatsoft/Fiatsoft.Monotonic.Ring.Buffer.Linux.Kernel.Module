/* Copyright (c) 2026 fiatsoft.com.
 * SPDX-License-Identifier: GPL-2.0-only */

/* common/ringbuf.h - shared header for ringbuf kernel module and userland tests */

#ifndef RINGBUF_H
#define RINGBUF_H

#include <linux/ioctl.h>
#include "ringbuf_abi.h"
#include "ringbuf_abi_impl.h"

extern bool ringbuf_unloading;

#define RBUF_IOC_MAGIC 'r'
#define RBUF_IOC_CLAIM       _IO(RBUF_IOC_MAGIC,  0)
#define RBUF_IOC_ADVANCE     _IOW(RBUF_IOC_MAGIC, 1, uint32_t)
#define RBUF_IOC_INFO        _IOR(RBUF_IOC_MAGIC, 2, struct rbuf_info)
#define RBUF_IOC_RELEASE     _IO(RBUF_IOC_MAGIC,  3)
#define RBUF_IOC_QUERY_STATE _IOR(RBUF_IOC_MAGIC, 4, struct ringbuf_state)
#define RBUF_IOC_SPIN_POLICY _IOW(RBUF_IOC_MAGIC, 5, int)
#define RBUF_IOC_FLUSH       _IOW(RBUF_IOC_MAGIC, 6, int)

#endif

/* Copyright (c) 2026 fiatsoft.com.
 * SPDX-License-Identifier: GPL-2.0-only */

#ifndef _RINGBUF_DEBUGFS_H_
#define _RINGBUF_DEBUGFS_H_

struct ring_dev;

/* module lifecycle */
int  ringbuf_debugfs_init(void);
void ringbuf_debugfs_exit(void);

/* per-device hooks */
void ringbuf_debugfs_add(struct ring_dev *r);
void ringbuf_debugfs_remove(struct ring_dev *r);

/* info file */
char *get_binary_str(uint32_t val, char *buf);

#endif
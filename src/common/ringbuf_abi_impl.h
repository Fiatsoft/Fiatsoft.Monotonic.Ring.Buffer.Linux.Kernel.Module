/* Copyright (c) 2026 fiatsoft.com.
 * SPDX-License-Identifier: GPL-2.0-only */

/* common/ringbuf_abi_impl.h */
static inline const char* rbuf_to_str_impl(int val)
{
    switch (val) {
#define X(id, str, cat, val) case id: return str;
        RBUF_MASTER_LIST(X)
#undef X
    default:
        return "unknown";
    }
}

static inline int rbuf_from_str_impl(const char *s, int category)
{
#define X(id, str, cat, val) \
    if ((cat == category) && strcmp(s, str) == 0) return id;
    RBUF_MASTER_LIST(X)
#undef X
    return -1;
}

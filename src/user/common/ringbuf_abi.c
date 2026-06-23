// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

/* user/common/ringbuf_abi.c */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#include "ringbuf_abi.h"
#include "ringbuf_abi_impl.h"

const char* rbuf_to_str(int val) { return rbuf_to_str_impl(val); }
int rbuf_from_str(const char *s, int cat) { return rbuf_from_str_impl(s, cat); }
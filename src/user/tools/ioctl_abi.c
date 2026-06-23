// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#include <stdio.h>
#include "ringbuf.h"
int main(){ printf("CLAIM 0x%x ADV 0x%lx INFO 0x%lx\n", RBUF_IOC_CLAIM, RBUF_IOC_ADVANCE, RBUF_IOC_INFO); return 0; }
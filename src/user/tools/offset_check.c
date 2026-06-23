// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "ringbuf.h"

int main(void)
{
       struct rbuf_shm rbuf_shm_;
       printf("sizeof head = %zu\n",
              sizeof(rbuf_shm_.head));

       printf("OFF_HEAD     = %zu\n",
              offsetof(struct rbuf_shm, head));
       printf("OFF_TAIL = %zu\n",
              offsetof(struct rbuf_shm, tail));
       printf("OFF_DATA     = %zu\n",
              offsetof(struct rbuf_shm, data));

       return 0;
}
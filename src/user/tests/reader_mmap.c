// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <stdbool.h>
#include <getopt.h>
#include <sys/wait.h>

#include "ringbuf.h"
#include "helpers.h" 


static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --dev PATH [options]\n", prog);
    printf("\n");
    printf("Required:\n");
    printf("  --dev PATH          ring device (name, dev-name or full-path)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --duration SECONDS  test duration (default 5)\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s --dev /dev/ringbuf_device0\n", prog);
} 

static inline uint32_t mod_distance(uint32_t from, uint32_t to, uint32_t cap)
{
    return (to >= from) ? (to - from) : (cap - from + to);
}

int main(int argc, char **argv)
{
    char dev_path[PATH_MAX];
    const char *dev = NULL;
    int duration = -1;
    static struct option opts[] = {
        {"dev",  required_argument, 0, 'd'},
        {"duration",  required_argument, 0, 's'},
        {"help", no_argument,       0, 'v'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:s:h", opts, NULL)) != -1) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 's': duration = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:
            fprintf(stderr, "usage: %s --dev PATH [--duration SEC]\n", argv[0]);
            return 2;
        }
    }
    if (!dev) {
        fprintf(stderr, "%s: --dev required\n", __FILE__);
        usage(argv[0]);
        return 2;
    }

    if(!normalize_dev(dev_path, sizeof(dev_path), dev)) {
        fprintf(stderr, "%s: --dev '%s' not found\n", __FILE__, dev);
        errno = EINVAL;
        perror("invalid --dev parameter");
        return 2;
    }
    int fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        handle_failed_ringbuf_dev_open(dev_path, errno);
        die_ec("open", errno, 1);
    }

    if (ioctl(fd, RBUF_IOC_CLAIM) != 0) {
        perror("ioctl(CLAIM)");
        close(fd);
        return 1;
    }

    // uint32_t info[3];
    struct rbuf_info info;
    if (ioctl(fd, RBUF_IOC_INFO, &info) != 0) {
        perror("ioctl(INFO)");
        goto out_release;
    }
    
    uint32_t total_size = info.size;
    uint32_t data_cap   = info.cap;
    if (data_cap == 0) {
        fprintf(stderr, "ioctl(INFO) returned zero data_cap\n");
        goto out_release;
    }
    
    uint8_t *base = mmap(NULL, total_size, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        perror("mmap");
        goto out_release;
    }
    
    uint64_t *headp = (uint64_t *)(base + OFF_HEAD);
    uint64_t *tailp = (uint64_t *)(base + OFF_TAIL);
    uint8_t  *data  = (uint8_t  *)(base + OFF_DATA);
    
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    char outbuf[4096];
    size_t out_idx = 0;

    if (duration > 0) 
        alarm(duration);

    uint64_t our_pos = __atomic_load_n(tailp, __ATOMIC_ACQUIRE);
    for (;;) {
        
        uint64_t head = __atomic_load_n(headp, __ATOMIC_ACQUIRE);
        uint64_t tail = __atomic_load_n(tailp, __ATOMIC_ACQUIRE);

        /* GAP DETECTION */
        if (our_pos < tail) {
            our_pos = tail;
        }

        if (our_pos == head) {
            poll(&pfd, 1, -1);
            continue;
        }
        
        while (our_pos < head) {
            
            uint64_t index = our_pos & (data_cap - 1);  // mask only here
            unsigned char c = data[index];

            write(STDOUT_FILENO, &c, 1);

            our_pos++;
        }
        
        /* Tell kernel we consumed up to our_pos */
        uint64_t adv = our_pos;
        if (ioctl(fd, RBUF_IOC_ADVANCE, &adv) != 0)
            perror("ADVANCE");
        
    }
        
    /* flush leftover */
    if (out_idx) write(STDOUT_FILENO, outbuf, out_idx);

    munmap(base, total_size);

out_release:
    ioctl(fd, RBUF_IOC_RELEASE);
    close(fd);
    return 0;
}

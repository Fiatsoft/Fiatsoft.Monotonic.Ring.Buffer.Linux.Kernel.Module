// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <getopt.h>

#include "ringbuf.h"
#include "helpers.h"

//todo
static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --dev PATH\n", prog);
    printf("\n");
    printf("Required:\n");
    printf("  --dev PATH          ring device (name, dev-name or full-path)\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s --dev /dev/ringbuf_device0\n", prog);
} 


int main(int argc, char **argv)
{
    char dev_path[PATH_MAX];
    const char *dev = NULL;
    int policy;
    char buf[16];
    uint64_t t0, t1;

    static struct option opts[] = {
        {"dev",  required_argument, 0, 'd'},
        {"help", no_argument,       0, 'v'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:h", opts, NULL)) != -1) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:
            fprintf(stderr, "usage: %s --dev PATH\n", argv[0]);
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
        perror("CLAIM"); return 1;
    }

    /* -------- BUSY -------- */
    policy = RBUF_SPIN_BUSY;
    ioctl(fd, RBUF_IOC_SPIN_POLICY, &policy);

    t0 = now_ns();
    read(fd, buf, sizeof(buf));   /* empty ring */
    t1 = now_ns();

    printf("BUSY latency: %llu ns\n",
           (unsigned long long)(t1 - t0));

    /* -------- SLEEP -------- */
    policy = RBUF_SPIN_SLEEP;
    ioctl(fd, RBUF_IOC_SPIN_POLICY, &policy);

    t0 = now_ns();
    read(fd, buf, sizeof(buf));   /* empty ring */
    t1 = now_ns();

    printf("SLEEP latency: %llu ns\n",
           (unsigned long long)(t1 - t0));

    ioctl(fd, RBUF_IOC_RELEASE);
    close(fd);

    return 0;
}
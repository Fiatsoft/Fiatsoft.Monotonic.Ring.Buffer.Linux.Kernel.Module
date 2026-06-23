// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <getopt.h>
#include <sys/ioctl.h>

#include "helpers.h"
#include "ringbuf_abi.h"

static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --dev DEVICE\n", prog);
}

int main(int argc, char **argv)
{
    char dev_path[PATH_MAX];
    const char *dev = NULL;

    static struct option opts[] = {
        {"dev", required_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };

    int c;

    while ((c = getopt_long(argc, argv, "d:h", opts, NULL)) != -1) {
        switch (c) {
        case 'd':
            dev = optarg;
            break;

        case 'h':
            usage(argv[0]);
            return 0;

        default:
            return 2;
        }
    }

    if (!dev) {
        usage(argv[0]);
        return 2;
    }

    if (!normalize_dev(dev_path, sizeof(dev_path), dev)) {
        fprintf(stderr, "invalid device\n");
        return 2;
    }

    int fd = open(dev_path, O_RDONLY);

    if (fd < 0) {
        handle_failed_ringbuf_dev_open(dev_path, errno);
        die_ec("open", errno, 1);
    }

    if (ioctl(fd, RBUF_IOC_FLUSH) < 0) {
        perror("ioctl(FLUSH)");
        close(fd);
        return 1;
    }

    printf("flushed: %s\n", dev_path);

    close(fd);
    return 0;
}
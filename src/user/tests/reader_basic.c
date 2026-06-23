// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <getopt.h>

#include "ringbuf.h"
#include "helpers.h"

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

int main(int argc, char **argv) {
    char dev_path[PATH_MAX];
    const char *dev = NULL;
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
        close(fd);
        die_c("ioctl(CLAIM)",1);
    }

    char buf[128];
    ssize_t n;

    while (1) {
        n = read(fd, buf, sizeof(buf) - 1);
        if (n < 0) {
            perror("read");
            close(fd);
            return 1;
        } else if (n == 0) {
            // EOF — should not normally happen for character device
            printf("[reader] EOF\n");
            break;
        }

        buf[n] = '\0';
        printf("[reader] got: %s", buf);
    }

    close(fd);
    return 0;
}

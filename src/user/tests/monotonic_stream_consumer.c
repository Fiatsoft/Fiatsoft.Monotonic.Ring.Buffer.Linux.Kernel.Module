// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <limits.h>

#include "ringbuf.h"
#include "helpers.h"

#define BUF_SZ 4096

static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --dev PATH [options]\n\n", prog);
    printf("\n");
    printf("Required:\n");
    printf("  --dev PATH          ring device (name, dev-name or full-path)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --bytes N           total bytes to read\n");
    printf("  --verify            validate stream correctness\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s --dev /dev/ringbuf_device0 --bytes 1048576 --verify\n", prog);
}

int main(int argc, char **argv) {
    char dev_path[PATH_MAX];
    const char *dev = NULL;
    size_t total = 0;
    size_t limit = 0;
    int verify = 0;

    static struct option opts[] = {
        {"dev",    required_argument, 0, 'd'},
        {"bytes",  required_argument, 0, 'b'},
        {"verify", no_argument,       0, 'v'},
        {"help",   no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "b:vd:", opts, NULL)) != -1) {
        switch (c) {
        case 'b': limit = strtoull(optarg, NULL, 10); break;
        case 'v': verify = 1; break;
        case 'd': dev = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:
            fprintf(stderr, "usage: %s --dev PATH --bytes N [--verify]\n", argv[0]);
            usage(argv[0]);
            return 2;
        }
    }
    if (!dev) {
        fprintf(stderr, "%s: --dev required\n", __FILE__);
        usage(argv[0]);
        return 2;
    }
    if (!limit) {
        fprintf(stderr, "%s: --bytes required\n", __FILE__);
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

    uint8_t buf[BUF_SZ];
    struct rbuf_info info;
    uint64_t expected_pos;
    
    if (ioctl(fd, RBUF_IOC_INFO, &info) < 0) {
        perror("ioctl INFO");
        return 1;
    } 
    expected_pos = info.head;

    while (total < limit) {
        if (ioctl(fd, RBUF_IOC_INFO, &info) < 0) {
            perror("ioctl INFO");
            return 1;
        }
        uint8_t expect = 0;

        
        if (info.tail > expected_pos) {
            printf("consumer: GAP from byte %zu, extending to byte %zu\n", info.head, info.tail);
            die("GAP detected");
            return 1;
        }

        ssize_t n = read(fd, buf, sizeof(buf));
        expected_pos += n;
        if (n < 0) die("read");
        if (n == 0) continue;

        if (verify) {
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] != expect) {
                    fprintf(stderr,
                        "VERIFY FAIL: got %u expected %u at offset %zu\n",
                        buf[i], expect, total + i);
                    return 1;
                }
                expect++;
            }
        }

        total += n;

    }

    close(fd);
    printf("consumer: PASS (%zu bytes)\n", total);
    return 0;
}
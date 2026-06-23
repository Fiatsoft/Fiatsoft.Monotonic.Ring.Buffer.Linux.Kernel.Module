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
#include <limits.h>
#include <time.h>

#include "ringbuf.h"
#include "helpers.h" 

#define BUF_SZ 4096

static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --dev PATH [options]\n", prog);
    printf("\n");
    printf("Required:\n");
    printf("  --dev PATH          ring device (name, dev-name or full-path)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --bytes N           total bytes to produce\n");
    printf("  --pattern           emit deterministic pattern\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s --dev /dev/ringbuf_device0 --bytes 1048576 --verify\n", prog);
}

int main(int argc, char **argv) {
    char dev_path[PATH_MAX];
    const char *dev = NULL;
    size_t total = 0;
    size_t limit = 0;
    // int pattern = 0;

    static struct option opts[] = {
        {"bytes",   required_argument, 0, 'b'},
        // {"pattern", no_argument,       0, 'p'},
        {"dev",     required_argument, 0, 'd'},
        {"help",    no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "b:pd:", opts, NULL)) != -1) {
        switch (c) {
        case 'b': {
            limit = strtoull(optarg, NULL, 10); 
            if (limit % sizeof(uint64_t) != 0) {
                fprintf(stderr,
                    "--bytes must be multiple of %zu\n",
                    sizeof(uint64_t));
                return 2;
            }
            break;
        }
        // case 'p': pattern = 1; break;
        case 'd': dev = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:
            fprintf(stderr, "usage: %s --dev PATH --bytes N [--verify]\n", argv[0]);
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
        errno = EINVAL;
        perror("invalid --dev parameter");
        return 2;
    }
    int fd = open(dev_path, O_WRONLY);
    if (fd < 0) {
        handle_failed_ringbuf_dev_open(dev_path, errno);
        die_ec("open", errno, 1);
    }
    
    uint64_t buf[BUF_SZ / sizeof(uint64_t)];
    uint64_t seq = 0;

    while (total < limit) {

        size_t chunk = sizeof(buf);

        if (total + chunk > limit)
            chunk = limit - total;

        /*
        * enforce uint64_t framing
        */
        chunk -= (chunk % sizeof(uint64_t));

        if (chunk == 0)
            break;

        size_t count = chunk / sizeof(uint64_t);

        for (size_t i = 0; i < count; i++)
            buf[i] = seq++;

        ssize_t n = write(fd, buf, chunk);

        if (n < 0)
            die("write error");

        if (n == 0)
            die("write returned 0");

        total += n;

        struct timespec req = {
            .tv_sec = 0,
            .tv_nsec = 1,
        };

        nanosleep(&req, NULL);
    }

    close(fd);
    printf("%s: DONE (%zu bytes)\n", __FILE__, total);
    return 0;
}

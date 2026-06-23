// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

// Write sequential uint64_t values into ring device for a fixed time.
// Example: ./build/user/tests/overwrite_producer --dev /dev/ringbuf_msft --duration 5 --nsleep 0
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <sys/types.h>
#include <limits.h>

#include "ringbuf.h" /* optional - for IOC defs, not required for write */
#include "helpers.h" 

#define BUF_VALUES 4096  /* number of uint64_ts per burst (32KB) */

static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --dev PATH [options]\n", prog);
    printf("\n");
    printf("Required:\n");
    printf("  --dev PATH          ring device\n");
    printf("\n");
    printf("Options:\n");
    printf("  --duration N        total run time (default: unlimited)\n");
    printf("  --nsleep N          sleep between writes (nanoseconds)\n");
    printf("\n");
    printf("Example:\n");
    printf("   %s --dev /dev/ringbuf_device0 --duration 5 --nsleep 0\n", prog);
} 
static void synopsis(const char *prog) {
    fprintf(stderr, "usage: %s --dev PATH [--duration N]\n", prog);
}

int main(int argc, char **argv) {
    char dev_path[PATH_MAX];
    const char *dev = NULL;
    uint64_t duration = UINT64_MAX/1000000000ULL; /* seconds */
    unsigned long sleep_ns = 0;
    int c;

    static struct option opts[] = {
        {"dev", required_argument, 0, 'd'},
        {"duration", required_argument, 0, 's'},
        {"nsleep", required_argument, 0, 'n'},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };

    while ((c = getopt_long(argc, argv, "d:s:n:", opts, NULL)) != -1) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 's': duration = atoi(optarg); break;
        case 'n': sleep_ns = strtoul(optarg, NULL, 10); break;
        case 'h': usage(argv[0]); return 0;
        default:
            synopsis(argv[0]);
            return 2;
        }
    }
    if (!dev) {
        fprintf(stderr, "%s: --dev required\n", __FILE__);
        synopsis(argv[0]);
        return 2;
    }

    if(!normalize_dev(dev_path, sizeof(dev_path), dev)) {
        errno = EINVAL;
        perror("invalid --dev parameter");
        usage(argv[0]);
        return 2;
    }
    int fd = open(dev_path, O_WRONLY);
    if (fd < 0) {
        handle_failed_ringbuf_dev_open(dev_path, errno);
        die_ec("open", errno, 1);
    }

    uint64_t buf[BUF_VALUES];
    uint64_t counter = 0;
    uint64_t written_vals = 0;

    uint64_t start = now_ns();
    uint64_t end = start + (uint64_t)duration * 1000000000ULL;

    struct timespec req = {0};
    if (sleep_ns) {
        req.tv_sec = sleep_ns / 1000000000UL;
        req.tv_nsec = sleep_ns % 1000000000UL;
    }

    while (now_ns() < end) {
        /* fill buffer */
        for (size_t i = 0; i < BUF_VALUES; ++i) buf[i] = counter++;
        size_t bytes = sizeof(buf);

        ssize_t n = write(fd, (void*)buf, bytes);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("write");
        }
        /* allow partial writes */
        if ((size_t)n < bytes) {
            /* advance by number of full values written */
            size_t full = n / sizeof(uint64_t);
            written_vals += full;
            /* if partial trailing bytes were written, we treat them as lost for this test */
            break;
        } else {
            written_vals += (bytes / sizeof(uint64_t));
        }

        if (sleep_ns) nanosleep(&req, NULL);
    }

    printf("producer: DONE: duration=%lu s, values_written=%llu\n",
           duration, (unsigned long long)written_vals);

    close(fd);
    return 0;
}

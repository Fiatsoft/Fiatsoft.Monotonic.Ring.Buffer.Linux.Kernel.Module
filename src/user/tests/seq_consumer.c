// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

// NOTE:
// This test validates sequential stream behavior under normal operating
// conditions.
//
// When executed concurrently with overwrite-mode writers or under
// aggressive contention, reordered observations or discontinuities may
// occur if unread regions are replaced before consumption completes.
//
// Such events are expected under overwrite semantics and should not be
// interpreted as kernel memory corruption.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>

#include "helpers.h" 

#define BATCH_VALUES 4096

static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --dev PATH [options]\n", prog);
    printf("\n");
    printf("Required:\n");
    printf("  --dev PATH          ring device (name, dev-name or full-path)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --count N           number of values to read (default: unlimited)\n");
    printf("  --sleep-ns N        delay between reads (nanoseconds)\n");
    printf("  --help              show this help\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s --dev /dev/ringbuf_device0\n", prog);
} 

int main(int argc, char **argv)
{
    char dev_path[PATH_MAX];
    const char *dev = NULL;
    uint64_t max_values = 0; /* 0 = infinite */
    long sleep_ns = 0;

    static struct option opts[] = {
        {"count",    required_argument, 0, 'c'},
        {"sleep-ns", required_argument, 0, 's'},
        {"dev",      required_argument, 0, 'd'},
        {"help",     no_argument, 0, 'h'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "c:s:d:", opts, NULL)) != -1) {
        switch (c) {
        case 'c': max_values = strtoull(optarg, NULL, 10); break;
        case 's': sleep_ns = strtoull(optarg, NULL, 10); break;
        case 'd': dev = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:
            fprintf(stderr,
                "usage: %s --dev PATH [--count N] [--sleep-ns N]\n",
                argv[0]);
            return 2;
        }
    }
    
    if (!dev) {
        fprintf(stderr, "--dev required\n");
        usage(argv[0]);
        return 1;
    }
    
    if(!normalize_dev(dev_path, sizeof(dev_path), dev)) {
        errno = EINVAL;
        perror("invalid --dev parameter");
        return 1;
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

    uint64_t buffer[BATCH_VALUES];
    uint64_t expected = 0;
    bool first = true;

    uint64_t received = 0;
    uint64_t gap_count = 0;
    uint64_t max_gap = 0;

    struct timespec req = { .tv_sec = 0, .tv_nsec = sleep_ns };

    while (max_values == 0 || received < max_values) {

        ssize_t bytes = read(fd, buffer, sizeof(buffer));
        if (bytes < 0)
            die("read");

        if (bytes == 0)
            continue;

        if (bytes % sizeof(uint64_t)) {
            fprintf(stderr, "partial value read (%zd bytes)\n", bytes);
            exit(EXIT_FAILURE);
        }

        size_t count = bytes / sizeof(uint64_t);

        for (size_t i = 0; i < count; i++) {

            uint64_t v = buffer[i];

            if (first) {
                expected = v;
                first = false;
            }

            if (v < expected) {
                fprintf(stderr,
                    "REORDER detected: got %lu expected %lu\n",
                    v, expected);
                exit(EXIT_FAILURE);
            }

            if (v > expected) {
                uint64_t gap = v - expected;
                gap_count++;
                if (gap > max_gap)
                    max_gap = gap;

                printf("GAP: missed %lu values (from %lu to %lu)\n",
                       gap, expected, v - 1);

                expected = v;
            }

            expected++;
            received++;

            if (max_values && received >= max_values)
                break;
        }

        if (sleep_ns)
            nanosleep(&req, NULL);
    }

    printf("DONE\n");
    printf("Received:   %lu values\n", received);
    printf("Gaps:       %lu\n", gap_count);
    printf("Max gap:    %lu\n", max_gap);

    close(fd);
    return 0;
}
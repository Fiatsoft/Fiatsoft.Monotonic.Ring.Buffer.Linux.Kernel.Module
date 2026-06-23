// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

// Read sequential uint64_t values from ring device for fixed time.
// Counts gaps and resyncs under overwrite pressure.

// NOTE:
// This test intentionally exercises overwrite-mode behavior using a
// resynchronizable uint64_t stream under sustained producer pressure.
//
// Because overwrite-mode permits unread regions to be replaced while
// consumers are actively traversing the stream, discontinuities and
// resynchronization events are expected.
//
// Once synchronization is lost, the consumer may recover at an
// arbitrary stream position. Reported gap statistics therefore become
// approximate and should be interpreted as recovery indicators rather
// than precise loss measurements.
//
// The stream intentionally omits framing and integrity markers in order
// to maximize overwrite pressure and recovery-path exercise.
//
// Observed gaps therefore indicate overwrite loss or desynchronization
// rather than kernel memory corruption.
//
// The test primarily validates:
//   - overwrite survivability,
//   - recovery progression,
//   - continued forward traversal,
//   - and device stability under contention..

#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <time.h>

#include "ringbuf.h"
#include "helpers.h"

#define READ_BUF (16 * 1024) /* bytes */
#define TAIL_BUF  (64 * 1024) /* pending bytes buffer */
#define RESYNC_DELTA_LIMIT (UINT64_C(1) << 40)

static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --dev PATH [options]\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  --duration N  run time before exiting (seconds, default 5)\n");
    printf("\n");
    printf("Example:\n");
    printf("   %s --dev /dev/ringbuf_device0 --duration 5\n", prog);
}

int main(int argc, char **argv)
{
    char dev_path[PATH_MAX];
    const char *dev = NULL;
    int duration = 5;
    int c;
    uint64_t resyncs = 0;
    int recovered_stream = 0;

    static struct option opts[] = {
        {"dev", required_argument, 0, 'd'},
        {"duration", required_argument, 0, 's'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    while ((c = getopt_long(argc, argv, "d:s:h", opts, NULL)) != -1) {
        switch (c) {
        case 'd':
            dev = optarg;
            break;
        case 's':
            duration = atoi(optarg);
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            fprintf(stderr, "usage: %s --dev PATH [--duration N]\n", argv[0]);
            return 0;
        }
    }

    if (!dev) {
        fprintf(stderr, "consumer: --dev required\n");
        usage(argv[0]);
        return 2;
    }

    if (!normalize_dev(dev_path, sizeof(dev_path), dev)) {
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

    uint8_t rawbuf[READ_BUF];
    uint8_t tailbuf[TAIL_BUF];
    size_t tail_used = 0;

    uint64_t total_values = 0;
    uint64_t gaps = 0;
    uint64_t max_gap = 0;
    uint64_t prev = 0;
    int first = 1;

    uint64_t start_ns = now_ns();
    uint64_t end_ns = start_ns + (uint64_t)duration * 1000000000ULL;

    while (now_ns() < end_ns) {
        ssize_t n = read(fd, rawbuf, sizeof(rawbuf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            die("read");
        }

        if (n == 0) {
            struct timespec req = {0, 1000000}; /* 1 ms */
            nanosleep(&req, NULL);
            continue;
        }

        /* If the pending buffer would overflow, drop oldest aligned words
           until there is room. This preserves uint64_t framing. */
        if (tail_used + (size_t)n > sizeof(tailbuf)) {
            size_t want = (size_t)n;
            while (tail_used + want > sizeof(tailbuf) && tail_used >= sizeof(uint64_t)) {
                size_t drop = sizeof(uint64_t);
                memmove(tailbuf, tailbuf + drop, tail_used - drop);
                tail_used -= drop;
                resyncs++;
            }

            /* If the read itself is larger than the whole pending buffer, bail. */
            if (want > sizeof(tailbuf)) {
                fprintf(stderr, "consumer: read larger than buffer\n");
                break;
            }

            /* If still no room, drop everything we have and resync. */
            if (tail_used + want > sizeof(tailbuf)) {
                tail_used = 0;
                resyncs++;
            }
        }

        memcpy(tailbuf + tail_used, rawbuf, (size_t)n);
        tail_used += (size_t)n;

        size_t offset = 0;

        while (tail_used - offset >= sizeof(uint64_t)) {
            if (now_ns() >= end_ns)
                goto done;

            uint64_t v;
            memcpy(&v, tailbuf + offset, sizeof(uint64_t));
            offset += sizeof(uint64_t);

            if (first) {
                prev = v;
                first = 0;
                total_values++;
                continue;
            }

            if (v <= prev) {
                /* overwrite boundary / wrap / resync */
                resyncs++;
                prev = v;
                total_values++;
                continue;
            }

            {
                uint64_t delta = v - prev; /* safe now: v > prev */

                if (delta > RESYNC_DELTA_LIMIT) {
                    /* likely desynced observation, not a meaningful gap */
                    recovered_stream = 1;
                    resyncs++;
                    prev = v;
                    total_values++;
                    continue;
                }

                if (delta > 1) {
                    uint64_t gap = delta - 1;
                    gaps++;
                    if (gap > max_gap)
                        max_gap = gap;
                }

                prev = v;
                total_values++;
            }
        }

        size_t leftover = tail_used - offset;
        if (leftover > 0)
            memmove(tailbuf, tailbuf + offset, leftover);
        tail_used = leftover;
    }

done:
#if defined(RBUF_IOC_INFO)
    {
        struct rbuf_info info;
        if (ioctl(fd, RBUF_IOC_INFO, &info) == 0) {
            printf("ioctl INFO: size=%llu cap=%llu head=%llu tail=%llu\n",
                   (unsigned long long)info.size,
                   (unsigned long long)info.cap,
                   (unsigned long long)info.head,
                   (unsigned long long)info.tail);
        }
    }
#endif

    uint64_t bytes = total_values * sizeof(uint64_t);
    double seconds = (double)duration;
    double mbps = (bytes / seconds) / (1024.0 * 1024.0);

    printf("\nRESULTS (duration=%d s)\n", duration);
    printf("  values observed : %llu\n", (unsigned long long)total_values);
    printf("  gaps detected   : %llu\n", (unsigned long long)gaps);
    if (recovered_stream)
        printf("  max gap         : (stream recovered)\n");
    else
        printf("  max gap         : %llu\n",
            (unsigned long long)max_gap);
    printf("  resyncs         : %llu\n", (unsigned long long)resyncs);
    printf("  throughput      : %.2f MB/s\n", mbps);

    close(fd);
    return 0;
}

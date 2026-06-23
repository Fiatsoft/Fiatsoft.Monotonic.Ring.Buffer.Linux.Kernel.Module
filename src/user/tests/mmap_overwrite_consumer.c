// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

// NOTE:
// This test intentionally exercises overwrite-mode behavior under
// concurrent producer pressure.
//
// Because overwrite-mode permits unread data to be replaced while a
// consumer is actively traversing the mapping, monotonicity violations
// and stream discontinuities are expected during stress conditions.
//
// Such events do not necessarily indicate kernel corruption or memory
// safety issues; they are an inherent consequence of consuming an
// unframed overwrite stream without synchronization barriers.
//
// The test primarily validates:
//   - survivability,
//   - mmap visibility,
//   - overwrite progression,
//   - and continued device operation under contention.

#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
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
    printf("Options:\n");
    printf("  --duration SECONDS  test duration (default 5)\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s --dev /dev/ringbuf_device0\n", prog);
} 

int main(int argc, char **argv)
{
    char dev_path[PATH_MAX];
    const char *dev = NULL;
    int duration = 5;
    static struct option opts[] = {
        {"dev",      required_argument, 0, 'd'},
        {"duration", required_argument, 0, 's'},
        {"help",     no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:s:h", opts, NULL)) != -1) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 's': duration = atoi(optarg); break;
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

    size_t map_size = 0;
    struct rbuf_info info;
    if (ioctl(fd, RBUF_IOC_INFO, &info) != 0) {
        perror("ioctl info");
        return 1;
    }
    
    if (info.abi_version != RBUF_ABI_VERSION) {
        fprintf(stderr, "ABI mismatch (kernel=%u user=%u)\n",
                info.abi_version, RBUF_ABI_VERSION);
        exit(1);
    }

    map_size = info.size;

    void *base = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);

    if (base == MAP_FAILED) { perror("mmap"); return 1; }
    
    uint64_t prev = 0;
    int first = 1;

    uint64_t total = 0;
    uint64_t gaps = 0;
    uint64_t max_gap = 0;

    uint64_t end = now_ns() + (uint64_t)duration * 1000000000ULL;

    uint64_t *headp = (uint64_t *)(base + OFF_HEAD);
    uint64_t *tailp = (uint64_t *)(base + OFF_TAIL);
    uint8_t  *data  = (uint8_t *)(base + OFF_DATA); 

    uint64_t reader_pos = __atomic_load_n(headp, __ATOMIC_ACQUIRE); // uint64_t reader_pos = info.head;  // attach mode

    while (now_ns() < end) {

        uint64_t head = __atomic_load_n(headp, __ATOMIC_ACQUIRE);
        uint64_t tail = __atomic_load_n(tailp, __ATOMIC_ACQUIRE);

        if (reader_pos < tail)
            reader_pos = tail;

        while (reader_pos + sizeof(uint64_t) <= head) {
            size_t off = reader_pos & (info.cap - 1);
            size_t cap = info.cap;
            uint64_t v;
            if (off + sizeof(uint64_t) <= cap) {
                /* no wrap */
                memcpy(&v, data + off, sizeof(uint64_t));
            } else {
                /* wrapped record */
                size_t wrap_first = cap - off;
                size_t wrap_second = sizeof(uint64_t) - wrap_first;
                uint8_t tmp[sizeof(uint64_t)];
                memcpy(tmp, data + off, wrap_first);
                memcpy(tmp + wrap_first, data, wrap_second);
                memcpy(&v, tmp, sizeof(uint64_t));
            }

            if (first) {
                prev = v;
                first = 0;
            } else {
                if (v <= prev) {
                    printf("FATAL monotonic violation\n");
                    printf("prev=%" PRIu64 " v=%" PRIu64 " pos=%" PRIu64 " off=%zu head=%" PRIu64 " tail=%" PRIu64 "\n",
                        prev, v, reader_pos, off, head, tail);
                    return 2;
                }
                if (v > prev + 1) {
                    uint64_t g = v - prev - 1;
                    gaps++;
                    if (g > max_gap) max_gap = g;
                }
                prev = v;
            }

            reader_pos += sizeof(uint64_t);
            total++;
        }
    }

    double runtime = duration;
    double mbps = (total * sizeof(uint64_t)) / (1024.0*1024.0) / runtime;

    printf("\nMMAP RESULTS\n");
    printf("values   : %llu\n", (unsigned long long)total);
    printf("gaps     : %llu\n", (unsigned long long)gaps);
    printf("max gap  : %llu\n", (unsigned long long)max_gap);
    printf("MB/s     : %.2f\n", mbps);

    munmap(base, map_size);
    close(fd);
    return 0;
}
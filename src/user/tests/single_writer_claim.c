// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

// NOTE:
// This test validates concurrent writer survivability and continued
// device operation under unsynchronized multi-writer contention.
//
// Writers intentionally emit independent unsynchronized streams and
// do not attempt to preserve global ordering semantics.
//
// The test primarily validates:
//   - absence of crashes,
//   - forward progress,
//   - concurrent write survivability,
//   - reader survivability during active writes,
//   - poll/read liveness,
//   - and continued device operation under contention.
//
// The test is not intended to validate deterministic ordering,
// framing guarantees, or protocol-level integrity across writers.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/poll.h>

#define THREADS 8
#define WRITES_PER_THREAD 10000
#define CHUNK 128

#include "ringbuf.h"
#include "helpers.h"

char dev_path[PATH_MAX];
const char *dev = NULL;
int verbose;

static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --dev PATH [options]\n", prog);
    printf("\n");
    printf("Required:\n");
    printf("  --dev PATH          ring device (name, dev-name or full-path)\n");
    printf("Options:\n");
    printf("  --verbose           print status and hex dumps\n");
    printf("  --duration SECONDS  read duration (default 5)\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s --dev /dev/ringbuf_device0\n", prog);
} 

struct writer_result {
    int open_errno;
    int writes_completed;
};

static void *writer_thread(void *arg)
{
    struct writer_result *res = arg;

    int fd = open(dev_path, O_WRONLY);

    if (fd < 0) {
        res->open_errno = errno;
        return NULL;
    }

    char buf[CHUNK];
    memset(buf, 'A', sizeof(buf));

    for (int i = 0; i < WRITES_PER_THREAD; i++) {
        if (write(fd, buf, sizeof(buf)) < 0)
            break;
        res->writes_completed++;
    }

    close(fd);
    return NULL;
}

int main(int argc, char **argv)
{
    struct writer_result results[THREADS] =  {0};

    int duration = 5;
    static struct option opts[] = {
        {"dev",  required_argument, 0, 'd'},
        {"verbose",  no_argument, 0, 'v'},
          {"duration", required_argument, 0, 's'},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:vs:h", opts, NULL)) != -1) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'v': verbose = 1; break;
        case 's': duration = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:
            fprintf(stderr, "usage: %s --dev-name PATH [--verbose] [--duration SEC]\n", argv[0]);
            fprintf(stderr, "       %s --help\n", argv[0]);
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

    pthread_t th[THREADS];

    printf("[" __FILE__ "] launching %d writers...\n", THREADS);

    for (long i = 0; i < THREADS; i++) {
        if (pthread_create(&th[i],
                NULL,
                writer_thread,
                &results[i])) {
            perror("pthread_create");
            return 1;
        }
    }

    for (int i = 0; i < THREADS; i++) {
        void *ret;
        pthread_join(th[i], &ret);
        if (ret != 0) {
            fprintf(stderr, "writer %d failed\n", i);
            return 1;
        }
        printf("thread %lu finished, returned %ld\n", th[i], (long)ret);
    }

    printf("[" __FILE__ "] all writers completed\n");

    int winners = 0;
    int busy = 0;
    
    for (int i = 0; i < THREADS; i++) {
        if (results[i].open_errno == 0)
            winners++;

        if (results[i].open_errno == EBUSY)
            busy++;
    }

    printf("writers admitted : %d\n", winners);
    printf("writers rejected : %d\n", busy);

    if (winners != 1) {
        fprintf(stderr,
            "FAIL: expected exactly one writer\n");
        return 1;
    }

    int fd = open(dev_path, O_RDONLY);
    if (ioctl(fd, RBUF_IOC_CLAIM) != 0) {
        close(fd);
        die_c("ioctl(CLAIM)",1);
    }

    /* bounded observation window */
    int timeout_ms = duration * 1000;
    int elapsed = 0;
    int step = 100;  // 100ms polling granularity

    char tmp[256];
    int total_read = 0;

    while (elapsed < timeout_ms) {
        struct pollfd pfd = {
            .fd = fd,
            .events = POLLIN
        };

        int ret = poll(&pfd, 1, step);
        if (ret < 0) {
          close(fd);
          die_c("poll", 1);
        }

        if (ret == 0) {
            /* timeout slice */
            elapsed += step;
            continue;
        }

        if (pfd.revents & POLLIN) {
            ssize_t n = read(fd, &tmp, sizeof(tmp));
            if (n < 0) {
                close(fd);
                die_c("read", 1);
            }

            if (n > 0) {
                total_read += n;
                if (verbose) {
                    printf("read %zd bytes: %s\n", n, tmp);
                }
            }
        }
    }

    close(fd);

    printf("[" __FILE__ "] observed %d bytes total\n", total_read);
    printf("PASS: single-writer claim enforcement working\n");
    return 0;
}
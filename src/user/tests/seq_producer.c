// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

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

#define BATCH_VALUES 4096  /* 4096 * 8 = 32 KB per write */

static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --dev PATH [options]\n", prog);
    printf("\n");
    printf("Required:\n");
    printf("  --dev PATH          ring device (name, dev-name or full-path)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --count N           number of values to write (default: unlimited)\n");
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
    uint64_t total_values = 0; /* 0 = infinite */
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
        case 'c': total_values = strtoull(optarg, NULL, 10); break;
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
    int fd = open(dev_path, O_WRONLY);
    if (fd < 0) {
        handle_failed_ringbuf_dev_open(dev_path, errno);
        die_ec("open", errno, 1);
    }

    uint64_t buffer[BATCH_VALUES];
    uint64_t value = 0;
    uint64_t sent = 0;

    struct timespec req = { .tv_sec = 0, .tv_nsec = sleep_ns };

    while (total_values == 0 || sent < total_values) {

        size_t batch = BATCH_VALUES;

        if (total_values && sent + batch > total_values)
            batch = total_values - sent;

        for (size_t i = 0; i < batch; i++)
            buffer[i] = value++;

        ssize_t bytes = write(fd, buffer, batch * sizeof(uint64_t));
        if (bytes < 0)
            die("write");

        if ((size_t)bytes != batch * sizeof(uint64_t)) {
            fprintf(stderr, "short write\n");
            exit(EXIT_FAILURE);
        }

        sent += batch;

        if (sleep_ns)
            nanosleep(&req, NULL);
    }

    fprintf(stdout, "DONE.\n");
    close(fd);
    return 0;
}
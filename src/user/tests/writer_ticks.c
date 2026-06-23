// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include "helpers.h"

static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --dev PATH [options]\n", prog);
    printf("\n");
    printf("Required:\n");
    printf("  --dev PATH          ring device (name, dev-name or full-path)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --sleep-ms N        delay between ticks (milliseconds)\n");
    printf("\n");
    printf("Example:\n");
    printf("   %s --dev /dev/ringbuf_device0 --seconds 5 --nsleep 0\n", prog);
}

int main(int argc, char **argv) {
    // char path[PATH_MAX];
    char dev_path[PATH_MAX];
    const char *dev = NULL;
    unsigned long sleep_ms = 20;
    int fd;
    int c;

    static struct option opts[] = {
        {"dev", required_argument, 0, 'd'},
        {"sleep-ms", required_argument, 0, 'm'},
        {"help",     no_argument, 0, 'h'},
        {0,0,0,0}
    };

    while ((c = getopt_long(argc, argv, "d:m:", opts, NULL)) != -1) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'm':
            if (optarg)
                sleep_ms = strtoul(optarg, NULL, 10);
            else
                sleep_ms = 20; // or some default
            break;
        case 'h': usage(argv[0]); return 0;
        default:
            fprintf(stderr, "usage: %s --dev PATH [--sleep-ms MS]\n", argv[0]);
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
    fd = open(dev_path, O_WRONLY);
    if (fd < 0) { 
        handle_failed_ringbuf_dev_open(dev_path, errno);
        die_ec("open", errno, 1);
    }

    int interval_ms = sleep_ms;

    int i = 0;
    while (1) {
        int price_cents = 10000 + (i % 500);
        char buf[128];
        int n = snprintf(buf, sizeof(buf), "TICK-%d PRICE=%d.%02d\n", i, price_cents/100, price_cents%100);
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            perror("write");
            close(fd);
            return 1;
        }
        printf("[writer] wrote %zd bytes: %s", w, buf);
        i++;
        struct timespec ts = { .tv_sec = interval_ms/1000, .tv_nsec = (interval_ms%1000) * 1000000 };
        nanosleep(&ts, NULL);
    }
    close(fd);
    return 0;
}

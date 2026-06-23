// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

// Usage: 
// echo "hello world" | ./bin/emit_stdin --dev /dev/ringbuf_ibm
// cat file.bin | ./bin/emit_stdin --dev /dev/ringbuf_ibm

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <getopt.h>

#include "helpers.h"

static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --dev PATH\n", prog);
}

int main(int argc, char **argv)
{
    char dev_path[PATH_MAX];
    const char *dev = NULL;

    static struct option opts[] = {
        {"dev", required_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:h", opts, NULL)) != -1) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default: return 2;
        }
    }

    if (!dev) {
        usage(argv[0]);
        return 2;
    }

    if (!normalize_dev(dev_path, sizeof(dev_path), dev)) {
        fprintf(stderr, "invalid dev\n");
        return 2;
    }

    int fd = open(dev_path, O_WRONLY);
    if (fd < 0) {
        handle_failed_ringbuf_dev_open(dev_path, errno);
        die_ec("open", errno, 1);
        // perror("open");
        // return 1;
    }

    char buf[4096];
    ssize_t n;

    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(fd, buf + off, n - off);
            if (w < 0) {
                perror("write");
                close(fd);
                return 1;
            }
            off += w;
        }
    }

    if (n < 0)
        perror("stdin read");

    close(fd);
    return 0;
}

// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
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
    struct stat st;

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
    }

    if (fstat(fd, &st) != 0) {
        fprintf(stderr,
                "fstat(%s): %s\n",
                dev_path,
                strerror(errno));
        close(fd);
        return 1;
    }

    printf("path        : %s\n", dev_path);
    printf("inode       : %lu\n", (unsigned long)st.st_ino);
    printf("mode        : 0%o\n", st.st_mode);
    printf("is_chr      : %s\n",
           S_ISCHR(st.st_mode) ? "yes" : "no");

    if (S_ISCHR(st.st_mode)) {
        printf("major       : %u\n", major(st.st_rdev));
        printf("minor       : %u\n", minor(st.st_rdev));
    }
    else {
      fprintf(stderr, "\nWARNING: %s is not a character device; does it pre-date module insertion?\n"
                        "         Removal of device with `sudo rm %s` and recreation is recommended\n", dev_path, dev_path);
    }

    close(fd);
    return 0;
}
// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "ringbuf.h"
#include "helpers.h"

static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --dev PATH [POLICY]\n", prog);
    printf("\n");
    printf("Examples:\n");
    printf("  %s --dev /dev/ringbuf_msft sleep\n", prog);
    printf("  echo busy | %s --dev /dev/ringbuf_msft\n", prog);
}

static void trim_ws_inplace(char *s)
{
    char *start, *end;

    if (!s)
        return;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        memmove(s, s + 1, strlen(s));

    start = s;
    end = s + strlen(s);
    while (end > start &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
        *--end = '\0';
}

static bool parse_policy_token(const char *s, int *out)
{
    char *end = NULL;
    long v;

    if (!s || !*s || !out)
        return false;

    *out = rbuf_from_str(s, SERIES_SPIN_POLICY);
    if (*out >= 0)
        return true;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno == 0 && end && *end == '\0' &&
        v >= RBUF_SPIN_BUSY && v <= RBUF_SPIN_SLEEP)
    {
        *out = (int)v;
        return true;
    }

    return false;
}

int main(int argc, char **argv)
{
    char dev_path[PATH_MAX];
    const char *dev = NULL;
    const char *policy_arg = NULL;

    static struct option opts[] = {
        {"dev", required_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:h", opts, NULL)) != -1) {
        switch (c) {
        case 'd':
            dev = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            return 2;
        }
    }
    if (!dev) {
        usage(argv[0]);
        return 2;
    }

    if (optind < argc)
        policy_arg = argv[optind];

    if (!policy_arg || !*policy_arg) {
        static char stdin_buf[64];
        ssize_t n = read(STDIN_FILENO, stdin_buf, sizeof(stdin_buf) - 1);
        if (n < 0) {
            perror("stdin read");
            return 1;
        }
        if (n == 0) {
            fprintf(stderr, "no spin policy provided\n");
            return 2;
        }
        stdin_buf[n] = '\0';
        trim_ws_inplace(stdin_buf);
        policy_arg = stdin_buf;
    }

    if (!normalize_dev(dev_path, sizeof(dev_path), dev)) {
        fprintf(stderr, "invalid dev: %s\n", dev);
        return 2;
    }

    int policy;
    if (!parse_policy_token(policy_arg, &policy)) {
        fprintf(stderr, "invalid spin policy: %s\n", policy_arg);
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

    int ret = 0;
    if ((ret = ioctl(fd, RBUF_IOC_SPIN_POLICY, &policy)) != 0) {
        perror("ioctl(SPIN_POLICY)");
        (void)ioctl(fd, RBUF_IOC_RELEASE);
        close(fd);
        return 1;
    }

    if ((ret = ioctl(fd, RBUF_IOC_RELEASE)) != 0) {
        perror("ioctl(RELEASE)");
    }

    close(fd);
    return 0;
}
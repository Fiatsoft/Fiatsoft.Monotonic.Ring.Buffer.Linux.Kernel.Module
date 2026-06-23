// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "ringbuf.h"
#include "helpers.h"

int open_ringbuf_from_argv(
    char** argv,
    int argc, 
    const char *base,
    char *appendage,
    int open_flags,
    char *resolved_path,
    size_t resolved_len
) {
    char *dev = NULL;
    const char *DEVICE_ARG = "--dev";
    for (int i = 1; i < argc; i++) {
        // if (!strcmp(argv[i], DEVICE_ARG) && i + 1 < argc) {
        //     dev = argv[++i];
        // }
        if (!strcmp(argv[i], DEVICE_ARG)) {
            if (i + 1 >= argc || argv[i+1][0] == '-') {
                fprintf(stderr, "error: %s requires a value\n", DEVICE_ARG);
                errno = EINVAL;
                return -1;
            }
            dev = argv[++i];
        }
    }

    if (!dev || !*dev) {
        fprintf(stderr,
            "error: missing %s <name> parameter\n", DEVICE_ARG);
        errno = EINVAL;
        return -1;
    }

    if (appendage==NULL) 
        appendage = "";

    int n;
    if (strchr(dev, '/')) {
        n = snprintf(resolved_path, resolved_len, "%s", dev);
    } else {
        n = snprintf(resolved_path, resolved_len,
                     "%s%s%s", base, dev, appendage);
    }

    if (n < 0 || (size_t)n >= resolved_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fprintf(stdout, "\n");
    return open(resolved_path, open_flags);
}

void die_ec(const char *msg, int _errno, int exit_code) {
    errno = _errno;
    perror(msg);
    exit(exit_code);
}
void die_c(const char *msg, int exit_code) {
    perror(msg);
    exit(exit_code);
}
void die(const char *msg) {
    perror(msg);
    exit(1);
}

uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

bool normalize_dev(char *out, size_t sz, const char *in)
{
    if (!in || !*in)
        return false;

    int n;

    if (strncmp(in, "/dev/ringbuf_", 13) == 0) {
        n = snprintf(out, sz, "%s", in);
    } else if (strncmp(in, "ringbuf_", 8) == 0) {
        n = snprintf(out, sz, "/dev/%s", in);
    } else if (strncmp(in, "/dev/", 5) == 0) {
        n = snprintf(out, sz, "%s", in);
    } else {
        n = snprintf(out, sz, "/dev/ringbuf_%s", in);
    }

    if (n < 0 || (size_t)n >= sz)
        return false;

    return true;
}

bool normalize_debugfs_view(char *out,
                            size_t sz,
                            const char *in,
                            const char *view)
{
    int n;
    char suffix[64];

    if (!out || !sz || !in || !*in || !view || !*view)
        return false;

    /* full debugfs path already */
    if (strncmp(in, "/sys/kernel/debug/ringbuf/", 28) == 0) {

        /* already includes requested projection */
        snprintf(suffix, sizeof(suffix), "/%s", view);
        if (strstr(in, suffix)) {
            n = snprintf(out, sz, "%s", in);
        } else {
            n = snprintf(out, sz, "%s/%s", in, view);
        }

    /* already prefixed device name */
    } else if (strncmp(in, "ringbuf_", 8) == 0) {

        n = snprintf(out, sz,
                     "/sys/kernel/debug/ringbuf/%s/%s",
                     in,
                     view);

    /* bare device name */
    } else {

        n = snprintf(out, sz,
                     "/sys/kernel/debug/ringbuf/ringbuf_%s/%s",
                     in,
                     view);
    }

    return n >= 0 && (size_t)n < sz;
}

bool normalize_tap(char *out, size_t sz, const char *in)
{
    return normalize_debugfs_view(out, sz, in, "tap");
}

bool normalize_dump(char *out, size_t sz, const char *in)
{
    return normalize_debugfs_view(out, sz, in, "dump");
}

void trim_ws(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = '\0';
}

void run_cmd(char *const argv[], bool verbose)
{
    size_t total = 0;

    for (int i = 0; argv[i]; i++) {
        total += strlen(argv[i]) + 1; // include null terminator
    }

    long arg_max = sysconf(_SC_ARG_MAX);
    if (arg_max < 0)
        die("sysconf(_SC_ARG_MAX)");

    if (total > (size_t)arg_max) {
        fprintf(stderr, "command too long: %zu > %ld\n", total, arg_max);
        exit(1);
    }

    if (verbose) {
        printf("running:");
        for (int i = 0; argv[i]; i++)
            printf(" %s", argv[i]);
        printf("\n");
    }

    fflush(NULL);
    execv(argv[0], argv);

    perror("execv failed");
    _exit(1);
}

void spawn_and_wait(char *const argv[], bool verbose)
{
    pid_t pid = fork();
    if (pid == 0) {
        run_cmd(argv, verbose);
    }

    if (verbose)
        printf("waiting for PID %d...\n", pid);
    waitpid(pid, NULL, 0);
}

uint64_t get_avail(int dev_fd, struct rbuf_info *info_out)
{
    struct rbuf_info info;

    if (ioctl(dev_fd, RBUF_IOC_INFO, &info) < 0)
        return 0;

    if (info.head < info.tail)
        return 0;

    uint64_t avail = info.head - info.tail;

    if (avail > info.cap)
        avail = info.cap;

    if (info_out)
        *info_out = info;

    return avail;
}

void handle_failed_ringbuf_dev_open(const char *dev_path, int err) {
    switch (err) {
        case ENOENT:
            fprintf(stderr,
                "device '%s' does not exist; check `ls /dev/ringbuf*` to verify it exists\n",
                dev_path);
            break;

        case ENODEV:
            fprintf(stderr,
                "device '%s' exists but has no backing device\n",
                dev_path);
            break;

        case ESHUTDOWN:
            fprintf(stderr,
                "device '%s' is shutting down (removed/draining); "
                "close all user-processes in `lsof %s` and recreate it\n",
                dev_path, dev_path);
            break;

        default:
            fprintf(stderr,
                "device '%s' failed to open; reason unknown\n",
                dev_path);
            break;
    }
    fprintf(stderr, "error code %d\n", err);
}

void handle_failed_ringbuf_tap_open(const char *tap_path, int err) {
    switch (err) {
        case ENOENT:
            fprintf(stderr,
                "tap %s could not be found; check in `sudo find /sys/kernel/debug/ringbuf/ | grep tap` to verify it exists\n",
                tap_path);
            break;

        case ENODEV:
            fprintf(stderr,
                "tap '%s' exists but has no backing device\n",
                tap_path);
            break;

        case ESHUTDOWN:
            fprintf(stderr,
                "tap '%s' is shutting down (likely removed); detach all user-processes from owning device and recreate it\n",
                tap_path);
            break;

        default:
            fprintf(stderr,
                "tap '%s' failed to open; reason unknown\n",
                tap_path);
            break;
    }
    fprintf(stderr, "error code: %d\n", err);
}
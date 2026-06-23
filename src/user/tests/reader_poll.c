// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "helpers.h"
#include "ringbuf.h"


static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --dev PATH [options]\n", prog);
    printf("\n");
    printf("Required:\n");
    printf("  --dev PATH          ring device (name, dev-name or full-path)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --duration SECONDS  test duration (default 5)\n");
    printf("\n");    printf("\n");
    printf("Example:\n");
    printf("  %s --dev /dev/ringbuf_device0\n", prog);
} 

int main(int argc, char **argv) {
    int duration = -1;
    char dev_path[PATH_MAX];
    const char *dev = NULL;
    static struct option opts[] = {
        {"dev",  required_argument, 0, 'd'},
        {"duration",  required_argument, 0, 's'},
        {"help", no_argument,       0, 'v'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:s:h", opts, NULL)) != -1) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 's': duration = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:
            fprintf(stderr, "usage: %s --dev PATH\n", argv[0]);
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

    // int fd = open(dev_path, O_RDONLY);
    int fd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        handle_failed_ringbuf_dev_open(dev_path, errno);
        die_ec("open", errno, 1);
    }

    if (ioctl(fd, RBUF_IOC_CLAIM) != 0) {
        perror("ioctl(CLAIM)");
        close(fd);
        return 1;
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    printf("Polling /dev/ringbuf (non-blocking open, poll will block)...\n");

    if (duration > 0) 
        alarm(duration);

    while (1) {
        int ret;
        do {
            ret = poll(&pfd, 1, -1);
        } while (ret < 0 && errno == EINTR);

        if (ret < 0) {
            perror("poll");
            break;
        }

        
        if (pfd.revents & POLLERR) {
            fprintf(stderr, "device removed (POLLERR)\n");
            break;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "poll: revents=0x%x\n", pfd.revents);
            break;
        }

        if (pfd.revents & POLLIN) {
            char buf[256];
            ssize_t n;

            do {
                n = read(fd, buf, sizeof(buf)-1);
            } while (n < 0 && errno == EINTR);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* nothing now — continue polling */
                    continue;
                }
                perror("read");
                break;
            } else if (n == 0) {
                /* EOF on device (unlikely) */
                fprintf(stderr, "read returned 0 (EOF)\n");
                break;
            }

            buf[n] = '\0';
            fwrite(buf, 1, n, stdout);
            fflush(stdout);
        }
    }

    close(fd);
    return 0;
}
// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

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
#include <signal.h>

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

static volatile sig_atomic_t stop = 0;

static void on_alarm(int sig)
{
    (void)sig;
    stop = 1;
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

    if (ioctl(fd, RBUF_IOC_CLAIM) != 0) {
      perror("ioctl(CLAIM)");
      close(fd);
      return 1;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    char buf[128];
    size_t total = 0;

    alarm(duration);

    while (!stop) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            total += (size_t)n;
            fwrite(buf, 1, (size_t)n, stdout);
        } else if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            break;
        } else {
            break;
        }
    }

    alarm(0);
    printf("total=%zu\n", total);
    close(fd);
    return 0;
}
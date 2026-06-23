// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include <errno.h>

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
    printf("Example:\n");
    printf("  %s --dev /dev/ringbuf_device0\n", prog);
} 

char *get_binary_str(uint32_t val, char *buf) {
    int i;

    for (i = 31; i >= 0; i--) {
        // Shift val right by i positions and check the LSB
        buf[31 - i] = (val & (1U << i)) ? '1' : '0';
    }
    buf[32] = '\0';

    return buf;
}

int main(int argc, char **argv)
{
    char dev_path[PATH_MAX];
    const char *dev = NULL;
    static struct option opts[] = {
        {"dev",  required_argument, 0, 'd'},
        {"help", no_argument,       0, 'v'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:h", opts, NULL)) != -1) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:
            fprintf(stderr, "usage: %s ] --dev PATH --bytes N [--verify]\n", argv[0]);
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

    // uint32_t info[3];
    struct rbuf_info info;
    char bin_str_buf[32];

    if (ioctl(fd, RBUF_IOC_INFO, &info) < 0) {
        perror("ioctl INFO");
        return 1;
    }
    printf("size=%lu (total)\n", info.size);
    printf("cap=%lu (data capacity)\n", info.cap);
    printf("head=%lu\n", info.head);
    printf("tail=%lu\n", info.tail);
    printf("writer_policy=%u (%s)\n", info.writer_policy, rbuf_to_str(info.writer_policy));
    printf("reader_policy=%u (%s)\n", info.reader_policy, rbuf_to_str(info.reader_policy));
    printf("spin_policy=%u (%s)\n", info.spin_policy, rbuf_to_str(info.spin_policy));
    printf("numa_node=%d\n", info.numa_node);
    printf("flags=0x%08x (b%s)\n", info.flags, get_binary_str(info.flags, bin_str_buf));
    printf("reserved[0]=0x%08x (b%s)\n", info.reserved[0], get_binary_str(info.reserved[0], bin_str_buf));
    printf("reserved[1]=0x%08x (b%s)\n", info.reserved[1], get_binary_str(info.reserved[1], bin_str_buf));
    printf("reserved[2]=0x%08x (b%s)\n", info.reserved[2], get_binary_str(info.reserved[2], bin_str_buf));
    printf("reserved[3]=0x%08x (b%s)\n", info.reserved[3], get_binary_str(info.reserved[3], bin_str_buf));
    printf("reserved[4]=0x%08x (b%s)\n", info.reserved[4], get_binary_str(info.reserved[4], bin_str_buf));
    printf("reserved[5]=0x%08x (b%s)\n", info.reserved[5], get_binary_str(info.reserved[5], bin_str_buf));
    printf("reserved[6]=0x%08x (b%s)\n", info.reserved[6], get_binary_str(info.reserved[6], bin_str_buf));
    printf("reserved[7]=0x%08x (b%s)\n", info.reserved[7], get_binary_str(info.reserved[7], bin_str_buf));

    close(fd);
    return 0;
}
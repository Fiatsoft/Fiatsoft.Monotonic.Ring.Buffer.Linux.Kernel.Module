// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only


/* syscall-heavy test
 ./throughput_benchmark \
   --device /dev/ringbuf \
   --duration 5 \
   --chunk 4096
 high-throughput mode
 ./throughput_benchmark \
   --chunk 4096 \
   --batch 262144
 diagnostic mode
 ./throughput_benchmark \
   --chunk 4096 \
   --batch 262144 \
   --lag_report */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <getopt.h>
#include <sched.h>
#include <limits.h>

#include "ringbuf.h"
#include "helpers.h" 

#define NS_PER_SEC 1000000000ULL

static void pin_to_cpu(int cpu)
{
    if (cpu < 0)
        return;

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);

    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
    }
}

struct bench_args {
    const char *device;
    int duration_sec;
    size_t chunk;
    uint batch;
    bool lag_report;

    int writer_cpu;
    int reader_cpu;
};

struct bench_ctx {
    int rfd;
    uint8_t *base;
    uint64_t cap;

    size_t chunk;
    bool lag_report;

    int reader_cpu;

    volatile bool stop;

    uint64_t max_lag;
};

static void *reader_thread(void *arg)
{
    struct bench_ctx *ctx = arg;

    pin_to_cpu(ctx->reader_cpu);

    uint64_t *headp = (uint64_t *)(ctx->base + OFF_HEAD);
    uint64_t *tailp = (uint64_t *)(ctx->base + OFF_TAIL);
    uint8_t  *data  = ctx->base + OFF_DATA;

    uint64_t reader_pos =
        __atomic_load_n(headp, __ATOMIC_ACQUIRE);

    uint64_t cap = ctx->cap;

    while (!ctx->stop) {

        uint64_t head =
            __atomic_load_n(headp, __ATOMIC_ACQUIRE);

        while (reader_pos + ctx->chunk <= head) {

            uint64_t tail =
                __atomic_load_n(tailp, __ATOMIC_ACQUIRE);

            if (reader_pos < tail) {
                reader_pos = tail;
                continue;
            }

            uint64_t offset = reader_pos & (cap - 1);

            /* subtle correctness fix:
               touch memory so consumer bandwidth is real */

            volatile uint64_t sample =
                *(volatile uint64_t *)(data + offset);

            (void)sample;

            reader_pos += ctx->chunk;
        }

        if (ctx->lag_report) {

            uint64_t tail =
                __atomic_load_n(tailp, __ATOMIC_ACQUIRE);

            uint64_t lag = (head >= tail) ? (head - tail) : 0;

            if (lag > ctx->max_lag)
                ctx->max_lag = lag;
        }

        /* small sleep prevents pure spin */
        static int spin = 0;
        if (++spin > 1000) {
            sched_yield();
            spin = 0;
        }
    }

    return NULL;
}

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
    printf("  --chunk BYTES       event size (default 4096)\n");
    printf("  --batch BYTES       write batch size\n");
    printf("  --writer_cpu ID     pin writer thread\n");
    printf("  --reader_cpu ID     pin reader thread\n");
    printf("  --lag_report        print lag statistics\n");
    printf("\n");
    printf("Examples:\n");
    printf("  Basic:\n");
    printf("    %s --dev /dev/ringbuf_msft --duration 3 --chunk 65536 --batch 1048576 --writer_cpu 1 --reader_cpu 2 --lag_report && echo -e \"\\nSTATS\" \n", prog);
    printf("  With Stats (Elevation Required):\n");
    printf("    %s --dev /dev/ringbuf_msft --duration 3 --chunk 65536 --batch 1048576 --writer_cpu 1 --reader_cpu 2 --lag_report && echo -e \"\\nSTATS\" && sudo cat /sys/kernel/debug/ringbuf/ringbuf_msft/stats\n", prog);
}

int main(int argc, char **argv)
{
    char dev_path[PATH_MAX];
    struct bench_args args = {
        .device = NULL,
        .duration_sec = 5,
        .chunk = 4096,
        .batch = 0,
        .lag_report = false,
        .writer_cpu = -1,
        .reader_cpu = -1,
    };

    static struct option long_opts[] = {
        {"dev",   required_argument, 0, 'd'},
        {"duration", required_argument, 0, 't'},
        {"chunk",    required_argument, 0, 'c'},
        {"batch", required_argument, 0, 'b'},
        {"lag_report", no_argument, 0, 'l'},
        {"writer_cpu", required_argument, 0, 'w'},
        {"reader_cpu", required_argument, 0, 'r'},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:t:c:b:lw:r:", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'd': args.device = optarg; break;
            case 't': args.duration_sec = atoi(optarg); break;
            case 'c': args.chunk = strtoull(optarg,NULL,10); break;
            case 'b': args.batch = strtoull(optarg,NULL,10); break;
            case 'l': args.lag_report = true; break;
            case 'w': args.writer_cpu = atoi(optarg); break;
            case 'r': args.reader_cpu = atoi(optarg); break;
            case 'h': usage(argv[0]); return 0;
            default:
                fprintf(stderr, "usage: %s --dev PATH [--duration N] [--chunk N] [--batch N] [--writer_cpu N] [--reader_cpu N] [--lag_report]\n", argv[0]);
                return 2;
        }
    }
    if (!args.device) {
        fprintf(stderr, "Error: --dev is required\n");
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    printf("\nBENCH CONFIG\n");
    printf("device        : %s\n", args.device);
    printf("duration_sec  : %d\n", args.duration_sec);
    printf("chunk_bytes   : %zu\n", args.chunk);
    printf("batch_bytes   : %zu\n", args.batch ? args.batch : args.chunk);
    printf("writer_cpu    : %d\n", args.writer_cpu);
    printf("reader_cpu    : %d\n", args.reader_cpu);
    printf("lag_report    : %s\n", args.lag_report ? "yes" : "no");

    pin_to_cpu(args.writer_cpu);

    if(!normalize_dev(dev_path, sizeof(dev_path), args.device)) {
        errno = EINVAL;
        perror("invalid --dev parameter");
        return 1;
    }

    int wfd = open(dev_path, O_WRONLY);
    if (wfd < 0) {
        if (strcmp(args.device, dev_path))
            fprintf(stderr, "device %s (as %s) could not be found; ", args.device, dev_path);
        else
            fprintf(stderr, "device %s could not be found; ", dev_path);
        fprintf(stderr, "check in `ls /dev/ringbuf*` to verify it exists:\n");
        die("open writer");
    }
    int rfd = open(dev_path, O_RDONLY);
    if (rfd < 0) {
        if (strcmp(args.device, dev_path))
            fprintf(stderr, "device %s (as %s) could not be found; ", args.device, dev_path);
        else
            fprintf(stderr, "device %s could not be found; ", dev_path);
        fprintf(stderr, "check in `ls /dev/ringbuf*` to verify it exists:\n");
        die("open reader");
    }

    struct rbuf_info info;
    if (ioctl(rfd, RBUF_IOC_INFO, &info) != 0) {
        perror("ioctl(INFO)");
        return 1;
    }

    if (info.writer_policy == RBUF_WRITER_BLOCK) {
        printf("\nBlocking-device detected.\n");
        printf("This mmap throughput test requires overwrite mode.\n");
        printf("Create device with:\n\n");
        printf("  echo %s | sudo tee /sys/class/ringbuf/remove_device\n", args.device);
        printf("  echo \"name=%s size=16384 writer_policy=overwrite\" "
            "| sudo tee /sys/class/ringbuf/add_device\n", args.device);
        return 0;
    }

    uint8_t *base = mmap(NULL, info.size,
                         PROT_READ,
                         MAP_SHARED, rfd, 0);
    if (base == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    struct bench_ctx ctx = {
        .rfd = rfd,
        .base = base,
        .cap = info.cap,
        .chunk = args.chunk,
        .lag_report = args.lag_report,
        .reader_cpu = args.reader_cpu,
        .stop = false,
        .max_lag = 0,
    };
    pthread_t tid;
    pthread_create(&tid, NULL, reader_thread, &ctx);



    uint64_t start = now_ns();
    uint64_t bytes_written = 0;
    
    size_t write_size = args.batch ? args.batch : args.chunk;
    if (args.batch && args.batch % args.chunk != 0) {
        fprintf(stderr, "batch must be multiple of chunk\n");
        exit(1);
    }
    void *buf = malloc(write_size);
    memset(buf, 0xAB, write_size);

    while ((now_ns() - start) <
        (uint64_t)args.duration_sec * NS_PER_SEC) {

        ssize_t ret = write(wfd, buf, write_size);

        if (ret < 0) {
            perror("write");
            break;
        }

        bytes_written += ret;
    }

    ctx.stop = true;
    pthread_join(tid, NULL);

    uint64_t elapsed_ns = now_ns() - start;
    double sec = (double)elapsed_ns / NS_PER_SEC;
    double mb = (double)bytes_written / (1024.0 * 1024.0);

    double mbps = mb / sec;
    double events = (double)bytes_written / args.chunk;
    double eps = events / sec;

    printf("\nTHROUGHPUT RESULTS\n");
    printf("bytes    : %lu\n", bytes_written);
    printf("seconds  : %.2f\n", sec);
    printf("MB/s     : %.2f\n", mbps);
    printf("events/s : %.0f\n", eps);

    if (args.lag_report) {
        printf("\nLAG REPORT\n");
        printf("max_lag_bytes : %lu\n", ctx.max_lag);
        double usage =
            (double)ctx.max_lag / ctx.cap * 100.0;
        printf("buffer_usage  : %.2f %%\n", usage);
    }

    munmap(base, info.size);
    close(wfd);
    close(rfd);
    free(buf);

    return 0;
}
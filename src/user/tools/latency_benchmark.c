// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __x86_64__
#include <immintrin.h>
#endif
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ringbuf.h"
#include "helpers.h"

#define NS_PER_SEC 1000000000ULL

#define TRACE_WRITER(...) do { if (verbose_trace) fprintf(stderr, "writer: " __VA_ARGS__); } while (0)
#define TRACE_READER(...) do { if (verbose_trace) fprintf(stderr, "reader: " __VA_ARGS__); } while (0)

static inline void cpu_relax(void)
{
    _mm_pause();
}
static bool verbose_trace = false;

static inline uint64_t rdtsc_start(void)
{
    unsigned hi, lo;
    __asm__ __volatile__("cpuid\n\trdtsc\n\t"
                         : "=a"(lo), "=d"(hi)
                         :
                         : "%rbx", "%rcx");
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtsc_end(void)
{
    unsigned hi, lo;
    __asm__ __volatile__("rdtscp\n\t"
                         : "=a"(lo), "=d"(hi)
                         :
                         : "%rbx", "%rcx");
    __asm__ __volatile__("cpuid\n\t" ::: "%rax", "%rbx", "%rcx", "%rdx");
    return ((uint64_t)hi << 32) | lo;
}

static double calibrate_tsc_cycles_per_ns(void)
{
    struct timespec start, end;
    uint64_t t0 = rdtsc_start();
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        perror("clock_gettime");
        exit(1);
    }

    sleep(1);

    uint64_t t1 = rdtsc_end();
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        perror("clock_gettime");
        exit(1);
    }

    uint64_t ns = (uint64_t)(end.tv_sec - start.tv_sec) * NS_PER_SEC +
                  (uint64_t)(end.tv_nsec - start.tv_nsec);
    if (ns == 0) {
        fprintf(stderr, "calibrate_tsc: elapsed ns is zero\n");
        exit(1);
    }

    return (double)(t1 - t0) / (double)ns;
}

static void pin_cpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        PRINT_FMT(64, "error: indicated cpu %d does not exsist?", cpu);
        die("sched_setaffinity");
    }
}

struct msg {
    uint64_t seq;
    uint64_t tsc;
};

static int cmp_u64(const void *a, const void *b)
{
    uint64_t ua = *(const uint64_t *)a;
    uint64_t ub = *(const uint64_t *)b;
    return (ua > ub) - (ua < ub);
}

static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --dev PATH [--mode single|fork|pingpong] [options]\n", prog);
    printf("\n");
    printf("Required:\n");
    printf("  --dev PATH          ring device (name, ringbuf_name, or full /dev path)\n");
    printf("\n");
    printf("Modes:\n");
    printf("  --mode single       one process; mmap observe + write/read in process\n");
    printf("  --mode fork         two processes; writer and mmap reader (default)\n");
    printf("  --mode pingpong     two processes; one in-flight message at a time\n");
    printf("\n");
    printf("Options:\n");
    printf("  --duration SEC      run time in seconds (default: 5)\n");
    printf("  --samples N         max samples (default: 100000)\n");
    printf("  --writer-cpu N      writer CPU (default: 0)\n");
    printf("  --reader-cpu N      reader CPU (default: 1)\n");
    printf("  --prime N           warmup writes before measuring (default: 0)\n");
    printf("  --verbose-trace     emit trace messages\n");
    printf("  --help              show this help\n");
    printf("\n");
    printf("Example:\n");
    printf("   %s --dev /dev/ringbuf_msft  --duration 10 --samples 100000 --mode fork --writer-cpu 0 --reader-cpu 1", prog);
    printf("\n");
}

struct bench_args {
    const char *device;
    const char *mode;
    int duration;
    size_t max_samples;
    int writer_cpu;
    int reader_cpu;
    size_t prime;
};

static void sample_sort_and_print(uint64_t *samples, size_t collected, double cycles_per_ns)
{
    if (collected == 0) {
        printf("No samples collected\n");
        return;
    }

    qsort(samples, collected, sizeof(uint64_t), cmp_u64);

    double min_ns = (double)samples[0] / cycles_per_ns;
    double p50_ns = (double)samples[collected / 2] / cycles_per_ns;
    double p90_ns = (double)samples[(collected * 90) / 100] / cycles_per_ns;
    double p99_ns = (double)samples[(collected * 99) / 100] / cycles_per_ns;
    double max_ns = (double)samples[collected - 1] / cycles_per_ns;

    printf("cycles_per_ns=%.2f\n", cycles_per_ns);
    printf("latency_min_ns=%.6f\n", min_ns);
    printf("latency_p50_ns=%.6f\n", p50_ns);
    printf("latency_p90_ns=%.6f\n", p90_ns);
    printf("latency_p99_ns=%.6f\n", p99_ns);
    printf("latency_max_ns=%.6f\n", max_ns);
}

static void reader_consume_mmap(uint8_t *data,
                                size_t cap,
                                uint64_t *reader_pos,
                                uint64_t snapshot_head,
                                uint64_t tail_now,
                                uint64_t *samples,
                                size_t *collected,
                                size_t max_samples,
                                double cycles_per_ns)
{
    const size_t msg_size = sizeof(struct msg);

    if (snapshot_head < *reader_pos)
        *reader_pos = snapshot_head;

    if (snapshot_head - *reader_pos > cap)
        *reader_pos = snapshot_head;

    while (*reader_pos + msg_size <= snapshot_head && *collected < max_samples) {
        if (*reader_pos < tail_now)
            *reader_pos = tail_now;

        if (*reader_pos + msg_size > snapshot_head)
            break;

        size_t off = (size_t)(*reader_pos & (cap - 1));
        struct msg tmp;

        if (off + msg_size <= cap) {
            memcpy(&tmp, data + off, msg_size);
        } else {
            size_t first = cap - off;
            uint8_t buf[sizeof(struct msg)];
            memcpy(buf, data + off, first);
            memcpy(buf + first, data, msg_size - first);
            memcpy(&tmp, buf, msg_size);
        }

        uint64_t delta_cycles = rdtsc_end() - tmp.tsc;
        samples[(*collected)++] = delta_cycles;
        *reader_pos += msg_size;
    }

    (void)cycles_per_ns;
}

static int run_single(struct bench_args *args,
                      int wfd,
                    //   int rfd,
                      struct rbuf_info *info,
                      uint8_t *base,
                      uint64_t *samples,
                      double cycles_per_ns)
{
    volatile uint64_t *headp = (volatile uint64_t *)(base + OFF_HEAD);
    volatile uint64_t *tailp = (volatile uint64_t *)(base + OFF_TAIL);
    uint8_t *data = base + OFF_DATA;
    // const size_t msg_size = sizeof(struct msg);
    size_t collected = 0;
    uint64_t seq = 0;
    uint64_t reader_pos = __atomic_load_n((const uint64_t *)headp, __ATOMIC_ACQUIRE);

    for (size_t i = 0; i < args->prime; ++i) {
        struct msg m = { .seq = seq++, .tsc = rdtsc_start() };
        ssize_t n;
        do {
            n = write(wfd, &m, sizeof(m));
        } while (n < 0 && errno == EINTR);
        if (n != (ssize_t)sizeof(m)) {
            perror("write");
            return 1;
        }

        uint64_t snapshot_head = __atomic_load_n((const uint64_t *)headp, __ATOMIC_ACQUIRE);
        uint64_t tail_now = __atomic_load_n((const uint64_t *)tailp, __ATOMIC_ACQUIRE);
        reader_consume_mmap(data, info->cap, &reader_pos, snapshot_head, tail_now,
                            samples, &collected, args->max_samples, cycles_per_ns);
    }

    uint64_t start = rdtsc_start();
    uint64_t duration_cycles = (uint64_t)((double)args->duration * (double)NS_PER_SEC * cycles_per_ns);

    while ((rdtsc_end() - start) < duration_cycles && collected < args->max_samples) {
        struct msg m = { .seq = seq++, .tsc = rdtsc_start() };
        ssize_t n;
        do {
            n = write(wfd, &m, sizeof(m));
        } while (n < 0 && errno == EINTR);
        if (n != (ssize_t)sizeof(m)) {
            perror("write");
            return 1;
        }

        uint64_t snapshot_head = __atomic_load_n((const uint64_t *)headp, __ATOMIC_ACQUIRE);
        uint64_t tail_now = __atomic_load_n((const uint64_t *)tailp, __ATOMIC_ACQUIRE);
        reader_consume_mmap(data, info->cap, &reader_pos, snapshot_head, tail_now,
                            samples, &collected, args->max_samples, cycles_per_ns);
    }

    printf("mode=single collected=%zu\n", collected);
    sample_sort_and_print(samples, collected, cycles_per_ns);
    return 0;
}

static int run_fork(struct bench_args *args,
                    int wfd,
                    int rfd,
                    struct rbuf_info *info,
                    uint8_t *base,
                    uint64_t *samples,
                    double cycles_per_ns)
{
    volatile uint64_t *headp = (volatile uint64_t *)(base + OFF_HEAD);
    volatile uint64_t *tailp = (volatile uint64_t *)(base + OFF_TAIL);
    uint8_t *data = base + OFF_DATA;
    size_t collected = 0;
    uint64_t reader_pos = __atomic_load_n((const uint64_t *)headp, __ATOMIC_ACQUIRE);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        pin_cpu(args->writer_cpu);
        close(rfd);

        uint64_t seq = 0;
        while (1) {
            struct msg m = { .seq = seq++, .tsc = rdtsc_start() };
            ssize_t n;
            do {
                n = write(wfd, &m, sizeof(m));
            } while (n < 0 && errno == EINTR);
            if (n != (ssize_t)sizeof(m)) {
                perror("write");
                _exit(1);
            }
        }
    }

    pin_cpu(args->reader_cpu);
    close(wfd);

    uint64_t start = rdtsc_start();
    uint64_t duration_cycles = (uint64_t)((double)args->duration * (double)NS_PER_SEC * cycles_per_ns);

    while ((rdtsc_end() - start) < duration_cycles && collected < args->max_samples) {
        uint64_t snapshot_head = __atomic_load_n((const uint64_t *)headp, __ATOMIC_ACQUIRE);
        uint64_t tail_now = __atomic_load_n((const uint64_t *)tailp, __ATOMIC_ACQUIRE);
        reader_consume_mmap(data, info->cap, &reader_pos, snapshot_head, tail_now,
                            samples, &collected, args->max_samples, cycles_per_ns);
    }

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    printf("mode=fork collected=%zu\n", collected);
    sample_sort_and_print(samples, collected, cycles_per_ns);
    return 0;
}

static int run_pingpong(struct bench_args *args,
                        int wfd,
                        int rfd,
                        struct rbuf_info *info,
                        uint8_t *base,
                        uint64_t *samples,
                        double cycles_per_ns)
{
    volatile uint64_t *headp = (volatile uint64_t *)(base + OFF_HEAD);
    volatile uint64_t *tailp = (volatile uint64_t *)(base + OFF_TAIL);
    uint8_t *data = base + OFF_DATA;
    const size_t msg_size = sizeof(struct msg);

    size_t collected = 0;
    uint64_t reader_pos;
    pid_t pid;

    /*
     * Claim the reader FD so ADVANCE is allowed.
     * This also matches your module's intended ownership model.
     */
    if (ioctl(rfd, RBUF_IOC_CLAIM) != 0) {
        perror("ioctl(RBUF_IOC_CLAIM)");
        return 1;
    }

    /*
     * Start “live” at current head so we do not measure old backlog.
     * We do NOT write tail directly; the reader will publish progress via ioctl.
     */
    reader_pos = __atomic_load_n((const uint64_t *)headp, __ATOMIC_ACQUIRE);

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* child = writer */
        pin_cpu(args->writer_cpu);
        close(rfd);

        uint64_t seq = 0;

        for (;;) {
            struct msg m = {
                .seq = seq++,
                .tsc = rdtsc_start(),
            };

            /*
             * Capture tail before write so we can wait for exactly one
             * message-worth of consumption.
             */
            uint64_t prev_tail = __atomic_load_n((const uint64_t *)tailp, __ATOMIC_ACQUIRE);

            ssize_t n;
            do {
                n = write(wfd, &m, sizeof(m));
            } while (n < 0 && errno == EINTR);

            if (n != (ssize_t)sizeof(m)) {
                perror("write");
                _exit(1);
            }

            /*
             * Wait until the reader advances tail by one message.
             * This keeps ping-pong to one in-flight message.
             */
            while (__atomic_load_n((const uint64_t *)tailp, __ATOMIC_ACQUIRE) < prev_tail + msg_size) {
                cpu_relax();
                if (kill(getppid(), 0) != 0) _exit(0);
            }
        }
    }

    /* parent = reader */
    pin_cpu(args->reader_cpu);
    close(wfd);

    uint64_t start = rdtsc_start();
    uint64_t duration_cycles =
        (uint64_t)((double)args->duration * (double)NS_PER_SEC * cycles_per_ns);

    while ((rdtsc_end() - start) < duration_cycles &&
           collected < args->max_samples) {

        uint64_t snapshot_head = __atomic_load_n((const uint64_t *)headp, __ATOMIC_ACQUIRE);

        /*
         * If the writer has not produced a new message yet, wait a bit.
         */
        if (reader_pos + msg_size > snapshot_head) {
            cpu_relax();
            continue;
        }

        /*
         * If the writer outruns us for any reason, skip to live head.
         */
        if (snapshot_head - reader_pos > info->cap)
            reader_pos = snapshot_head;

        if (reader_pos + msg_size > snapshot_head)
            continue;

        size_t off = (size_t)(reader_pos & (info->cap - 1));
        struct msg tmp;

        if (off + msg_size <= info->cap) {
            memcpy(&tmp, data + off, msg_size);
        } else {
            size_t first = info->cap - off;
            uint8_t buf[sizeof(struct msg)];
            memcpy(buf, data + off, first);
            memcpy(buf + first, data, msg_size - first);
            memcpy(&tmp, buf, msg_size);
        }

        uint64_t delta_cycles = rdtsc_end() - tmp.tsc;
        samples[collected++] = delta_cycles;

        reader_pos += msg_size;

        /*
         * Publish consumption through the ioctl path, not by writing tailp.
         * This avoids the PROT_READ segfault.
         */
        if (ioctl(rfd, RBUF_IOC_ADVANCE, &reader_pos) != 0) {
            perror("ioctl(RBUF_IOC_ADVANCE)");
            break;
        }
    }

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    /*
     * Release ownership cleanly.
     */
    (void)ioctl(rfd, RBUF_IOC_RELEASE);

    printf("mode=pingpong collected=%zu\n", collected);
    sample_sort_and_print(samples, collected, cycles_per_ns);
    return 0;
}

int main(int argc, char **argv)
{
    TRACE_READER("%s:%s:%d\n", __FILE__, __func__, __LINE__);
    struct bench_args args = {
        .device = NULL,
        .mode = "fork",
        .duration = 5,
        .max_samples = 100000,
        .writer_cpu = 0,
        .reader_cpu = 1,
        .prime = 0,
    };

    static struct option long_opts[] = {
        {"dev", required_argument, 0, 'd'},
        {"mode", required_argument, 0, 'm'},
        {"duration", required_argument, 0, 't'},
        {"samples", required_argument, 0, 's'},
        {"writer-cpu", required_argument, 0, 'w'},
        {"reader-cpu", required_argument, 0, 'r'},
        {"prime", required_argument, 0, 'p'},
        {"verbose-trace", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:m:t:s:w:r:p:vh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': args.device = optarg; break;
        case 'm': args.mode = optarg; break;
        case 't': {
            char *end = NULL;
            long val = strtol(optarg, &end, 10);
            if (*end != '\0' || val <= 0)
                die_ec("invalid --duration", EINVAL, 2);
            args.duration = (int)val;
            break;
        }
        case 's': {
            char *end = NULL;
            unsigned long long val = strtoull(optarg, &end, 10);
            if (*end != '\0' || val == 0)
                die_ec("invalid --samples", EINVAL, 2);
            if (val > SIZE_MAX / sizeof(uint64_t))
                die_ec("--samples too large", EINVAL, 2);
            args.max_samples = (size_t)val;
            break;
        }
        case 'w': args.writer_cpu = atoi(optarg); break;
        case 'r': args.reader_cpu = atoi(optarg); break;
        case 'p': {
            char *end = NULL;
            unsigned long long val = strtoull(optarg, &end, 10);
            if (*end != '\0')
                die_ec("invalid --prime", EINVAL, 2);
            args.prime = (size_t)val;
            break;
        }
        case 'v': verbose_trace = true; break;
        case 'h': usage(argv[0]); return 0;
        default:
            fprintf(stderr,
                "usage: %s --dev PATH [--duration SEC] [--samples N] [--mode single|fork|pingpong] [--writer-cpu N] [--reader-cpu N] [--prime N] [--verbose-trace]\n",
                argv[0]);
            fprintf(stderr,
                "       %s --help\n",
                argv[0]);
            return 2;
        }
    }

    if (!args.device) {
        fprintf(stderr, "--dev required\n");
        usage(argv[0]);
        return 2;
    }

    char dev_path[PATH_MAX];
    if (!normalize_dev(dev_path, sizeof(dev_path), args.device))
        die_ec("invalid --dev parameter", EINVAL, 2);

    int wfd = open(dev_path, O_WRONLY);
    if (wfd < 0)
        die("open writer");

    int rfd = open(dev_path, O_RDONLY);
    if (rfd < 0)
        die("open reader");

    struct rbuf_info info;
    if (ioctl(rfd, RBUF_IOC_INFO, &info) != 0)
        die("ioctl(INFO)");

    if ((info.cap & (info.cap - 1)) != 0)
        die_ec("ring buffer capacity must be a power of two", EINVAL, 1);

    double cycles_per_ns = calibrate_tsc_cycles_per_ns();

    uint8_t *base = mmap(NULL, info.size, PROT_READ, MAP_SHARED, rfd, 0);
    if (base == MAP_FAILED)
        die("mmap");
    uint64_t *samples = calloc(args.max_samples, sizeof(uint64_t));
    if (!samples)
        die("calloc");

    int rc = 0;
    if (strcmp(args.mode, "single") == 0) {
        // rc = run_single(&args, wfd, rfd, &info, base, samples, cycles_per_ns);
        rc = run_single(&args, wfd, &info, base, samples, cycles_per_ns);
    } else if (strcmp(args.mode, "fork") == 0) {
        rc = run_fork(&args, wfd, rfd, &info, base, samples, cycles_per_ns);
    } else if (strcmp(args.mode, "pingpong") == 0) {
        rc = run_pingpong(&args, wfd, rfd, &info, base, samples, cycles_per_ns);
    } else {
        fprintf(stderr, "unknown --mode '%s'\n", args.mode);
        usage(argv[0]);
        rc = 2;
    }

    munmap(base, info.size);
    close(wfd);
    close(rfd);
    free(samples);
    return rc;
}
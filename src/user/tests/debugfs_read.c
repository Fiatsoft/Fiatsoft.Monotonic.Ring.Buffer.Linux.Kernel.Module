// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libgen.h>

#include "ringbuf.h"
#include "helpers.h"

#define BUF_SIZ        65536
#define ADDING_DEFAULT  64

static int verbose = 0;

static size_t min_size(size_t a, size_t b)
{
    return (a < b) ? a : b;
}

static void print_line(void)
{
    puts("======================\n");
}

static ssize_t read_all(int fd, uint8_t *buf, size_t cap)
{
    size_t total = 0;

    while (total < cap) {
        ssize_t n = read(fd, buf + total, cap - total);
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        total += (size_t)n;
    }

    return (ssize_t)total;
}

static void dump_hex(const char *label, const uint8_t *buf, size_t len)
{
    if (!verbose)
        return;

    printf("=== %s (%zu bytes) ===\n", label, len);

    for (size_t i = 0; i < len; i += 16) {
        printf("%04zx  ", i);

        for (size_t j = 0; j < 16; j++) {
            if (i + j < len)
                printf("%02x ", buf[i + j]);
            else
                printf("   ");
        }

        printf(" |");

        for (size_t j = 0; j < 16 && (i + j) < len; j++) {
            unsigned char c = buf[i + j];
            putchar(isprint(c) ? c : '.');
        }

        printf("|\n");
    }

    putchar('\n');
}

static void die_open_path(const char *what, const char *path)
{
    fprintf(stderr, "%s: open(%s) failed: ", what, path);
    perror("");
    exit(EXIT_FAILURE);
}

static int reopen_ro_fd(int old_fd, const char *path)
{
    if (old_fd >= 0)
        close(old_fd);

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        die_open_path("reopen_ro_fd", path);

    return fd;
}

static void refresh_projections(int *tap_fd, int *dump_fd,
                                const char *tap_path,
                                const char *dump_path)
{
    if (tap_fd)
        *tap_fd = reopen_ro_fd(*tap_fd, tap_path);
    if (dump_fd)
        *dump_fd = reopen_ro_fd(*dump_fd, dump_path);
}

static void run_producer(const char *dev_path, size_t bytes, bool verbose_run)
{
    char bytes_str[32];
    snprintf(bytes_str, sizeof(bytes_str), "%zu", bytes);

    char *argv[] = {
        "./build/user/tests/monotonic_stream_producer",
        "--dev", (char *)dev_path,
        "--bytes", bytes_str,
        NULL
    };

    spawn_and_wait(argv, verbose_run);
}

static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  sudo %s --dev-name PATH [options]\n", prog);
    printf("\n");
    printf("Required:\n");
    printf("  --dev-name NAME          ring device (name, device-name only), not full-path\n");
    printf("\n");
    printf("Options:\n");
    printf("  --verbose                print status and hex dumps\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s --dev-name ringbuf_device0 --verbose\n", prog);
    printf("\n");
    printf("Remarks:\n");
    printf("  * Requires elevation.\n");
    printf("  * Assumes no concurrent writers (test is not synchronized).\n");
    printf("\n");
}

int main(int argc, char **argv)
{
    char dev_path[PATH_MAX];
    char tap_path[PATH_MAX];
    char dump_path[PATH_MAX];

    const char *dev = NULL;
    const char *tap = NULL;
    const char *dump = NULL;

    static struct option opts[] = {
        {"dev-name", required_argument, 0, 'd'},
        {"verbose",  no_argument,       0, 'v'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:vh", opts, NULL)) != -1) {
        switch (c) {
        case 'd':
            dev = optarg;
            tap = dump = dev;
            break;
        case 'v':
            verbose = 1;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            fprintf(stderr, "usage: %s --dev-name PATH [--verbose]\n", argv[0]);
            fprintf(stderr, "       %s --help\n", argv[0]);
            return 2;
        }
    }

    if (!dev) {
        fprintf(stderr, "%s: --dev-name required\n", __FILE__);
        usage(argv[0]);
        return 2;
    }

    if (!normalize_dev(dev_path, sizeof(dev_path), dev)) {
        fprintf(stderr, "%s: invalid --dev-name '%s'\n", __FILE__, dev);
        errno = EINVAL;
        perror("normalize_dev");
        return 2;
    }
    if (!normalize_tap(tap_path, sizeof(tap_path), tap)) {
        fprintf(stderr, "%s: invalid --dev-name '%s'\n", __FILE__, dev);
        errno = EINVAL;
        perror("normalize_tap");
        return 2;
    }
    if (!normalize_dump(dump_path, sizeof(dump_path), dump)) {
        fprintf(stderr, "%s: invalid --dev-name '%s'\n", __FILE__, dev);
        errno = EINVAL;
        perror("normalize_dump");
        return 2;
    }

    printf("[SETUP] open files, form estimates, produce test data\n");
    printf("-----------------------------------------------------\n");

    int dev_fd  = open(dev_path, O_RDONLY);
    int tap_fd  = open(tap_path, O_RDONLY);
    int dump_fd = open(dump_path, O_RDONLY);

    assert(dev_fd >= 0);
    assert(tap_fd >= 0);
    assert(dump_fd >= 0);
    
    if (ioctl(dev_fd, RBUF_IOC_CLAIM) != 0) {
        close(dev_fd);
        die_c("ioctl(CLAIM)",1);
    }

    struct rbuf_info info;
    uint64_t avail_u64 = get_avail(dev_fd, &info);
    size_t avail = (size_t)avail_u64;
    size_t adding = ADDING_DEFAULT;

    printf("precondition: no concurrent writers (test is not synchronized)\n");
    printf("state: head=%" PRIu64 " tail=%" PRIu64 " used=%" PRIu64 " cap=%" PRIu64 "\n",
           info.head, info.tail, avail_u64, info.cap);
    print_line();

    // uint8_t buf1[BUF_SIZ];
    // uint8_t buf2[BUF_SIZ];

    if (ioctl(dev_fd, RBUF_IOC_INFO, &info) != 0) {
        perror("ioctl(INFO)");
        exit(1);
    }
    uint8_t *buf1 = malloc(info.cap);
    uint8_t* buf2 = malloc(info.cap);
    if (!buf1 || !buf2) {
        perror("malloc");
        exit(1);
    }

    /*
     * TEST 1 —  view existing data
     */
    printf("[TEST 1: view existing data] views gets expected size per ioctl-info\n");
    printf("-----------------------------------------------------\n");
    printf("expectation: tap should read %zu bytes\n", avail);
    ssize_t tap_n = read_all(tap_fd, buf1, info.cap);
    printf("action: tap read %zd bytes\n", tap_n);
    assert(tap_n >= 0);
    assert((size_t)tap_n == avail);
    printf("expectation: tap should read EOF (zero bytes)\n");
    ssize_t tap_n2 = read(tap_fd, buf1, info.cap);
    if (verbose)
        printf("state: tap second read %zd\n", tap_n2);
    assert(tap_n2 == 0);

    size_t expected_dump = (size_t)min_size((size_t)info.head, (size_t)info.cap);
    printf("expectation: dump should read %zu bytes\n", expected_dump);
    ssize_t dump_n = read_all(dump_fd, buf1, info.cap);
    printf("action: dump read %zd bytes\n", dump_n);
    assert(dump_n >= 0);
    assert((size_t)dump_n == expected_dump);
    printf("expectation: dump should read EOF (zero bytes)\n");
    ssize_t dump_n2 = read(dump_fd, buf1, info.cap);
    if (verbose)
        printf("state: dump second read %zd\n", dump_n2);
    assert(dump_n2 == 0);

    printf("\nRESULT: PASS\n");
    print_line();

    /*
     * TEST 2 — tap snapshot semantics
     */
    printf("[TEST 2: snapshot semantics] tap won't see new writes\n");
    printf("-----------------------------------------------------\n");
    printf("setup: re-setting tap-read cursor.\n");
    lseek(tap_fd, 0, SEEK_SET);

    if (verbose)
        printf("action: producing %zu bytes...\n", adding);

    run_producer(dev_path, adding, verbose);
    printf("action: produced %zu bytes.\n", adding);

    printf("expectation: tap should still read %zu bytes\n", avail);
    ssize_t tap_old_n = read(tap_fd, buf1, info.cap);
    printf("state: tap read %zd bytes\n", tap_old_n);
    assert(tap_old_n >= 0);
    assert((size_t)tap_old_n == avail);

    printf("action: re-open tap FD\n");
    tap_fd = reopen_ro_fd(tap_fd, tap_path);
    uint64_t avail_new_u64 = get_avail(dev_fd, &info);
    size_t avail_new = (size_t)avail_new_u64;

    printf("state: head=%" PRIu64 " tail=%" PRIu64 " used=%" PRIu64 " cap=%" PRIu64 "\n",
           info.head, info.tail, avail_new_u64, info.cap);
    printf("expectation: %s should have %zu unread bytes per ioctl, greater than older reading\n",
           dev_path, avail_new);
    printf("state: avail_new > avail: %zu > %zu\n", avail_new, avail);
    assert(avail_new > avail);

    printf("expectation: tap should read %zu bytes\n", avail_new);
    ssize_t tap_new_n = read(tap_fd, buf1, info.cap);
    printf("state: tap read %zd bytes\n", tap_new_n);
    assert(tap_new_n > tap_old_n);
    assert((size_t)tap_new_n == avail_new);

    printf("\nRESULT: PASS\n");
    print_line();
    
    /*
     * TEST 3 — tap idempotence
     */
    printf("[TEST 3: idempotent reads] is /tap data stable across reads?\n");
    printf("-----------------------------------------------------\n");
    avail = avail_new;

    printf("setup: re-setting tap-read cursor.\n");
    lseek(tap_fd, 0, SEEK_SET);
    printf("expectation: tap should read %zu bytes\n", avail);
    ssize_t t1 = read(tap_fd, buf1, info.cap);
    printf("action: tap read %zd bytes\n", t1);

    printf("setup: re-setting tap-read cursor.\n");
    lseek(tap_fd, 0, SEEK_SET);
    printf("expectation: tap should read %zu bytes again\n", avail);
    ssize_t t2 = read(tap_fd, buf2, info.cap);
    printf("action: tap read %zd bytes again\n", t2);

    if (verbose)
        printf("state: length: read1=%zd, read2=%zd\n", t1, t2);

    assert(t1 == t2);
    assert(memcmp(buf1, buf2, (size_t)t1) == 0);
    printf("state: both reads have the same contents, byte-wise\n");

    printf("action: re-open tap FD, re-run test\n");
    tap_fd = reopen_ro_fd(tap_fd, tap_path);
    printf("expectation: tap should read %zu bytes\n", avail);
    t1 = read(tap_fd, buf1, info.cap);
    printf("action: tap read %zd bytes\n", t1);

    printf("setup: re-setting tap-read cursor.\n");
    lseek(tap_fd, 0, SEEK_SET);
    printf("expectation: tap should read %zu bytes again\n", avail);
    t2 = read(tap_fd, buf2, info.cap);
    printf("action: tap read %zd bytes again\n", t2);

    if (verbose)
        printf("state: length: read1=%zd, read2=%zd\n", t1, t2);

    assert(t1 == t2);
    assert(memcmp(buf1, buf2, (size_t)t1) == 0);
    printf("state: both reads have the same contents, byte-wise\n");
    
    
    printf("\nRESULT: PASS\n");
    print_line();

    /*
     * TEST 4 — dump contains tap window
     */
    printf("[TEST 4: superset property] Is /dump a superset of (contains) /tap?\n");
    printf("-----------------------------------------------------\n");
    printf("setup: re-opening dump-reader, re-setting tap-read cursor.\n");
    dump_fd = reopen_ro_fd(dump_fd, tap_path);
    // lseek(dump_fd, 0, SEEK_SET);
    lseek(tap_fd, 0, SEEK_SET);

    avail_u64 = get_avail(dev_fd, &info);
    avail = (size_t)avail_u64;

    printf("state: head=%" PRIu64 " tail=%" PRIu64 " avail=%" PRIu64 " cap=%" PRIu64 "\n",
           info.head, info.tail, avail_u64, info.cap);
    printf("expectation: tap should read %zu bytes, dump %zu bytes\n",
           avail, (size_t)min_size((size_t)info.head, (size_t)info.cap));

    tap_n = read(tap_fd, buf2, info.cap);
    dump_n = read_all(dump_fd, buf1, info.cap);
    printf("action: tap read %zd, dump read %zd\n", tap_n, dump_n);

    printf("expectation: dump has the same contents as tap byte-wise, appended and possibly wrapped\n");
    if (verbose) {
        printf("state: for visual validation\n");
        dump_hex(dump_path, buf1, (size_t)dump_n);
        dump_hex(tap_path, buf2, (size_t)tap_n);
    }
    
    bool found = false;
    for (size_t i = 0; i + (size_t)tap_n <= (size_t)dump_n; i++) {
        if (memcmp(buf1 + i, buf2, (size_t)tap_n) == 0) {
            found = true;
            break;
        }
    }
    if (!found)
        printf("state: superset property: VIOLATED\n");
    assert(found);

    printf("\nRESULT: PASS\n");
    print_line();

    /*
     * TEST 5 — writer interaction
     */
    printf("[TEST 5: writer interaction] is added data visible?\n");
    printf("-----------------------------------------------------\n");
    printf("setup: re-setting tap-read cursor.\n");
    lseek(tap_fd, 0, SEEK_SET);

    if (verbose)
        printf("action: producing %zu bytes...\n", adding);
    run_producer(dev_path, adding, verbose);
    printf("action: produced %zu bytes.\n", adding);

    printf("expectation: tap should read %zd bytes on old FD\n", tap_n);
    ssize_t tap_old_after_write = read(tap_fd, buf1, info.cap);
    printf("tap read: %zd bytes\n", tap_old_after_write);
    assert(tap_old_after_write >= 0);
    assert(tap_old_after_write == tap_n);

    printf("action: re-opening /tap FD\n");
    tap_fd = reopen_ro_fd(tap_fd, tap_path);

    printf("expectation: tap should read %zd bytes on new FD\n", tap_n + (ssize_t)adding);
    ssize_t tap_new_after_write = read(tap_fd, buf1, info.cap);
    printf("tap read: %zd bytes\n", tap_new_after_write);
    assert(tap_new_after_write == tap_n + (ssize_t)adding);

    printf("\nRESULT: PASS\n");
    print_line();

    /*
     * TEST 6 — drain interaction
     */
    printf("[TEST 6: drain interaction] do views function correctly post drain?\n");
    printf("-----------------------------------------------------\n");
    if (verbose)
        printf("setup: draining ringbuf-device...\n");

    if (ioctl(dev_fd, RBUF_IOC_RELEASE) != 0) {
        close(dev_fd);
        die_c("ioctl(RELEASE)",1);
    }    
    char* reader_duration[] =  {
        "./build/user/tests/reader_duration",
        "--dev", (char *)dev_path,
        "--duration", "3",
        NULL
    };
    spawn_and_wait(reader_duration, verbose);

    printf("setup: drained ringbuf-device.\n");
    run_producer(dev_path, adding, verbose);
    printf("setup: produced %zu bytes\n", adding);
    avail_u64 = get_avail(dev_fd, &info);
    avail = (size_t)avail_u64;
    printf("state: head=%" PRIu64 " tail=%" PRIu64 " used=%" PRIu64 " cap=%" PRIu64 "\n",
           info.head, info.tail, avail_u64, info.cap);
           
    printf("action: read tap\n");
    printf("expectation: tap should read %zu bytes\n", adding);

    refresh_projections(&tap_fd, &dump_fd, tap_path, dump_path);
    tap_n = read_all(tap_fd, buf1, info.cap);
    printf("tap read: %zd bytes\n", tap_n);
    assert(tap_n >= 0);
    assert(tap_n == (ssize_t)adding);
    expected_dump = (size_t)min_size((size_t)info.head, (size_t)info.cap);
    printf("expectation: dump should read %zu bytes\n", expected_dump);
    dump_n = read_all(dump_fd, buf1, info.cap);
    printf("dump read: %zd bytes\n", dump_n);
    assert(dump_n >= 0);
    assert(dump_n == (ssize_t)expected_dump);

    printf("action: drain again, prove accumulation in /dump\n");
    spawn_and_wait(reader_duration, verbose);
    run_producer(dev_path, adding, verbose);
    refresh_projections(&tap_fd, &dump_fd, tap_path, dump_path);
    avail_u64 = get_avail(dev_fd, &info);
    avail = (size_t)avail_u64;
    expected_dump = (size_t)min_size((size_t)info.head, (size_t)info.cap);
    assert(avail == adding);
    printf("action: read views\n");
    printf("expectation: tap should read %zu bytes\n", adding);
    tap_n = read_all(tap_fd, buf1, info.cap);
    printf("tap read: %zd bytes\n", tap_n);
    assert(tap_n >= 0);
    assert(tap_n == (ssize_t)adding);
    printf("expectation: dump should read %zu bytes\n", expected_dump);
    dump_n = read_all(dump_fd, buf1, info.cap);
    printf("dump read: %zd bytes\n", dump_n);
    assert(dump_n >= 0);
    assert(dump_n == (ssize_t) (expected_dump));

    printf("\nRESULT: PASS\n");
    print_line();

    close(dev_fd);
    close(tap_fd);
    close(dump_fd);

    free(buf1);
    free(buf2);

    printf("ALL DEBUGFS TESTS PASS\n");
    return 0;
}

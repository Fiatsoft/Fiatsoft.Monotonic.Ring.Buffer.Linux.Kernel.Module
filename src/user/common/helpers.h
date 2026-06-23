/* Copyright (c) 2026 fiatsoft.com.
 * SPDX-License-Identifier: GPL-2.0-only */

#ifndef HELPERS_H
#define HELPERS_H

#pragma once

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/ioctl.h>

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

#define PRINT_FMT(size, fmt, ...) do { \
    char _tmp[size]; \
    snprintf(_tmp, size, fmt, ##__VA_ARGS__); \
    puts(_tmp); \
} while(0)

/*
 * Open a ringbuf device using argv-style arguments.
 *
 * Parameters:
 *   argv, argc      - command-line arguments
 *   base            - base device name (e.g., "ringbuf")
 *   appendage       - optional suffix (e.g., instance name)
 *   open_flags      - flags passed to open(2)
 *   resolved_path   - output buffer for final device path
 *   resolved_len    - size of resolved_path buffer
 *
 * Returns:
 *   file descriptor on success, negative on failure
 *
 * Notes:
 *   - Accepts shorthand device names (e.g., "msft")
 *   - Resolves to /dev/ringbuf_<name>
 */
int open_ringbuf_from_argv(
    char** argv,
    int argc, 
    const char *base,
    char *appendage,
    int open_flags,
    char *resolved_path,
    size_t resolved_len
);

/*
 * Print error message and exit.
 */
void die_ec(const char *msg, int _errno, int exit_code);
void die_c(const char *msg, int exit_code);
void die(const char *msg);

/*
 * Return monotonic time in nanoseconds.
 *
 * Uses CLOCK_MONOTONIC.
 */
uint64_t now_ns(void);

/*
 * Normalize a device name into a full /dev path.
 *
 * Accepts:
 *   "msft"              → "/dev/ringbuf_msft"
 *   "ringbuf_msft"      → "/dev/ringbuf_msft"
 *   "/dev/ringbuf_msft" → unchanged
 *
 * Output is written to 'out'.
 */
bool normalize_dev(char *out, size_t sz, const char *in);

bool normalize_tap(char *out, size_t sz, const char *in);

bool normalize_dump(char *out, size_t sz, const char *in);

void trim_ws(char *s);

/*
USAGE:
run_cmd((char *[]) {
    "./bin/monotonic_stream_producer",
    "monotonic_stream_producer",
    "--dev", dev_path,
    "--bytes", "64",
    NULL
}); */
void run_cmd(char *const argv[], bool verbose);

/* 
USAGE:
* char adding_str[32];
* snprintf(adding_str, sizeof(adding_str), "%zd", adding);
* spawn_and_wait((char *[]) {
*     "./bin/monotonic_stream_producer",
*     "monotonic_stream_producer",
*     "--dev", dev_path,
*     "--bytes", adding_str,
*     NULL
* }); */
void spawn_and_wait(char *const argv[], bool verbose);

uint64_t get_avail(int dev_fd, struct rbuf_info *info_out);

void handle_failed_ringbuf_dev_open(const char *dev_path, int err);
void handle_failed_ringbuf_tap_open(const char *tap_path, int err);

#endif
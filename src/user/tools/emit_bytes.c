// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s STRING\n", argv[0]);
        return 1;
    }

    const char *s = argv[1];
    size_t len = strlen(s);

    if (write(STDOUT_FILENO, s, len) != (ssize_t)len) {
        perror("write");
        return 1;
    }

    return 0;
}
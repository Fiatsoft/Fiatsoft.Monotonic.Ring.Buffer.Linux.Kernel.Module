// Copyright (c) 2026 fiatsoft.com.
// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>

volatile int stop = 0;

void *cpu_hog(void *arg)
{
    (void)arg;
    while (!stop) {
        asm volatile("" ::: "memory");
    }
    return NULL;
}

int main(int argc, char *argv[])
{
const char *latency_test_command;
const char *latency_test_command_default = "./build/user/tools/latency_benchmark --dev msft  --duration 3 --samples 3333";
  if (argc < 2 || argv[1][0] == '\0') {
    latency_test_command = latency_test_command_default;
  }
  else {
    latency_test_command = argv[1];
  }

  pthread_t hogs[4];

  // create scheduler pressure
  for (int i = 0; i < 4; i++)
      pthread_create(&hogs[i], NULL, cpu_hog, NULL);

  // run your ring test here
  system(latency_test_command);

  stop = 1;

  for (int i = 0; i < 4; i++)
      pthread_join(hogs[i], NULL);

  return 0;
}
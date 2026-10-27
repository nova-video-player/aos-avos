// Copyright 2026 Courville Software
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once
/*
 * Small helpers shared by the concurrency tests in this directory: a CHECK macro that keeps going
 * (so one run reports every failure), and a watchdog that turns a deadlock into a readable FAIL
 * line instead of a silent timeout in the Python runner.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int t_fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); t_fails++; } \
                           else { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *t_watchdog_name = "test";
static void t_watchdog_fired(int sig) {
    (void)sig;
    static const char msg[] = "FAIL: watchdog expired -- deadlock or lost wake-up\n";
    if (write(1, msg, sizeof msg - 1) < 0) { /* nothing useful to do */ }
    _exit(2);
}
/* Call first thing in main(). `secs` should be several times a normal run's length. */
static inline void t_watchdog(int secs, const char *name) {
    t_watchdog_name = name;
    setvbuf(stdout, NULL, _IOLBF, 0);   /* line-buffered even when piped: a killed/_exit()ed test must not lose its FAIL lines */
    signal(SIGALRM, t_watchdog_fired);
    alarm((unsigned)secs);
}
static inline int t_finish(void) {
    printf("%s: %s\n", t_watchdog_name, t_fails ? "FAILED" : "ALL PASSED");
    return t_fails != 0;
}

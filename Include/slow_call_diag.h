/*
 * Copyright 2026 Courville Software
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AVOS_SLOW_CALL_DIAG_H
#define AVOS_SLOW_CALL_DIAG_H

#include <stdint.h>
#include "debug.h"

#if DEBUG_LOGS && defined(DEBUG_MSG) && defined(CONFIG_ANDROID)
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#define SLOW_CALL_DIAG_ENABLED (Debug[DBG_SINK] != 0)
#else
#define SLOW_CALL_DIAG_ENABLED 0
#endif

typedef struct {
	int64_t wall_ns;
	int64_t cpu_ns;
	int active;
} slow_call_diag_t;

static inline slow_call_diag_t slow_call_diag_begin(int enabled)
{
	slow_call_diag_t d = {0};
#if DEBUG_LOGS && defined(DEBUG_MSG) && defined(CONFIG_ANDROID)
	if (enabled) {
		int saved_errno = errno;
		struct timespec wall, cpu;
		if (!clock_gettime(CLOCK_MONOTONIC, &wall)) {
			d.wall_ns = (int64_t)wall.tv_sec * 1000000000LL + wall.tv_nsec;
			d.cpu_ns = !clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) ?
				(int64_t)cpu.tv_sec * 1000000000LL + cpu.tv_nsec : -1;
			d.active = 1;
		}
		errno = saved_errno;
	}
#else
	(void)enabled;
#endif
	return d;
}

// Report wall time and CPU separately: low CPU implies waiting or descheduling,
// not necessarily a blocked HAL/socket. No record means only "no slow return";
// a call still blocked at capture end cannot emit a completion record.
static inline void slow_call_diag_end(slow_call_diag_t d, const char *operation,
	int64_t result, int64_t bytes, int audio_queue, int video_queue)
{
#if DEBUG_LOGS && defined(DEBUG_MSG) && defined(CONFIG_ANDROID)
	if (!d.active)
		return;
	int saved_errno = errno;
	struct timespec wall, cpu;
	if (!clock_gettime(CLOCK_MONOTONIC, &wall)) {
		int64_t end = (int64_t)wall.tv_sec * 1000000000LL + wall.tv_nsec;
		if (end - d.wall_ns >= 100000000LL) {
			int64_t cpu_ns = d.cpu_ns >= 0 && !clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) ?
				(int64_t)cpu.tv_sec * 1000000000LL + cpu.tv_nsec - d.cpu_ns : -1;
			serprintf_record("playback_slow_call: operation=%s tid=%ld start_ns=%lld end_ns=%lld wall_us=%lld cpu_us=%lld result=%lld bytes=%lld audio_queue=%d video_queue=%d\n",
				operation, (long)syscall(SYS_gettid), (long long)d.wall_ns, (long long)end,
				(long long)((end - d.wall_ns) / 1000), (long long)(cpu_ns >= 0 ? cpu_ns / 1000 : -1),
				(long long)result, (long long)bytes, audio_queue, video_queue);
		}
	}
	errno = saved_errno;
#else
	(void)d; (void)operation; (void)result; (void)bytes; (void)audio_queue; (void)video_queue;
#endif
}
#endif

/*
 * Copyright 2017 Archos SA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __ATIME_H__
#define __ATIME_H__

#include "types.h"

#include <time.h>
#include <errno.h>

typedef struct tm TM;

extern volatile ULONG  s_time;		// time in 1s steps, like UNIX system time, counts seconds since 00:00:00 on January 1, 1970 (UTC).
extern volatile ULONG  d_time;		// time in 1/10s steps
extern volatile ULONG  m_time;		// time in 1/1000s steps

time_t secure_time( time_t *t );
time_t user_time  ( time_t *t );

int atime( void );			// "archos" time in ms

int time_update_time( void );
void time_init_time( void );

static inline int msec_sleep(unsigned long msecs)
{
	struct timespec treq;
	struct timespec trem;
	int ret;

	if ( msecs >= 1000 ) {
		treq.tv_sec  = msecs / 1000;
		msecs %= 1000;
	} else
		treq.tv_sec = 0;

	treq.tv_nsec = msecs * 1000 * 1000;
	
	do {
		ret = nanosleep(&treq, &trem);
		// time's up, outta here
		if ( ret >= 0 )
			break;

		// interrupted?
		if (errno == EINTR) {
			// continue to wait
			treq = trem;
		} else
			break;

	} while (1);

	return ret;
}

static inline int usec_sleep(unsigned long usecs)
{
	struct timespec treq;
	struct timespec trem;
	
	treq.tv_sec = 0;
	treq.tv_nsec = usecs * 1000;
	
	return nanosleep(&treq, &trem);
}

void atime_print_gmt( const char *msg, time_t *time );
void atime_print_tm( const char *msg, struct tm *time );
void atime_print_local( const char *msg, time_t *time );

void display_clock_debug_values( void );

#endif

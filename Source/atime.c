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

#include "global.h"
#include "types.h"
#include "debug.h"
#include "atime.h"

#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

volatile ULONG  s_time = 0;	// system time = unix time()
volatile ULONG 	d_time = 0;	// monotonic time in 1/10s 
volatile ULONG 	m_time = 0;	// monotonic time in 1/1000s
static   UINT64	n_time = 0;	// system time in nanoseconds

static INT64	time_ref = 0;

#define TIME_STR_LEN 30

static void _print_time( const char *msg, struct tm *tm, const char *format )
{
	char time_str[TIME_STR_LEN];
	char buffer[MAX_NAME_LEN];

	if ( !msg || !tm || !format )
		return;

	asctime_r( tm, time_str );
	time_str[strlen(time_str) - 1] = '\0';
	
	snprintf( buffer, MAX_NAME_LEN, "%s %s%s", msg, time_str, format );
	serprintf( "%s\n", buffer );
}
	
void atime_print_gmt( const char *msg, time_t *time )
{
	struct tm my_tm;

	gmtime_r( time, &my_tm );
	_print_time( msg, &my_tm, "(GMT/UTC)" );
}

void atime_print_local( const char *msg, time_t *time )
{
	struct tm my_tm;

	localtime_r( time, &my_tm );
	_print_time( msg, &my_tm, "(LOCAL)" );
}

void atime_print_tm( const char *msg, struct tm *time_tm )
{	
	_print_time( msg, time_tm, "(UNKNOWN)" );
}

int time_update_time( void )
{
	struct timespec tv;
	static pthread_mutex_t _mutex = PTHREAD_MUTEX_INITIALIZER;
	
	pthread_mutex_lock( &_mutex );

	clock_gettime(CLOCK_MONOTONIC, &tv );
	
	n_time = (UINT64)tv.tv_sec * (UINT64)1000000000 + (UINT64)tv.tv_nsec;
	m_time = (n_time - time_ref) / 1000000;
	d_time = m_time / 100;
	s_time = d_time / 10;

	//serprintf("%u %u %u %u\r\n", s_time, d_time, c_time, m_time ); 

	pthread_mutex_unlock( &_mutex );

	return (n_time - time_ref) / 100000;
}

int atime( void )
{
	time_update_time();
	return m_time;
}

void time_init_time( void )
{
	time_update_time();
	time_ref = n_time;
}


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
#include "tty.h"
#include "log.h"
#include "atime.h"
#include "console.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#ifdef DEBUG_MSG

#define SERPRINTF_BUF_SIZE (1024 * 8)

void LOG_open (void)
{
	TTY_open(CONSOLE_handle_char);
}

void LOG_open_name (const char *name)
{
	LOG_open();
}

void LOG_close(void)
{
	TTY_close();
}

static void sendString( char *text )
{
	static char clean[SERPRINTF_BUF_SIZE];
	char *c = clean;
	
	unsigned char *t = (UCHAR*)text;
	// clean UTF chars
	while( *t ) {
		if( *t > 0x80 ) {
			sprintf( c, "[%02X]", *t++ );
			c += 4;
		} else {
			*c++ = *t++;
		}
	}
	*c = '\0';
	TTY_write( clean );
}

#ifdef USE_MUTEX
	#include "pthread.h"
	static pthread_mutex_t serprintf_mutex = PTHREAD_MUTEX_INITIALIZER;
	#define LOCK	pthread_mutex_lock( &serprintf_mutex );
	#define UNLOCK	pthread_mutex_unlock( &serprintf_mutex );
#else
	static volatile int lock = 0;
	#define LOCK	if( !lock ) { \
		 		lock = 1; \
			} else { \
				while( lock ) \
					msec_sleep( 1 ); \
			} 
			
	#define UNLOCK	lock = 0;
#endif

int vserprintf(const char *fmt, va_list va )
{
	int ret;
	char sbuf[SERPRINTF_BUF_SIZE];
  	
	ret = vsnprintf(sbuf, SERPRINTF_BUF_SIZE - 1, fmt, va);

	LOCK 
	sendString( sbuf );
	UNLOCK 

	return ret;
}

int serprintf(const char *fmt, ...)
{
	int ret;
	
  	va_list ap;

  	va_start (ap, fmt);
	ret = vserprintf( fmt, ap );
  	va_end (ap);

	return ret;
}

// ********************************************************************
//
//	this "printf" catches all printf's in the code, so they do not
//	end up in the stdio.h call!
//
// ********************************************************************
int printf(const char *fmt, ...)
{
	int ret;
	
  	va_list ap;

  	va_start (ap, fmt);
	ret = vserprintf( fmt, ap );
  	va_end (ap);

	return ret;
}

void _LOG(char level, const char *log_tag, const char *fmt, ...)
{
	int ret;
	char sbuf[SERPRINTF_BUF_SIZE];
  	va_list va;

	ret = snprintf(sbuf, SERPRINTF_BUF_SIZE - 1, "%c/%s: ", level, log_tag);
	if (ret <= 0)
		return;
  	va_start (va, fmt);
	ret += vsnprintf(sbuf + ret, SERPRINTF_BUF_SIZE - ret, fmt, va);
  	va_end (va);
	if (ret <= 0)
		return;
	strcat(sbuf, "\n");

	LOCK 
	sendString( sbuf );
	UNLOCK 

	return;
}
#else
void _LOG(char level, const char *log_tag, const char *fmt, ...) {}
#endif

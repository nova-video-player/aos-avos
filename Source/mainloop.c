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
#include "message.h"
#include "atime.h"
#include "debug.h"
#include "timers.h"
#include "threadcom.h"
#include "profiling.h"

#define MAX_TIMERS 32

static Timer gui_timer_data[MAX_TIMERS];

Timers gui_timers = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.data  = gui_timer_data,
	.dummy = { 0, 0, 0, NULL, "" },
	.size  = MAX_TIMERS,
	.cnt   = 0
};

data_event_t mainloop_events = {
	.next = &mainloop_events,
	.prev = &mainloop_events,
	.fd = -1,
	.tainted = 0,
};

#include <sys/select.h>

static threadcom_link_t* mainloop_notifier;
static pthread_mutex_t notifier_lock = PTHREAD_MUTEX_INITIALIZER;

static void _wakeup_event(threadcom_link_t* tlink)
{
	char tmp;
	// we don't do anything with the actual data,
	// just clear the pipe so that the data event stops
	// waking up the mainloop
	pthread_mutex_lock(&notifier_lock);
	while (!threadcom_get_event(mainloop_notifier, &tmp, 1))
		;
	pthread_mutex_unlock(&notifier_lock);
}

void mainloop_init( void )
{
	mainloop_notifier = threadcom_create(_wakeup_event, &mainloop_events);
}

void mainloop_deinit( void )
{
	threadcom_destroy(mainloop_notifier);
}

void mainloop_wakeup( void )
{
	pthread_mutex_lock(&notifier_lock);
	threadcom_post_event(mainloop_notifier, "W", 1);
	pthread_mutex_unlock(&notifier_lock);
}

void mainloop_flush_event( void )
{
	char tmp;

	pthread_mutex_lock(&notifier_lock);
	threadcom_get_event(mainloop_notifier, &tmp, 1);
	pthread_mutex_unlock(&notifier_lock);
}

PROFILE_DECLARE(ml);

static volatile int mainloop_level = 0;

void mainloop_exit( void ) 
{
	mainloop_level --;
	mainloop_events.abort = 1;
	mainloop_wakeup();
}

void mainloop_enter( void ) 
{
	// ******************************************************
	// Main loop
	// ******************************************************

	int now = ++mainloop_level;

//serprintf("mainloop_enter: level %d\r\n", mainloop_level );
	while (now == mainloop_level) {
		int next_timer_msec;
		
		ULONG tm = atime();

		// how long may we sleep?
#ifdef CONFIG_GUI
		if( MESSAGE_GetNum() ) {
			next_timer_msec = 0;
		} else
#endif
		{
			next_timer_msec = Timers_nextTimeout( &gui_timers ) - tm;
		}
		if( next_timer_msec < 0 ){
			// No timers, we sleep indefinitely,
			// avos will be only woken up by a data_event.
			service_data_events( &mainloop_events, NULL);
		} else {
			struct timeval tv;
			tv.tv_sec = 0;
			tv.tv_usec = next_timer_msec * 1000;
			service_data_events( &mainloop_events, &tv);
		}

		PROFILE_START(ml);

#ifdef CONFIG_GUI
		int handle_messages = 20;
		while ( handle_messages-- && MESSAGE_GetNum() ) {
			message msg;
			if ( MESSAGE_Get( &msg ) ) {
				message_handler_handle( &msg );
			}
		}
#endif		
		Timers_trigger( &gui_timers ); 

		PROFILE_STOP(ml);

	}
serprintf("mainloop_exit: level %d\r\n", mainloop_level );
}

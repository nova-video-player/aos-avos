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

#ifndef _THREADCOM_H
#define _THREADCOM_H

#include "dataevent.h"
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

struct threadcom_link;
typedef struct threadcom_link threadcom_link_t;

typedef void (*threadcom_event_t)(threadcom_link_t* comlink);
 
/**
 *	threadcom_create
 *
 *	Creates a thread-to-thread communication link with
 *	callback 'event'.
 *
 *	threadcom_event_t is a typedef for
 *	void callback(threadcom_link_t*)
 *
 *	On success, a pointer to a threadcom_link_t instance
 *	is returned, which needs to be destroyed when no longer
 *	needed to avoid resource leaks. 'data_events' is a reference
 *      to the data_event structure which will service the created
 *      link
 *
 *	On error, NULL is returned.
 */

#define threadcom_create(event, data_events) __threadcom_create(event, data_events, __FUNCTION__)
extern threadcom_link_t *__threadcom_create(threadcom_event_t event, data_event_t *data_events, const char* ident);

/**
 *	threadcom_destroy
 *
 *	Destroys a threadcom_link_t instance and releases all
 *	allocated resources.
 */
extern int threadcom_destroy(threadcom_link_t* comlink);

/**
 *	threadcom_post_event
 *
 *	Posts an event from a thread context to the main event loop.
 *	Any data can be posted, however keep in mind that this function
 *	is only non-blocking up to length = 4096. 'data' and 'length' 
 *	may not be 0.
 *
 *	On success, 0 is returned.
 *	On error, 1 is returned.
 */
extern int threadcom_post_event(threadcom_link_t* comlink, const void *data, ssize_t length);

/**
 *	threadcom_get_event
 *
 *	Retrieves data posted with threadcom_post_event from a thread.
 *	Use this call from within the threadcom_event_t callback.
 *	You _MUST_ use threadcom_get_event after an event was posted to
 *	clear the communication pipe, even if you discard the data
 *	afterwards.
 *
 *	'data' and 'length' may not be 0, and the poster and the receiver
 *	must agree on the size of the data being exchanged.
 *
 *	On success, 0 is returned.
 *	On error, 1 is returned.
 */
extern int threadcom_get_event(threadcom_link_t* comlink, void *data, ssize_t maxlen);

/**
 *	threadcom_post_string
 *
 *	This is just a very thin wrapper around threadcom_post_event.
 *	It's convenient to post C strings between a thread and the mainloop.
 *	The string is transmitted without any conversion, including the
 *	delimiting '0' at the end of the string.
 *
 *	On sucess, 0 is returned.
 *	On error, 1 is returned.
 */
extern int threadcom_post_string(threadcom_link_t* comlink, const char* string);

/**
 *	threadcom_get_string
 *
 *	This is just a very thin wrapper around threadcom_get_event.
 *	It's convenient to post C strings between a thread and the mainloop.
 *	
 *	's' must greater or equal to the maximum string length agreed upon
 *	between sender and receiver, including the trailing '0' delimiter.
 *	'maxlen' must be set to the allocated size of 's'
 *	
 *	On sucess, 0 is returned.
 *	On error, 1 is returned.
 */
extern int threadcom_get_string(threadcom_link_t* comlink, char* string, ssize_t maxlen);

/* test */
extern void threadcom_test(void);

#ifdef __cplusplus
}
#endif

#endif

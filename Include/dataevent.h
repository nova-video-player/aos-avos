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

#ifndef _DATA_EVENT_H
#define _DATA_EVENT_H

#include <sys/select.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * data event notification
 */

typedef void (*data_event_callback_t)(void* context);

typedef struct data_event {
	struct data_event* next;
	struct data_event* prev;
	
	int fd;
	void* context;
	data_event_callback_t read_func;
	data_event_callback_t write_func;
	data_event_callback_t except_func;

	int tainted;
	int abort;
	struct data_event* head;
	char ident[32];
} data_event_t;

// called from inside application to setup event notifiers
#define register_data_event(lhead, de, context) __register_data_event(lhead, de, context, __FUNCTION__)
extern int __register_data_event(data_event_t *lhead, data_event_t *de, void* context, const char *ident);
extern int unregister_data_event(data_event_t *de);

/** convenience function wrapping prepare_data_events and handle_data_events into one. may block
    up to 'maxwait' */
struct timeval;
extern void service_data_events(data_event_t *lhead, struct timeval* maxwait);

// debugging
extern void data_events_dump(data_event_t *lhead);

/** the data_event structure used by the main loop*/
extern data_event_t mainloop_events;

#ifdef __cplusplus
}
#endif

#endif


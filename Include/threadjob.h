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

#ifndef __THREADJOB_H
#define __THREADJOB_H

#include "dataevent.h"
#include <unistd.h>

struct threadjob;
typedef struct threadjob threadjob_t;
typedef void (*threadfunc_t)(void*);

#define THREADJOB_BLOCKING	1
#define THREADJOB_MODAL		2

/**
 *	threadjob_create
 *
 *	Create a thread object for out-of-line execution
 *	of a single function. The function must have the following
 *	signature:
 *		void func(void*);
 *
 *	threadjob_create() only creates and initializes the instance
 *	but does not actually start execution of the threaded function.
 * 
 *	On success, a pointer to a threadjob_t instance
 *	is returned, which needs to be destroyed when no longer
 *	needed to avoid resource leaks.
 *
 *	On error, NULL is returned.
 */
#define threadjob_create(func, data) __threadjob_create(func, data, __FUNCTION__)
extern threadjob_t* __threadjob_create(threadfunc_t func, void* data, const char* from);

/**
 *	threadjob_destroy
 *
 *	Destroys a threadcom_link_t instance and releases all
 *	allocated resources.
 */
extern int threadjob_destroy(threadjob_t *job);

/**
 *	threadjob_run
 *
 *	Execute the threaded function out-of-line. 'flags' allows the
 *	caller to determine whether threadjob_run() be blocking or not
 *	and if Disruptive Events processing is allowed or not.
 *
 *		THREADJOB_BLOCKING: blocking execution
 *		THREADJOB_MODAL: disable Disruptive Events
 *
 *	If the user requested blocking execution a new mainloop instance
 *	is created, causing threadjob_run() to block the calling context.
 *	Event processing will continue, paint events will continue to be
 *	serviced as well as timers. When the threaded function completes,
 *	the associated threadcom event terminates the new mainloop
 *	context, causing threadjob_run() to return to the caller.
 */
extern int threadjob_run(threadjob_t* job, int flags);

/**
 *	threadjob_set_abort_handler
 *
 *	This function allows to register an abort handler to cancel execution
 *	of the threaded function. The abort handler has the same signature as
 *	the threaded function and is called with the same 'data' argument.
 *	It must cause the threaded function to terminate execution and return
 *	immediately.
 */
extern int threadjob_set_abort_handler(threadjob_t* job, threadfunc_t abort_func);

/**
 *	threadjob_abort
 *
 *	Calls the registered abort handler to terminate execution of the threaded
 *	function.
 */
extern int threadjob_abort(threadjob_t* job);

/**
 *	threadjob_set_func
 *
 *	A threadjob object can be used more than once. If you
 *	want to run a new job with a different function you can
 *	reset it with this function.
 */
extern int threadjob_set_func(threadjob_t* job, threadfunc_t func, void* data);

#endif

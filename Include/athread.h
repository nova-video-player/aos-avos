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

#ifndef _ATHREAD_H
#define _ATHREAD_H

#include <pthread.h>
#include "atime.h"
#include "global.h"

#ifdef DEBUG_THREADS
// struct to help us identify a thread
typedef struct athread_info
{
	pthread_t	id;
	pid_t		pid;
	const char	* name;
} athread_info;

typedef struct athread_info_list
{
	athread_info	* info;
	size_t	count;
	size_t	size;
} athread_info_list;
void apthread_print_list(void);
void add_self_to_list_of_threads(char * name);
void del_self_from_list_of_threads(void);
int  apthread_create(pthread_t * thread, pthread_attr_t * attr, void * (*start_routine)(void *), void * arg, const char * name);
int  apthread_join(pthread_t th, void **thread_return);
int  apthread_cancel(pthread_t th);
#else	// NO DEBUG_THREADS
// define out functions
#define apthread_print_list() do {} while(0)
#define add_self_to_list_of_threads(name) do {} while(0)
#define del_self_from_list_of_threads() do {} while(0)
#define apthread_create(thread, attr, start_routine, arg, name) pthread_create(thread, attr, start_routine, arg)
#define apthread_join(th, thread_return) pthread_join(th,thread_return)
#define apthread_cancel(th) pthread_cancel(th)
#endif	// DEBUG_THREADS

int  thread_create(pthread_t *handle, void * (*thread_function)(void *), void *arg, int priority, char * name);

//
// thread state stuff
//
typedef enum {
	THREAD_EXIT = 0,
	THREAD_IDLE,
	THREAD_RUNNING,
} THREAD_STATES;

typedef struct THREAD_STATE {
	pthread_mutex_t	_mutex;
	pthread_cond_t	_cond;
	volatile int	_set;
	volatile int	_get;
	
	const char      *tag;
} THREAD_STATE;

static inline void thread_state_init( THREAD_STATE *state, int init, const char *tag )
{
	pthread_mutex_init( &state->_mutex, NULL );
	pthread_cond_init(&state->_cond, NULL);
	state->_set = init;
	state->_get = init;
	state->tag  = tag ? tag : "---";
}

int thread_state_set( THREAD_STATE *state, int new );
void thread_state_ack( THREAD_STATE *state );
void thread_state_ask_pause( THREAD_STATE *state );

static inline int thread_state_get( THREAD_STATE *state )
{
	return state->_get;
}

static inline int thread_state_asked( THREAD_STATE *state )
{
	return state->_set;
}


#endif

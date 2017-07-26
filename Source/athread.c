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
#include "athread.h"
#include "astdlib.h"
#include "debug.h"

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>

#define DBG  if(Debug[DBG_THREADS])
#define DBG2 if(Debug[DBG_THREADS]>1)
 
#ifdef DEBUG_THREADS
static athread_info_list list_of_threads = {NULL, 0, 0};
static pthread_mutex_t list_of_threads_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif	// DEBUG_THREADS


// *****************************************************************************
//
//	thread_create
//
//	create a thread, if "prio" != 0 mark it as SCHED_FIFO with the given "prio"rity
//
// *****************************************************************************
int thread_create(pthread_t *handle, void * (*thread_function)(void *), void *arg, int priority, char * name)
{
	pthread_attr_t thread_attr;
	pthread_attr_init(&thread_attr);

	if (priority != 0 && geteuid() == 0) {
DBG serprintf("create RT thread %s with prio %d\r\n", name, priority );
		pthread_attr_setschedpolicy(&thread_attr, SCHED_FIFO);
				
		struct sched_param sched_param;
		pthread_attr_getschedparam(&thread_attr, &sched_param);
		sched_param.sched_priority = priority;
		pthread_attr_setschedparam(&thread_attr, &sched_param);
	}
		
	return apthread_create( handle, &thread_attr, thread_function, arg, name );
}

#ifdef DEBUG_THREADS
#define LIST_OF_THREADS_INIT_SIZE	10
static void add_list_of_threads(pthread_t thread, const char * name)
{
	pthread_mutex_lock(&list_of_threads_mutex);
	if( !list_of_threads.size ){
		// empty list
		if( (list_of_threads.info = (athread_info*)acalloc(LIST_OF_THREADS_INIT_SIZE, sizeof(athread_info))) == NULL){
serprintf("calloc: initial list_of_threads for thread %d\n", thread);
			pthread_mutex_unlock(&list_of_threads_mutex);
			return;
		}
		list_of_threads.count=0;
		list_of_threads.size=LIST_OF_THREADS_INIT_SIZE;
	} else if( list_of_threads.size <= list_of_threads.count ){
		// realloc
		size_t new_size = list_of_threads.size << 1;
		athread_info	* new_info = (athread_info*)acalloc(new_size, sizeof(athread_info));
		if( new_info == NULL){
serprintf("calloc: resize list_of_threads for thread %d\n", thread);
			pthread_mutex_unlock(&list_of_threads_mutex);
			return;
		} else {
			memcpy(	list_of_threads.info, new_info, list_of_threads.size * sizeof(athread_info));
			afree(list_of_threads.info);
			list_of_threads.info = new_info;
			list_of_threads.size = new_size;
		}
	}

	{
		// perform the insert : look for an empty cell
		int i = 0;
		while( i < list_of_threads.size && list_of_threads.info[i].id != 0 ){
			++i;
		}
	
		list_of_threads.info[i].id = thread;
		list_of_threads.info[i].name = name;
		++list_of_threads.count;
	}
	pthread_mutex_unlock(&list_of_threads_mutex);
}

// *****************************************************************************
//
//	add_list_of_threads
//
//	add self to listof threads. used for the main thread (which is not created with a pthread_create)
//
// *****************************************************************************

void add_self_to_list_of_threads(char * name)
{
	add_list_of_threads(pthread_self(), name);
}

static void del_list_of_threads(pthread_t thread)
{
	int i = 0;

	pthread_mutex_lock(&list_of_threads_mutex);

	while( i < list_of_threads.size && list_of_threads.info[i].id != thread ){
		++i;
	}
	if( i < list_of_threads.size ){
		list_of_threads.info[i].id = 0;
		--list_of_threads.count;
	} else {
		// unknown thread, could not delete it.
serprintf("delete: list_of_threads for thread %d\n", thread);
	}
	pthread_mutex_unlock(&list_of_threads_mutex);
}

void del_self_from_list_of_threads(void)
{
	del_list_of_threads(pthread_self());
}

// *****************************************************************************
//
//	apthread_create
//
//	simple wrapper for pthread_create() that logs thread creations into list_of_threads
//
//	parameters are those for pthread_create(), with name used as a human-readble identification
//	for the newly created thread into list_of_threads
// *****************************************************************************
int apthread_create(pthread_t * thread, pthread_attr_t * attr, void * (*start_routine)(void *), void * arg, const char * name)
{
	int res = pthread_create(thread, attr, start_routine, arg);
	if( res == 0 ){
		add_list_of_threads(*thread, name);
	}
	return res;
}

// *****************************************************************************
//
//	apthread_cancel
//
//	simple wrapper for apthread_cancel
//
// *****************************************************************************
int  apthread_cancel(pthread_t th)
{
	int res = pthread_cancel(th);
	if( res != 0 ){
serprintf("apthread_cancel: %s\n", strerror(errno));
	}
	return res;
}

// *****************************************************************************
//
//	apthread_join
//
//	simple wrapper for apthread_join that logs thread join into list_of_threads
//
// *****************************************************************************
int apthread_join(pthread_t th, void **thread_return)
{
	int res = pthread_join(th, thread_return);
	if( res == 0 ){
		del_list_of_threads( th );
	} else {
serprintf("apthread_join: %s\n", strerror(errno));
	}
	return res;
}

// *****************************************************************************
//
//	apthread_print_list
//
//	serprintf threads referenced in list_of_threads
//
// *****************************************************************************
void apthread_print_list(void)
{
	int i;
	pthread_mutex_lock(&list_of_threads_mutex);
serprintf("List of registered threads:\n");

	for(i = 0; i < list_of_threads.size; ++i){
		if( list_of_threads.info[i].id ){
serprintf("\t%d : %s\n", list_of_threads.info[i].id, list_of_threads.info[i].name);
		}
	}
	pthread_mutex_unlock(&list_of_threads_mutex);
}

DECLARE_DEBUG_COMMAND_VOID("threads", apthread_print_list);

#endif	// DEBUG_THREADS

// *****************************************************************************
//
//	thread_state_set
//
// *****************************************************************************
int thread_state_set( THREAD_STATE *state, int new )
{
	int old;
	pthread_mutex_lock( &state->_mutex );
	old = state->_get;
	state->_set = new;

	if ( state->_set != THREAD_IDLE) {
		pthread_cond_broadcast(&state->_cond);
	}

DBG2 serprintf("SET %s  %08X  %d -> %d  start\r\n", state->tag, state, old, new );
	while( state->_get != state->_set ) {
		pthread_cond_wait(&state->_cond, &state->_mutex);
	}
DBG2 serprintf("SET %s  %08X  %d -> %d  end\r\n", state->tag, state, old, new );
	pthread_mutex_unlock( &state->_mutex );
	return old;
}

// *****************************************************************************
//
//	thread_state_ack
//
// *****************************************************************************
void thread_state_ack( THREAD_STATE *state )
{
DBG2 serprintf("ack %s  %08X  s %d  lock\r\n", state->tag, state, state->_set );
	pthread_mutex_lock(&state->_mutex);
	state->_get = state->_set;
	pthread_cond_broadcast(&state->_cond);
DBG2 serprintf("ack %s  %08X  g %d  start\r\n", state->tag, state, state->_get );
	while (state->_set == THREAD_IDLE) {
		pthread_cond_wait(&state->_cond, &state->_mutex);
	}
DBG2 serprintf("ack %s  %08X  g %d  end\r\n", state->tag, state, state->_get );
	pthread_mutex_unlock(&state->_mutex);
}

void thread_state_ask_pause( THREAD_STATE *state )
{
	pthread_mutex_lock(&state->_mutex);
	state->_set = THREAD_IDLE;
DBG2 serprintf("idl %s  %08X\r\n", state->tag );
	pthread_mutex_unlock(&state->_mutex);
}

#ifdef DEBUG_MSG
static volatile int 	test_thread_kill;
static pthread_t	test_thread_handle;

static void* test_thread(void *param)
{
	int verbose = 0;
	if( param )
		verbose = *(int*)param;
	int last_time = atime();
	
serprintf("test_thread started, verbose %d\n", verbose);
	do {
		msec_sleep( 10 );
		int now_time = atime();
		if ( now_time - last_time > 50 ) {
			serprintf(" **%d** ", now_time - last_time);
		} else if ( verbose ) {
			serprintf(" *%d* ", now_time - last_time);
		}
		last_time = now_time;
	} while ( !test_thread_kill );
serprintf("test_thread stopped\n");
	return NULL;
}

static void start_test_thread( int argc, char *argv[] )
{
	test_thread_kill = 0;
	int verbose = 0;
	if( argc > 1 ) {
		verbose = 1;
	}
	thread_create( &test_thread_handle, test_thread, &verbose, 100, "test_thread" );
}

static void stop_test_thread( void )
{
	test_thread_kill = 1;
	msec_sleep(2000);
	apthread_join(test_thread_handle, NULL);
}

DECLARE_DEBUG_COMMAND( "starttt", start_test_thread );
DECLARE_DEBUG_COMMAND_VOID( "stoptt", stop_test_thread );
#endif

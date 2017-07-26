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

#include "timers.h"
#include "atime.h"
#include "util.h"
#include "debug.h"
#include "object.h"
#include "mainloop.h"

#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <stdint.h>


/**
 @struct Timers
 @brief A list of timers where each entry has a callback that is 
 called either once or repeatedly until the entry gets removed.

 In most cases, you'll want to use the global gui_timers object 
 to have your callbacks executed in the main loop of AVOS. 
 However, it is also possible to create a custom Timers object 
 to use in different threads.
*/

/* Note: Elements are stored as a sorted linked list and data is used as the object pool.
   An id of -1 denotes an unused element. The linked list is formed as a ring where dummy 
   is the first and last element. Since dummy.timeout == INT32_MAX, it can serve as a sentinel.
*/

/**
 @typedef safe_timer_cb
 @brief Signature of the onTimeout callback.
*/

/**
 @enum TimerMode
 @brief TIMER_SINGLE or TIMER_REPEATED
*/

void asignal( int sig );

static BOOL inited = FALSE;

// Initializes the given Timers struct. 
void Timers_init( Timers *obj )
{
	obj->dummy.id = -1;
	obj->dummy.next = &obj->dummy;
	obj->dummy.timeout = INT32_MAX;

	int i;
	for( i = 0; i < obj->size; i++ ) {
		obj->data[i].id = -1;
	}
	if( obj == &gui_timers ) {
		inited = TRUE;
	}
}

// helper function to insert a timer into the linked list, keeping it sorted
static void _insert( Timers *obj, Timer *nt )
{
	Timer *before = &obj->dummy;
	while( before->next->timeout < nt->timeout ) {
		before = before->next;
	}
	nt->next = before->next;
	before->next= nt;
	obj->cnt++;
}

// helper function to remove a timer from the linked list
static void _remove( Timers *obj, Timer *before )
{
	before->next = before->next->next;
	obj->cnt--;
}

// inits and adds a timer element, does housekeeping
static int Timers_internalAdd( Timers *obj, void (*onTimeout_nolistener)(), void (*onTimeout)(void*), 
			void *listener, int interval, int delay, TimerMode mode, const char* caller )
{
	static int id_counter = 1;	// ID counter should start at 1 to prevent wrong remove
					// of timer with ID 0 when ID is not initialize to -1

	pthread_mutex_lock( &obj->mutex );

	int res= -1;
	if( obj->cnt < obj->size ) {
		ULONG tm = atime();

		// find an unused element
		Timer *nt = NULL;
		int i;
		for( i = 0; i < obj->size; i++ ) {
			if( obj->data[i].id == -1 ) {  // an id of -1 marks an unused element
				nt = obj->data + i;
				break;
			}
		}

		// fill out the timer struct
		nt->id		    = id_counter++;
		nt->timeout	    = tm + delay;
		nt->interval	    = mode == TIMER_SINGLE ? -1 : interval;
		nt->callback	    = onTimeout_nolistener;
		nt->callback_ctx    = onTimeout;
		nt->ctx		    = listener;
		strnZcpy( nt->caller, caller, sizeof( nt->caller ) - 1 );
		
		// insert the element
		_insert( obj, nt );

		res = nt->id;
	} else {
		serprintf("\nCannot insert Timer :-(\n");
#ifdef SIM
		Trace();
		serprintf("\n\n");
#endif
	} 

	pthread_mutex_unlock( &obj->mutex );
	mainloop_wakeup();

	return res;
}

/**
 @brief Adds a timer entry to the list of timers. Deprecated, use Timers_safeAdd instead.

 @warning If you pass a listener object, always make sure to remove the timer 
 in the destructor of the object, even if you're using TIMER_SINGLE.
 @warning Overwriting an already used timer id and thereby leaking the old timer is a common problem. 
 Timers_safeAdd should be used instead as it checks against the timer id being in use.
 @param onTimeout The callback to be called on a timeout
 @param interval The interval until the next timeout in msecs
 @param mode If mode is TIMER_SINGLE, the entry gets removed after the first timeout
 @returns A timer id that can be used to remove the entry 
*/
int __Timers_Add( Timers *obj, void (*onTimeout) (), int interval, TimerMode mode, const char* caller )
{
	assert( interval > 0 );

	return Timers_internalAdd( obj, onTimeout, NULL, NULL, interval, interval, mode, caller );
}

/**
 @brief Adds a timer entry to the list of timers. Deprecated, use Timers_safeDelayedAdd instead.

 Like Timers_Add called in TIMER_REPEATED mode. But the first timeout occurs after the given 
 delay instead of the normal timeout.
 @warning If you pass a listener object, always make sure to remove the timer 
 in the destructor of the object.
 @warning Overwriting an already used timer id and thereby leaking the old timer is a common problem. 
 Timers_safeDelayedAdd should be used instead as it checks against the timer id being in use.
 @param onTimeout The callback to be called on a timeout
 @param interval The interval between timeouts in msecs
 @param delay The delay before the first timeout in msecs
 @returns A timer id that can be used to remove the entry 
*/
int __Timers_DelayedAdd( Timers *obj, void (*onTimeout) (), int interval, int delay, const char* caller )
{
	assert( interval > 0 );

	return Timers_internalAdd( obj, onTimeout, NULL, NULL, interval, delay, TIMER_REPEATED, caller );
}

/**
 @brief Adds a timer entry to the list of timers. Deprecated, use Timers_safeAdd instead.

 Like Timers_Add, but accepts an additional listener object that gets passed to the callback.
 @warning If you pass a listener object, always make sure to remove the timer 
 in the destructor of the object, even if you're using TIMER_SINGLE.
 @warning Overwriting an already used timer id and thereby leaking the old timer is a common problem. 
 Timers_safeAdd should be used instead as it checks against the timer id being in use.
 @param onTimeout The callback to be called on a timeout
 @param listener The listener object to be passed to the callback
 @param interval The interval until the next timeout in msecs
 @param mode If mode is TIMER_SINGLE, the entry gets removed after the first timeout
 @returns A timer id that can be used to remove the entry 
*/
int __Timers_AddWithParam( Timers *obj, timer_cb onTimeout, void *listener, int interval, TimerMode mode, const char *caller )
{
	assert( interval > 0 );

	return Timers_internalAdd( obj, NULL, onTimeout, listener, interval, interval, mode, caller );
}

/**
 @brief Adds a timer entry to the list of timers. Deprecated, use Timers_safeAdd instead.

 Like Timers_DelayedAdd, but accepts an additional listener object that gets passed to the callback.
 @warning If you pass a listener object, always make sure to remove the timer 
 in the destructor of the object.
 @warning Overwriting an already used timer id and thereby leaking the old timer is a common problem. 
 Timers_safeDelayedAdd should be used instead as it checks against the timer id being in use.
 @param onTimeout The callback to be called on a timeout
 @param listener The listener object to be passed to the callback
 @param interval The interval between timeouts in msecs
 @param delay The delay before the first timeout in msecs
 @returns A timer id that can be used to remove the entry 
*/
int __Timers_DelayedAddWithParam( Timers *obj, timer_cb onTimeout, void *listener, int interval, int delay, const char* caller )
{
	assert( interval > 0 );

	return Timers_internalAdd( obj, NULL, onTimeout, listener, interval, delay, TIMER_REPEATED, caller );
}

/**
 @brief Adds a timer entry to the list of timers.

 The callback will be called once (TIMER_SINGLE) or repeatedly (TIMER_REPEATED) until the timer gets removed. 
 The callback will be executed in the thread that calls Timers_trigger (the mainloop for gui_timers). 
 If the timer id is already in use, the old timer will be removed.
 It is safe to add or remove timers from a different thread.
 It is safe to add or remove timers in the onTimeout callback.
 @warning Make sure to initialize the timer id variable to -1 before passing it for the first time.
 @warning If you pass a listener object, always make sure to remove the timer 
 in the destructor of the object, even if you're using TIMER_SINGLE.
 @param id The timer id will be written to this parameter. It can be used to remove the entry.
 @param onTimeout The callback to be called on a timeout
 @param listener The listener object to be passed to the callback
 @param interval The interval between timeouts in msecs
 @param mode If mode is TIMER_SINGLE, the entry gets removed after the first timeout
*/
void __Timers_safeAdd(Timers *obj, int *id, safe_timer_cb onTimeout, Object *listener, int interval, TimerMode mode, const char* caller )
{
	assert( interval > 0 && id != NULL );
	if( *id != -1 && *id != 0 ) {
		//serprintf( "Warning: overwriting an active or uninitialized timer. id %d  caller %s\n", *id, caller );
		Timers_remove( obj, id );
	}
	*id = Timers_internalAdd( obj, NULL, (timer_cb) onTimeout, listener, interval, interval, mode, caller );
}

/**
 @brief Adds a timer entry in TIMER_REPEATED mode to the list of timers after an initial delay.

 Like Timers_safeAdd called in TIMER_REPEATED mode. But the first timeout occurs after the given 
 delay instead of after the given interval.
 @warning Make sure to initialize the timer id variable to -1 before passing it for the first time.
 @warning If you pass a listener object, always make sure to remove the timer 
 in the destructor of the object.
 @param id The timer id will be written to this parameter. It can be used to remove the entry.
 @param onTimeout The callback to be called on a timeout
 @param listener The listener object to be passed to the callback
 @param interval The interval between timeouts in msecs
 @param delay The delay before the first timeout in msecs
*/
void __Timers_safeDelayedAdd( Timers *obj, int *id, safe_timer_cb onTimeout, Object *listener, int interval, int delay, const char* caller )
{
	assert( interval > 0 && id != NULL );
	if( *id != -1 ) {
		serprintf( "Warning: You're trying to overwrite an active or uninitialized timer. Let's try to remove it first.\n" );
		Timers_remove( obj, id );
	}
	*id = Timers_internalAdd( obj, NULL, (timer_cb) onTimeout, listener, interval, delay, TIMER_REPEATED, caller );
}

/**
 @brief Removes an entry from the list of Timers

 The id gets reset to -1.
 It is safe to call Timers_remove on a timer id of -1.
 @param id Pointer to the timer id of the entry as provided by Timers_Add
*/
void Timers_remove(Timers *obj, int *id )
{
	if( !id || *id == -1 ) {
		return;
	}
	if ( *id == 0 ) {
		serprintf( "Timer with ID 0 isn't in use, don't try to remove it !!!\r\n" );
		return;
	}

	pthread_mutex_lock( &obj->mutex );

	Timer *before = &obj->dummy;
	while( before->next->timeout < INT32_MAX ) {
		if( before->next->id == *id ) {
			before->next->id = -1;
			_remove( obj, before );
			break;
		}
		before = before->next;
	}

	*id = -1;
	pthread_mutex_unlock( &obj->mutex );
}

/**
 @brief Deprecated, use Timers_remove instead.
*/
void Timers_Remove( Timers *obj, int *id )
{
	Timers_remove( obj, id );
}

// Triggers any pending timers. Gets called by the main loop for gui_timers.
void Timers_trigger( Timers *obj )
{
	ULONG tm = atime();

	Timer *before = &obj->dummy;

	pthread_mutex_lock( &obj->mutex );

	while( before->next->timeout <= tm ) {
		Timer *first = before->next;

		if( first->interval > 0 ) {
			_remove( obj, before );
			first->timeout = tm + first->interval;
			_insert( obj, first );
		} else {
			before->next->id = -1;
			_remove( obj, before );
		}

		pthread_mutex_unlock( &obj->mutex );
		if( first->callback )
			first->callback();
		else if( first->callback_ctx )
			first->callback_ctx( first->ctx );
		pthread_mutex_lock( &obj->mutex );
	}

	pthread_mutex_unlock( &obj->mutex );
}

// returns the time the next timeout will occur.
int Timers_nextTimeout( Timers *obj ) {
	pthread_mutex_lock( &obj->mutex );

	int res = obj->cnt != 0 ? obj->dummy.next->timeout : 0;

	pthread_mutex_unlock( &obj->mutex );

	return res;
}

// returns whether the given object is registered as a listener for any timer.
int Timers_haveListener(Timers *obj, void *listener)
{
	if( !inited ) return 0;

	Timer *before = &obj->dummy;
	while( before->next->timeout < INT32_MAX ) {
		if( before->next->ctx == listener ) {
			serprintf( "Timers_haveListener: listener exists, was added by %s\n", before->next->caller );
			return 1;
		}
		before = before->next;
	}
	return 0;
}

#ifdef DEBUG_MSG
// dumps the timers structure for debugging purposes.
static void Timers_dump( Timers *obj ) 
{
	serprintf("Timers (%i):\n", obj->cnt);

	Timer *cur = obj->dummy.next;
	while( cur->timeout < INT32_MAX ) {
		serprintf("  id: %5i  int %8d  next_to %8d  cb %08X  ctx %08X  [%s]\n", cur->id, cur->interval, cur->timeout,  cur->callback?cur->callback:cur->callback_ctx, cur->ctx, cur->caller);
		cur = cur->next;
	}
	serprintf("\n");
}

static void timers_dump( void )
{
	Timers_dump( &gui_timers );
}
DECLARE_DEBUG_COMMAND_VOID("tdu", timers_dump );
#endif

/*
// test code
#include <unistd.h>
static void dummy() {
}


void Timers_Test( Timers *t ) {
	Timers_Add( t, dummy, 10, TIMER_REPEATED );
	Timers_Add( &gui_timers, dummy, 30, TIMER_SINGLE );
	Timers_Add( &gui_timers, dummy, 50, TIMER_REPEATED );

	int i;
	for( i = 0; i < 10; i++) {
		Timers_Dump( &gui_timers );
		Timers_trigger( &gui_timers );
		usleep(10* 1000);
	}
}
*/

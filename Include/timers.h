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

/**
 @file
 @brief Defines the Timers class and the global gui_timers variable.
*/

#ifndef TIMERS_H
#define TIMERS_H

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A list of timers where each entry has a callback that is 
   called either once or repeatedly until the entry gets removed.
*/

typedef enum {
	TIMER_REPEATED, 
	TIMER_SINGLE
} TimerMode;

struct Timers_str;
typedef struct Timers_str Timers;

// forward declaration for Object. we don't want to pull in
// all the AVOS-core includes when using the timers.h API in flashlite
struct Object_str;

extern Timers gui_timers;

typedef void (*safe_timer_cb)(struct Object_str *listener);

#define av_str_1(x)	#x
#define av_str(x)	av_str_1(x)

#define Timers_safeAdd(obj, id, onTimeout, listener, interval, mode) \
	__Timers_safeAdd(obj, id, onTimeout, listener, interval, mode, __FILE__ ":" av_str(__LINE__) ":" #onTimeout)	
extern void __Timers_safeAdd(Timers *obj, int *id, safe_timer_cb onTimeout, struct Object_str *listener, int interval, TimerMode mode, const char* caller);

#define Timers_safeDelayedAdd(obj, id, onTimeout, listener, interval, delay) \
	__Timers_safeDelayedAdd(obj, id, onTimeout, listener, interval, delay, __FILE__ ":" av_str(__LINE__) ":" #onTimeout )
extern void __Timers_safeDelayedAdd(Timers *obj, int *id, safe_timer_cb onTimeout, struct Object_str *listener, int interval, int delay, const char* caller);

extern void Timers_remove(Timers *obj, int *id );

// deprecated
typedef void (*timer_cb)(void *listener);

#define Timers_Add(obj, onTimeout, interval, mode) \
	__Timers_Add(obj, onTimeout, interval, mode, __FILE__ ":" av_str(__LINE__) ":" #onTimeout )
extern int  __Timers_Add(Timers *obj, void (*onTimeout)(), int interval, TimerMode mode, const char* caller);

#define Timers_DelayedAdd(obj, onTimeout, interval, delay) \
	__Timers_DelayedAdd(obj, onTimeout, interval, delay, __FILE__ ":" av_str(__LINE__) ":" #onTimeout )
extern int  __Timers_DelayedAdd(Timers *obj, void (*onTimeout)(), int interval, int delay, const char* caller);

#define Timers_AddWithParam(obj, onTimeout, listener, interval, mode) \
	__Timers_AddWithParam(obj, onTimeout, listener, interval, mode, __FILE__ ":" av_str(__LINE__) ":" #onTimeout )
extern int  __Timers_AddWithParam(Timers *obj, timer_cb onTimeout, void *listener, int interval, TimerMode mode, const char* caller);

#define Timers_DelayedAddWithParam(obj, onTimeout, listener, interval, delay) \
	__Timers_DelayedAddWithParam(obj, onTimeout, listener, interval, delay, __FILE__ ":" av_str(__LINE__) ":" #onTimeout )
extern int  __Timers_DelayedAddWithParam(Timers *obj, timer_cb onTimeout, void *listener, int interval, int delay, const char* caller);

extern void Timers_Remove(Timers *obj, int *id);


enum timer_mode {
	Repeated,
	Single
};

// these are normally only used inside the main loop
void Timers_init(       Timers *obj);
void Timers_trigger(    Timers *obj);
int  Timers_nextTimeout(Timers *obj);

int Timers_haveListener(Timers *obj, void *listener);

// internal
typedef struct Timer_str {
	int id;
	int timeout;
	int interval;
	// for standard callbacks
	void (*callback) ();
	// for callbacks with a context
	void *ctx;
	void (*callback_ctx)(void *ctx);
	char caller[128 + 1];
	struct Timer_str *next;
} Timer;

struct Timers_str {
	pthread_mutex_t mutex;
	Timer *data;
	Timer dummy;
	int size, cnt;
};

#ifdef __cplusplus
}
#endif

#endif // TIMERS_H

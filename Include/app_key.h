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

#ifndef _APP_KEY_H
#define _APP_KEY_H

#include "global.h"
#include "types.h"

#define KEY_NO_KEY		-1

enum {
	//......................
	// Jukebox keys
	//......................
	// Short press
	KEY_OK = 0,
	KEY_STOP,
	KEY_LEFT,
	KEY_RIGHT,
	KEY_UP,
	KEY_DOWN,
	KEY_ON,
	KEY_MENU,
	KEY_TAB,
	KEY_TVLCD,
	KEY_PGUP,
	KEY_PGDOWN,
	KEY_POWER,

	// Long press
	KEY_OK_L,
	KEY_STOP_L,
	KEY_LEFT_L,
	KEY_RIGHT_L,
	KEY_UP_L,
	KEY_DOWN_L,
	KEY_ON_L,
	KEY_MENU_L,
	KEY_TAB_L,
	KEY_TVLCD_L,
	KEY_PGUP_L,
	KEY_PGDOWN_L,
	KEY_POWER_L,

	KEY_VOLUP,
	KEY_VOLDOWN,

	//......................
	// IR remote keys
	//......................
	KEY_1,
	KEY_2,
	KEY_3,
	KEY_4,
	KEY_5,
	KEY_6,
	KEY_7,
	KEY_8,
	KEY_9,
	KEY_0,
	KEY_RECORD,
	KEY_MUTE,
	KEY_HOME,

	KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, 
	KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
	KEY_K, KEY_L, KEY_M, KEY_N, KEY_O,
	KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
	KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y,
	KEY_Z,
	KEY_COMMA,
	KEY_DOT,
	KEY_SPACE,

	KEY_RETURN,
	KEY_ALT,
	KEY_ALTGR,
	KEY_LEFTSHIFT,
	KEY_RIGHTSHIFT,
	KEY_CAPSLOCK,
	KEY_BACKSPACE,
	KEY_TEXT_TAB,

	KEY_MOUSE_UP,
	KEY_MOUSE_DOWN,
	KEY_MOUSE_LEFT,
	KEY_MOUSE_RIGHT,

	KEY_MOUSE_UP_LEFT,
	KEY_MOUSE_UP_RIGHT,
	KEY_MOUSE_DOWN_LEFT,
	KEY_MOUSE_DOWN_RIGHT,

	KEY_MOUSE_UP_GRAB,
	KEY_MOUSE_DOWN_GRAB,
	KEY_MOUSE_LEFT_GRAB,
	KEY_MOUSE_RIGHT_GRAB,

	KEY_MOUSE_UP_LEFT_GRAB,
	KEY_MOUSE_UP_RIGHT_GRAB,
	KEY_MOUSE_DOWN_LEFT_GRAB,
	KEY_MOUSE_DOWN_RIGHT_GRAB,

	KEY_MOUSE_CLICK_LEFT,
	KEY_MOUSE_CLICK_RIGHT,
	KEY_MOUSE_GRAB,

	KEY_FAST_REWIND,
	KEY_FAST_FORWARD,
	KEY_FAST_REWIND_L,
	KEY_FAST_FORWARD_L,

#ifdef CONFIG_REMOTE_FM
	//......................
	// FM remote keys
	//......................
	KEY_REMOTE_MP3,
	KEY_REMOTE_FM,
	KEY_REMOTE_PLAYPAUSE,
	KEY_REMOTE_HOLD,
	KEY_REMOTE_REC,
	KEY_REMOTE_LEFT,
	KEY_REMOTE_RIGHT,
	KEY_REMOTE_MP3_L,
	KEY_REMOTE_FM_L,
	KEY_REMOTE_PLAYPAUSE_L,
	KEY_REMOTE_HOLD_L,
	KEY_REMOTE_REC_L,
	KEY_REMOTE_LEFT_L,
	KEY_REMOTE_RIGHT_L,
#endif

	NUM_KEY_ACTIONS		// Must always be at the end of the list!!!
};

// Number of acceleration steps for the arrow keys
// (this is also the maximum value for key_speed() )
#define KEY_ACCEL_STEPS 	4

int  key_is_stop_key( int key );
int  key_get_async( int *id, int *key );
int  key_speed( void );
int  key_speed_from_time( int time );
int  key_get_async_stopkey( void );

typedef void (*key_cb)( void*, int );

void Key_SetReleaseHandler( void *obj, key_cb onKeyRelease );

#endif

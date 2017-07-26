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

// *****************************************************
//
// APP_STATE.H
//
// *****************************************************

#ifndef _APP_STATE_H
#define _APP_STATE_H

#include "types.h"

#include "app_key.h"
#include "message.h"

// ************************************************************
// STATE MACHINE
// ************************************************************

// States of the application state machine

typedef struct KEYB_STATE
{
	const char *name;
	fp	action[NUM_KEY_ACTIONS];// what to call when key pressed
	fp	menubar_handler;	// function to call to build the menu bar
	fp	redrawOSD; 		// function to call to redraw screen (OSD only)
	fp	state_deactivate;	// function to call to cleanup state so an arbitrary one can be entered cleanly
	fp	destroy;		// function to call to exit all apps and free all resources used by current app
	void	(*key_released)( int key );
	fp	on_resize_event;
} KEYB_STATE;

extern const KEYB_STATE* KEYB_INVALID;
extern const KEYB_STATE* KEYB_INFO;
extern const KEYB_STATE* KEYB_ERROR;
extern const KEYB_STATE* KEYB_STANDBY;
extern const KEYB_STATE* KEYB_WARNINFO;

extern const KEYB_STATE *STATE_callbacks;

void set_keyb_state( const KEYB_STATE *new_state );
const KEYB_STATE *get_keyb_state( void );
#define KEYB_state get_keyb_state()

void AC_SetKeybState( const KEYB_STATE *state );

void init_global_message_handlers( void );

void TVLCD_short( void );
void TVLCD_long( void );
BOOL AC_HandleGlobalKey( int key );

#endif	// _APP_STATE_H

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

#ifndef _PLATFORM_H_
#define _PLATFORM_H_

#define __stringify_1(x)        #x
#define __stringify(x)          __stringify_1(x)

//
// FRAME BUFFER
//
#include "fb.h"

typedef int (*FB_CONNECT) ( FRAME_BUFFER *fb );

typedef struct PLATFORM_REG_FB {
	const char		*name;
	const FB_CONNECT	connect;
} PLATFORM_REG_FB;

void platform_register_fb( PLATFORM_REG_FB *reg );

#define PLATFORM_REGISTER_FB( connect ) \
		static PLATFORM_REG_FB _reg_fb##connect = { \
			__stringify(connect), \
			&connect,\
		}; \
		static void _fn_reg_fb##connect( void ) __attribute__((constructor));\
		static void _fn_reg_fb##connect( void )\
		{ \
			platform_register_fb( &_reg_fb##connect ); \
		}

FB_CONNECT platform_get_fb_connect( void );

//
// KEYBOARD
//
#include "input.h"

typedef int (*KEYB_DEV_OPEN) ( struct input_device *dev );

typedef struct PLATFORM_REG_KEYB {
	const char		*name;
	const KEYB_DEV_OPEN	open;
} PLATFORM_REG_KEYB;

void platform_register_keyboard( PLATFORM_REG_KEYB *reg );

#define PLATFORM_REGISTER_KEYBOARD( open ) \
		static PLATFORM_REG_KEYB _reg_keyb##open = { \
			__stringify(open), \
			&open,\
		}; \
		static void _fn_reg_keyb##open( void ) __attribute__((constructor));\
		static void _fn_reg_keyb##open( void )\
		{ \
			platform_register_keyboard( &_reg_keyb##open ); \
		}

KEYB_DEV_OPEN platform_get_keyboard_open( void );

#endif

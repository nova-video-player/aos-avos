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
#include "debug.h"
#include "platform.h"

typedef struct PLATFORM {
	PLATFORM_REG_FB		*reg_fb;
	PLATFORM_REG_KEYB	*reg_keyb;
} PLATFORM;

static PLATFORM platform;

//
// FB
//
void platform_register_fb( PLATFORM_REG_FB *reg )
{
	platform.reg_fb = reg;
}

FB_CONNECT platform_get_fb_connect( void )
{
	return platform.reg_fb ? platform.reg_fb->connect : NULL;
}

//
// KEYBOARD
//
void platform_register_keyboard( PLATFORM_REG_KEYB *reg )
{
	platform.reg_keyb = reg;
}

KEYB_DEV_OPEN platform_get_keyboard_open( void )
{
	return platform.reg_keyb ? platform.reg_keyb->open : NULL;
}

#ifdef DEBUG_MSG
static void platform_dump_reg( void )
{
	serprintf("platform registry:\r\n");
	if( platform.reg_fb )
		serprintf("framebuffer: %s\r\n", platform.reg_fb->name );
	if( platform.reg_keyb )
		serprintf("keyboard:    %s\r\n", platform.reg_keyb->name );
}

DECLARE_DEBUG_COMMAND_VOID( "plreg", platform_dump_reg );
#endif

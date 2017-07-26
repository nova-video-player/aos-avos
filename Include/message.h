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

#ifndef _MESSAGE_H
#define _MESSAGE_H

#include <types.h>
#include "rect.h"

enum {
	MS_KEY_PRESSED			= 0,
	MS_TRACK_CHANGED		= 2,
	MS_NO_VALID_TRACK		= 3,
	MS_DISCARD_TRACK		= 4,
	MS_POWER_OFF			= 5,
	MS_KEY_RELEASED			= 6,
	MS_SHOW_APIC			= 7,
	MS_CLEAR_APIC			= 8,
	MS_POWER_BATTERY_LOW		= 9,
	MS_TRACK_ERROR			= 10,
	MS_STANDBY			= 11,
	MS_POWER_EMERGENCY_OFF		= 12,
	MS_HELMETCAM_CONNECTED		= 13,
	MS_HELMETCAM_DISCONNECTED	= 14,
	MS_START_HELMETCORDER		= 15,
	MS_STOP_HELMETCORDER		= 16,
	MS_HELMETCORDER_REC		= 17,
	MS_RECORDING_ERROR		= 18,
	MS_RECORDING_DISK_FULL		= 19,
	MS_RECORDING_MAXSIZE		= 20,
	MS_VIDEOCORDER_RESTART		= 21,
	MS_VIDEOCORDER_START		= 22,
	MS_VIDEOCORDER_ABORT		= 23,
	MS_MOUSE_MOVE			= 24,
	MS_MOUSE_PRESS			= 25,
	MS_MOUSE_DRAG			= 26,
	MS_MOUSE_RELEASE		= 27,
	MS_AUX_PLUG_UNPLUG		= 28,
	MS_DC_PLUG_UNPLUG		= 29,
	MS_CAMCORDER_ABORT		= 30,
	MS_HEADPHONES_PLUGGED		= 31,
	MS_HEADPHONES_UNPLUGGED		= 32,
	MS_TOGGLE_VIDEO			= 33,
	MS_RESTORE_WALLPAPER_PLANE	= 34,
	MS_ATV_ERROR			= 35,
	MS_ATV_MESSAGE			= 36,
	MS_ATV_DEBUG			= 37,
	MS_ATV_ABORT_RECORD		= 38,
	MS_EPG_LOCAL_TIME_UPDATE	= 39,
	MS_EPG_EIT_UPDATE		= 40,
	MS_DUMMY			= 41,
	MS_UPDATE			= 42,
	MS_SUSPEND_DIGITAL_OUTPUT	= 43,
	MS_RAW_KEY_PRESSED		= 45,
	MS_RAW_KEY_RELEASED		= 46,
	MS_ACTIVATE_SCREEN		= 47,
	MS_USB_UNPLUGGED		= 48,
	MS_USB_AUTOSWITCH		= 49,
};

struct MouseMessageData {
	int xpos, ypos;
	int buttons;
	BOOL from_mousepad;
};

struct RawKeyData {
	int keycode;
	int repeat;
	int source;
};

union MessageData {
	struct MouseMessageData mouse;
	struct RawKeyData key;
	rect area;
	int cache_index;
};

struct message_str {
	int id;
	int timestamp; // in msecs
	union MessageData data;
};

typedef struct message_str message;

BOOL	MESSAGE_Enqueue( message *msg );
void	MESSAGE( int num );
void	KEY_MESSAGE( int key, int state, int source );
void	RAW_KEY_MESSAGE( int key, int repeat, int state, int source );
BOOL	MESSAGE_Get( message *msg );
int	MESSAGE_GetNum( void );
void	MESSAGE_Clear( void );
void	MESSAGE_ClearKeys( void );
rect    MESSAGE_PurgeRedraws( const rect *area_to_purge );
void 	MESSAGE_DiscardTaps( void );
int 	MESSAGE_Inspect( int(*inspector)( message *msg ) );

typedef void (*mha)( message *msg );
typedef void (*mhx)( message *msg, void *ctx );

void message_handler_init( void );
int message_handler_add( int id, mha action, const char *tag );
int message_handler_add_with_ctx( int id, mhx action, void *ctx, const char *tag);
int message_handler_remove( int id );
int message_handler_handle( message *msg );

#endif

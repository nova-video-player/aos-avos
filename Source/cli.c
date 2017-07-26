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
#include "types.h"
#include "debug.h"
#include "util.h"
#include "fb.h"
#include "app_av.h"
#include "audio_interface.h"

void cli_play_video( const char *path, int etype );

static void fb_init( void )
{
	if(FB_connect( &fb )){
serprintf("_______Cannot connect to OSD_____\n");
	}
serprintf("fb_init: %d x %d\n", fb.osd_props.x_l, fb.osd_props.y_l);

	FB_set_osd_active(0);
	FB_set_video_active(0);
}

void cli_start(int argc, char *argv[])
{
	if( argc > 1 ) {
serprintf("\narguments: ");	
		int i;
		for( i = 1; i < argc; i++ ) {
serprintf("[%s] ", argv[i] );
		}
serprintf("\n");	
	}

	fb_init();
	audio_interface_init();

	if( argc > 1 ) {
		int etype = 0;
		if( argc > 2 ) {
			etype = atoi(argv[2]);
		}
		cli_play_video( argv[1], etype );
	}
}

void app_stop(void);

void cli_stop( void )
{
	AV_stop();
	app_stop();
}


DECLARE_DEBUG_COMMAND_VOID("x", cli_stop );

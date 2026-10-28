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
#include "sub_engine.h"
#include "sub_engine_registry.h"

void cli_play_video( const char *path, int etype );

// The subtitle engine is created by Java (SubtitleEngine.nativeCreate) on Android. There is no
// JVM in the CLI/sim build, so create it here, the same way fb_init()/audio_interface_init() bring up the
// other host backends, and publish it to the registry so streams can acquire() it later.
// Surfaceless: no ANativeWindow is attached, and the clock is not started yet.
static SUB_ENGINE *sub_eng = NULL;

static void sub_eng_init( void )
{
	if( sub_eng )
		return;
	sub_eng = sub_engine_create();
	if( !sub_eng ) {
serprintf("sub_eng_init: sub_engine_create FAILED\n");
		return;
	}
	sub_engine_registry_publish( sub_eng );
serprintf("sub_eng_init: sub engine %p created and published (surfaceless, clock not started)\n", sub_eng);
}

static void sub_eng_exit( void )
{
	if( !sub_eng )
		return;
	SUB_ENGINE *eng = sub_eng;
	sub_eng = NULL;
	// destroyed right here if no stream holds it, else by the last registry release
	sub_engine_registry_retract( eng, sub_engine_destroy );
serprintf("sub_eng_exit: sub engine %p retracted\n", eng);
}

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
	sub_eng_init();

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
	sub_eng_exit();
	app_stop();
}


DECLARE_DEBUG_COMMAND_VOID("x", cli_stop );

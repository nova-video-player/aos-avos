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
#include "app.h"
#include "astdlib.h"
#include "athread.h"
#include "mainloop.h"

void app_start( int argc, char *argv[] );
void gui_start( void );
void cli_start( int argc, char *argv[] );

#ifdef DEBUG_MSG
void serial_do_cmd(const unsigned char *cmd);
#endif

// *********************************************************************
//
//	main :-)
//
// *********************************************************************
int main( int argc, char *argv[] )
{
	LOG_open();

	//
	// Perform the whole bunch of hardware inits, ...
	//
	app_start( argc, argv );

#ifdef CONFIG_GUI
	gui_start();
#endif
#ifdef CONFIG_CLI
	cli_start(argc, argv);
#endif
	//
	// For testing purposes
	// Assume command line pars are serial commands
	//
#ifdef CONFIG_GUI
#ifdef DEBUG_MSG
	if( argc > 1 ) {
serprintf("\r\nperforming debug commands:...\r\n\r\n");	
		int i;
		for( i = 1; i < argc; i++ ) {
			debug_do_cmd( argv[i] );
		}
serprintf("\r\n...done with serial commands\r\n\r\n");	
	}
#endif	// DEBUG_MSG
#endif
	add_self_to_list_of_threads("mainloop");

	mainloop_init();

	mainloop_enter();

	mainloop_deinit();

	LOG_close();

	return 0;
}

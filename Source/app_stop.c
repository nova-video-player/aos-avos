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
#include "app.h"
#include "debug.h"
#include "i18n.h"
#include "file.h"
#include "athread.h"
#include "mainloop.h"

#include "audio_interface.h"

#include "avos_lifetime.h"

// ************************************************
//
//	app_stop
//
// ************************************************
void app_stop( void )
{
	avos_exit(AVOS_RUNLEVEL_APP);
	avos_exit(AVOS_RUNLEVEL_BC);

	mainloop_exit();

#if defined( DRIVE_SIM )
	drive_exit();
#endif


#ifdef CONFIG_I18N
	I18N_unload();
#endif
	// See what threads are running before saving params (debug info only)
	apthread_print_list();

	audio_interface_exit();

#ifdef GFI_ASYNC
	get_file_info_server_stop();
#endif

#ifndef SIM
	OSD_Disable();
#endif
	avos_exit(AVOS_RUNLEVEL_PLATFORM);

	avos_clean_files();
}

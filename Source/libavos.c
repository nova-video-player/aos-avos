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

#include <stdlib.h>

#include "global.h"
#include "log.h"
#include "av.h"
#include "avos_lifetime.h"
#include "audio_interface.h"
#include "debug.h"
#include "mainloop.h"
#include "pthread.h"
#include "timers.h"
#include "file.h"
#include "device_config.h"
#include "i18n.h"
#include "audio_spdif.h"
#include "stream.h"

#ifdef CONFIG_ANDROID
#include "jni.h"

JavaVM *myVm = NULL;
jobject myClassLoader;
jmethodID myFindClassMethod;
#endif

void device_config_init();
void device_config_set_android_pkg_name(const char *pkg_name);
void device_config_set_pluginlib(int pluginlib);
void device_config_set_subtitlepath(const char *path);
void device_config_set_decoder(int decoder);
void device_config_set_output_sample_rate(int sample_rate);

static pthread_t mainloop_thread;

//#define DUMP_OMX

void libavos_init(const char *name, const char *pkg_name, int has_pluginlib)
{
	LOG_open_name(name);
	serprintf("libavos_init\n");
	device_config_init();
	device_config_set_android_pkg_name(pkg_name);
	device_config_set_pluginlib(has_pluginlib);
	audio_interface_init();
#ifdef DUMP_OMX
	debug_do_cmd("dumpomx");
#endif
}

void libavos_exit()
{
	serprintf("libavos_exit");
	LOG_close();
	audio_interface_exit();
}

static void *mainloop_routine(void *ctx)
{
	serprintf("creating mainloop (for debug only)\n");
	avos_init(AVOS_RUNLEVEL_PLATFORM);

	Timers_init( &gui_timers );

	avos_init(AVOS_RUNLEVEL_BC);
	avos_init(AVOS_RUNLEVEL_APP);
	mainloop_init();
	mainloop_enter();
	mainloop_deinit();

	serprintf("leaving  mainloop (for debug only)\n");
	return NULL;
}

void libavos_debug_init()
{
	pthread_create(&mainloop_thread, NULL, mainloop_routine, NULL);
}

void libavos_debug_exit()
{
	avos_exit(AVOS_RUNLEVEL_APP);
	avos_exit(AVOS_RUNLEVEL_BC);
	avos_exit(AVOS_RUNLEVEL_PLATFORM);
	mainloop_exit();
	pthread_join(mainloop_thread, NULL);
	avos_clean_files();
}

void libavos_avsh(const char *cmd)
{
#ifdef DEBUG_MSG
	serprintf("\n-------------------------------------------------------\n");
	debug_do_cmd(cmd);
	serprintf("-------------------------------------------------------\n");
#endif
}

void libavos_set_subtitlepath(const char *path)
{
	device_config_set_subtitlepath(path);
}

void libavos_set_decoder(int decoder)
{
	device_config_set_decoder(decoder);
}

void libavos_set_codepage(int codepage)
{
	I18N_set_codepage(codepage);
}

void libavos_set_output_sample_rate(int sample_rate)
{
	device_config_set_output_sample_rate(sample_rate);
}

void libavos_set_passthrough(int force_passthrough)
{
#ifdef CONFIG_SPDIF
	audio_interface_exit();
	spdif_set_passthrough(force_passthrough);
	audio_interface_init();
#endif
}

void libavos_set_downmix(int downmix)
{
	stream_set_audio_downmix(downmix);
}

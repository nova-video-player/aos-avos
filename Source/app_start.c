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
#include "util.h"
#include "types.h"
#include "file_info.h"
#include "astdlib.h"
#include "fs.h"
#include "timers.h"
#include "i18n.h"
#include "atime.h"
#include "avos_lifetime.h"
#include "malloc.h"

#include <signal.h>
#include <sys/resource.h>
#include <stdio.h>

#define ERR if(1)

#ifndef SIM
#include "dma_alloc.h"
#endif

#if !defined (CONFIG_RELEASE) && !defined (G8A)
#define ALWAYS_COREDUMP
#endif

#include "dbg_alloc.h"

pid_t avos_pid;

void device_config_init( void );

// *********************************************************************
//
//	signal_handler
//
// *********************************************************************
static void signal_handler(int signum)
{
	if (signum == SIGUSR1) {
//serprintf("SIGUSR1 received\n");
		return;
	} else if (signum == SIGPIPE) {
serprintf("SIGPIPE received\n");
		return;
	} else if (signum == SIGALRM) {
serprintf("SIGALRM received\n");
		return;
	}

serprintf("\nTERMINATED( %d )\n", signum);

#ifdef DMALLOC
	dmalloc_shutdown();
#endif
#ifdef GFI_ASYNC
	get_file_info_server_stop();
#endif
	asystem_exit();
	
	avos_clean_files();

	LOG_close();

#ifndef CONFIG_ANDROID
	// dump core:
	abort();
#endif
	
	exit(1);
}

// *********************************************************************
//
//	install_signal_handler
//
// *********************************************************************
static void install_signal_handler( void )
{
serprintf("install_signal_handler\n");
	struct sigaction act = { 0 };

	sigemptyset(&act.sa_mask);
	// Install a signal handler to ummute the soundcard
	// if we crash or CRTL-C
	act.sa_handler = signal_handler;
	sigaction(SIGALRM, &act, NULL);
	sigaction(SIGINT,  &act, NULL);
	sigaction(SIGQUIT, &act, NULL);
	sigaction(SIGILL, &act, NULL);
	sigaction(SIGPIPE, &act, NULL);
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGSEGV, &act, NULL);
	sigaction(SIGFPE,  &act, NULL);
	sigaction(SIGBUS,  &act, NULL);
	sigaction(SIGUSR1,  &act, NULL);
}

DECLARE_DEBUG_COMMAND_VOID("ish", install_signal_handler );
#ifdef DMALLOC
DECLARE_DEBUG_COMMAND_VOID("dmalloc", dmalloc_shutdown );
#endif

void asignal( int sig )
{
#ifndef SIM	
	kill( avos_pid, sig );
#endif
}

#ifndef SIM
//#ifndef CONFIG_RELEASE
#ifdef ALWAYS_COREDUMP
// *********************************************************************
//
//	getBuildString
//
// *********************************************************************
static int getBuildString(char *buf, int buf_len)
{
	const char *build_info_file = "/usr/share/build_info";
	int fd = file_open(build_info_file, O_RDONLY, 0600);
	if ( fd == -1 ) {
		serprintf("no build info file found.\n");
		return 1;
	}

	FILE *f = fdopen(fd, "r");
	if ( !f ) {
		serprintf("failed to fdopen the build info file.\n");
		return 1;
	}

	fgets(buf, buf_len, f);

	fclose(f);
	return 0;
}

// *********************************************************************
//
//	enableCoreFiles
//
// *********************************************************************
static void enableCoreFiles(void)
{
	const char* core_pattern = "/proc/sys/kernel/core_pattern";

	FILE *f;

serprintf("\nCAUTION: corefiles enabled\n\n");
	f = fopen(core_pattern, "w");
	if ( !f ) {
		serprintf("failed to open '%s' : %s\n", core_pattern, strerror(errno));
	}

	char build_info[256];
	if ( getBuildString(build_info, sizeof(build_info)) ) {
		fprintf(f, HDD_ROOT"core-time_%%t\n");
	}
	else {
		fprintf(f, HDD_ROOT"core-time_%%t-build_%s\n", build_info);
	}

	fclose(f);

	struct rlimit core_limit = {
		.rlim_cur = RLIM_INFINITY,
		.rlim_max = RLIM_INFINITY
	};
	if ( setrlimit(RLIMIT_CORE, &core_limit) ) {
		serprintf("failed to setup RLIMIT_CORE: %s\n", strerror(errno));
	}
}
#endif // ALWAYS_COREDUMP
//#endif // !CONFIG_RELEASE

static void disableAlignmentTraps(void)
{
#if defined(NO_ALIGNMENT_TRAPS) || defined(CONFIG_RELEASE)
	FILE *cpu_alignment = fopen("/proc/cpu/alignment", "w");
	fprintf(cpu_alignment, "2\n");
	fclose(cpu_alignment);
#endif
}
#endif // !SIM


// ************************************************
//
//	app_start
//
//	setup everything
//
// ************************************************
void app_start( int argc, char *argv[] )
{
	serprintf(" \r\n");
	serprintf(" \r\n");
	serprintf("*************************************\r\n");
	serprintf("*           ARCHOS AVX/AVOS         *\r\n");
	serprintf("*    (c)  2005-2012 Archos Team     *\r\n");
	serprintf("*************************************\r\n");
	serprintf(" \r\n");
	serprintf("\r\n");
	serprintf( __DATE__ "  " __TIME__"\r\n");
	serprintf(" \r\n");
	serprintf(" \r\n");

	serprintf("Args:\r\n");
	int i;
	for( i = 0; i < argc; i++ ) {
		serprintf("\t%2d %s\r\n", i, argv[i] );
	}
	serprintf(" \r\n");

	avos_pid = getpid();

	avos_clean_files();

	avos_init(AVOS_RUNLEVEL_PLATFORM);

#ifdef GFI_ASYNC
	if (get_file_info_server_launch() == -1){
serprintf("Could not start gfi server.\r\n");
		return;
        }
#endif

	asystem_init();
#ifndef SIM
#ifdef ALWAYS_COREDUMP
	enableCoreFiles();
#endif
	disableAlignmentTraps();
#endif

	install_signal_handler();

#if defined( DRIVE_SIM )
	drive_init();
#endif

	time_init_time();

#if defined( CONFIG_I18N ) && defined( G8L )
	I18N_load();
#endif

	Timers_init( &gui_timers );

	// AVOS should be least likely to be slain by the oom killer
	adjust_oom(getpid(), -16);

	device_config_init();

	avos_init(AVOS_RUNLEVEL_BC);
	avos_init(AVOS_RUNLEVEL_APP);

	serprintf("avos_pid: %d\n", avos_pid);
}

#ifdef DEBUG_MSG
static void _avos_segfault( void )
{
	int *p = 0;
	*p = 12;	//boom
}

DECLARE_DEBUG_COMMAND_VOID("boom", _avos_segfault);
#endif

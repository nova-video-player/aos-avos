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
#include "util.h"
#include "fs.h"
#include "atime.h"
#include "stream.h"
#include "hardware.h"
//#include "app.h"
//#include "app_state.h"
#include "debug.h"
#include "astdlib.h"
#include "dataevent.h"
#include "athread.h"
#include "keyboard.h"
//#include "sysfs_ll.h"

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <fcntl.h>

#ifdef DEBUG_MSG

static void AC_hang( int argc, char *argv[] )
{
serprintf("SW hangs!\n"); 
	while(1);
}
DECLARE_DEBUG_COMMAND("hang", AC_hang );

static pthread_t	catio_thread;
static volatile int	catio_run = 0;

static void *catio_threadle( void *data )
{
	char *url = data;
	STREAM_URL src;
	
	stream_url_cpy_url(&src, url);
	STREAM_IO *io = stream_get_new_io( &src );
	
	if( !io ) {
serprintf("catio: no IO for %s\r\n", url);
		goto ErrorExit;
	}
	
	if( io->open( io, O_RDONLY )){
		io->delete(io);
serprintf("catio: cannot open IO for %s\r\n", url);
		goto ErrorExit;
	}

	UINT64 size = stream_io_get_size( io );
serprintf("catio: size %lld\r\n", size );

	int start = atime();
	UINT64 count = 0;
	while( count < size && catio_run ) {
		char buffer[65536];
		int ret = io->read( io, buffer, sizeof( buffer ) );
		if( ret < 0 ) {
serprintf("catio: error %d\r\n", ret );
			break;
		} 
//serprintf(".");
		count += ret;
		if ( ret != sizeof( buffer ) ) {
serprintf("catio: done\r\n" );
			break;
		}
	}	
	start = atime() - start;
serprintf("catio: read %lld of %lld bytes in %dms = %d bytes/s\r\n", count, size, start, start ? 1000 * count / start : -1  );	
	io->close (io);
	io->delete(io);

ErrorExit:
	catio_run = 0;
	return NULL;
}

static void cat_io( int argc, char *argv[] )
{
	if( catio_run ) {
		catio_run = 0;
		return;
	}
	
	if (argc < 2) {
serprintf("catio <url>\r\n");
		return;
	}

	catio_run = 1;
	
	pthread_create( &catio_thread, NULL, catio_threadle, (void*)argv[1] );
	pthread_detach( catio_thread );
}
DECLARE_DEBUG_COMMAND("catio", cat_io);

// core dumps
static void __core( void )
{
serprintf("core now\r\n");
	//recent compilers don't like *((int*)0) = 1;
	raise( SIGABRT );
}
DECLARE_DEBUG_COMMAND_VOID( "core", __core );

static void __abort( void )
{
serprintf("abort now\r\n");
	abort();
}
DECLARE_DEBUG_COMMAND_VOID( "abort", __abort );

static void _sigabrt( void )
{
serprintf("SIGABRT now\r\n");
	kill( getpid(), SIGABRT );
}
DECLARE_DEBUG_COMMAND_VOID( "sigabrt", _sigabrt );

static void _sigsegv( void )
{
serprintf("SIGSEGV now\r\n");
	kill( getpid(), SIGSEGV );
}
DECLARE_DEBUG_COMMAND_VOID( "sigsegv", _sigsegv );

static void _sigfpe( void )
{
serprintf("SIGFPE now\r\n");
	kill( getpid(), SIGFPE );
}
DECLARE_DEBUG_COMMAND_VOID( "sigfpe", _sigfpe );

static void _sigbus( void )
{
serprintf("SIGBUS now\r\n");
	kill( getpid(), SIGBUS );
}
DECLARE_DEBUG_COMMAND_VOID( "sigbus",  _sigbus );

static void data_events_dump_mainloop(void)
{
	data_events_dump(&mainloop_events);
}
DECLARE_DEBUG_COMMAND_VOID("ded", data_events_dump_mainloop );

static void _test( int argc, char *argv[] ){
	serprintf("test %d, %s\r\n", argc, argv[1]);
}
DECLARE_DEBUG_COMMAND("test", _test);
#endif

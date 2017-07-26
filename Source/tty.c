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
#include "tty.h"
#include "debug.h"
#include "dataevent.h"

#include <string.h>
#include <stdio.h>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/kd.h>
#include <linux/vt.h>

static int tty_open = 0;
#if !defined (CONFIG_RELEASE)
struct de_ctx {
	data_event_t de;
	void(*handle_char)(unsigned char);
};
static struct de_ctx de_ctx; 
static struct termios origTermData;
#endif

#if !defined (CONFIG_RELEASE)
// ******************************************************
//
//  _read
//
// ******************************************************
static void _read( void *_ctx )
{
	struct de_ctx *ctx = (struct de_ctx*)_ctx;

	char buffer[128];
	int  len;
	
	if (!tty_open)
		return;

	if ( (len = read( ctx->de.fd, buffer, sizeof( buffer) )) ) {
		int i;
		for( i = 0; i < len; i ++) {
			ctx->handle_char( buffer[i] );
		}
	}	
}
#endif

// *********************************************************************
//
//	TTY_open
//
// *********************************************************************
void TTY_open(void(*handle_char)(unsigned char))
{
#if !defined (CONFIG_RELEASE) 
	if (! tty_open) {
		if( !isatty(STDIN_FILENO) ){
			fprintf(stderr, "WARNING: stdin is not a tty\n");
		} else {
			struct termios termdata;
			tcgetattr( STDIN_FILENO, &origTermData );
			tcgetattr( STDIN_FILENO, &termdata );
	
			termdata.c_lflag = ISIG;
			termdata.c_cc[VMIN] = 0;
	
			tcsetattr(STDIN_FILENO, TCSANOW, &termdata);

			memset(&de_ctx, 0, sizeof(de_ctx));
			de_ctx.de.fd        = STDIN_FILENO;
			de_ctx.de.read_func = _read;
			de_ctx.handle_char  = handle_char;
			
			register_data_event(&mainloop_events, &de_ctx.de, &de_ctx);
		}

		tty_open = 1;
	}
#endif
}

void TTY_close(void)
{
#if !defined (CONFIG_RELEASE)
serprintf("TTY_close\n");
	if (tty_open) {
		if(isatty(STDIN_FILENO)){
			unregister_data_event(&de_ctx.de);
			tcsetattr( STDIN_FILENO, TCSANOW, &origTermData );
		}

		tty_open = 0;
	}
serprintf("\n\n");	
#endif
}

// *********************************************************************
//
//	TTY_write
//
// *********************************************************************
int TTY_write( const char* pString )
{
 	if ( !tty_open ) {
		return 0;
	}

	return write( STDOUT_FILENO, pString, strlen( pString ) );
}

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

/*
	debug.c
		routines for logging debug output on the serial port

*/

#include <stdio.h>

#include "global.h"
#include "util.h"
#include "debug.h"
#include "log.h"
#include "atime.h"

#ifdef SIM
#ifndef UCLIBC
#include <execinfo.h>
#endif
#endif
#include <stdarg.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>
#include <ctype.h>

// debug flags for a non release version
#ifndef CONFIG_RELEASE
int Debug[DBG_MAX_ENTRIES] = { 
	[DBG_VIDEO_PLAYER] = 0,
	[DBG_AUDIO_PLAYER] = 0,
	[DBG_PARSER] = 0,
	[DBG_STREAM] = 0,
	[DBG_SINK] = 0,
	[DBG_FB] = 1,
};
#endif

DECLARE_DEBUG_SWITCH("dbga", 		DBG_AUD);
DECLARE_DEBUG_SWITCH("dbgap",	 	DBG_AUDIO_PLAYER);
DECLARE_DEBUG_SWITCH("dbgvp",	 	DBG_VIDEO_PLAYER);
DECLARE_DEBUG_SWITCH("dbgp", 		DBG_PARSER);
DECLARE_DEBUG_SWITCH("dbgpm", 		DBG_PM);
DECLARE_DEBUG_SWITCH("dbgw", 		DBG_WMA);
DECLARE_DEBUG_SWITCH("dbgc", 		DBG_CHU);
DECLARE_DEBUG_SWITCH("dbgv", 		DBG_VID);
DECLARE_DEBUG_SWITCH("dbgvl", 		DBG_V4L);
DECLARE_DEBUG_SWITCH("dbgs", 		DBG_STREAM);
DECLARE_DEBUG_SWITCH("dbgm",	 	DBG_MALLOC);
DECLARE_DEBUG_SWITCH("dbgy", 		DBG_SYNC);
DECLARE_DEBUG_SWITCH("dbgq", 		DBG_Q);
DECLARE_DEBUG_SWITCH("dbghd", 		DBG_HD);
DECLARE_DEBUG_SWITCH("dbgf", 		DBG_FILE);
DECLARE_DEBUG_SWITCH("dbgimg", 		DBG_IMG);
DECLARE_DEBUG_SWITCH("dbgthumb",	DBG_THUMB);
DECLARE_DEBUG_SWITCH("dbgsink",	 	DBG_SINK);
DECLARE_DEBUG_SWITCH("dbgca", 		DBG_CA);
DECLARE_DEBUG_SWITCH("dbgcv", 		DBG_CV);
DECLARE_DEBUG_SWITCH("dbgmng", 		DBG_MANGLER);
DECLARE_DEBUG_SWITCH("dbgpsi", 		DBG_PSI);
DECLARE_DEBUG_SWITCH("dbgstate", 	DBG_STATE);
DECLARE_DEBUG_SWITCH("dbgkey", 		DBG_KEY);
DECLARE_DEBUG_SWITCH("dbgpath", 	DBG_PATH);
DECLARE_DEBUG_SWITCH("dbgdv", 		DBG_DV);
DECLARE_DEBUG_SWITCH("dbgdrm", 		DBG_DRM);
DECLARE_DEBUG_SWITCH("dbgrd", 		DBG_REDRAW);
DECLARE_DEBUG_SWITCH("dbgdvs", 		DBG_DVS);
DECLARE_DEBUG_SWITCH("dbgcomp",		DBG_COMP);
DECLARE_DEBUG_SWITCH("dbgsub",		DBG_SUB);
DECLARE_DEBUG_SWITCH("dblayout",	DBG_LAYOUT);
DECLARE_DEBUG_SWITCH("dbblend",		DBG_BLENDING);
DECLARE_DEBUG_SWITCH("dbgrsz",		DBG_RESIZER);
DECLARE_DEBUG_SWITCH("dbgad",	 	DBG_AUDIODEVICE);
DECLARE_DEBUG_SWITCH("dbgd",	 	DBG_DSP);
DECLARE_DEBUG_SWITCH("dbgpr",	 	DBG_PROBE);
DECLARE_DEBUG_SWITCH("dbgt",	 	DBG_THREADS);
DECLARE_DEBUG_SWITCH("dbgdss",	 	DBG_DSS);

#ifdef DEBUG_MSG
static int isHexString(const char* s)
{
	if ( strlen(s) >= 3 ) {
		if ( s[0] == '0' && ( s[1] == 'x' || s[1] == 'X') ) {
			return 1;
		}
	}
	return 0;
}

void DebugSwitch( char *name, int dbg, int argc, char *argv[] )
{
serprintf("%s : ", name );	
	if (argc > 1) {
		const char *arg = argv[1];
		if ( isHexString(arg) ) {
			int level = 0;
			if ( sscanf(arg + 2, "%x", &level) ) {
				Debug[dbg] = level;
			}
serprintf("[0x%x]\r\n", Debug[dbg] );
			return;
		}
		else {
			Debug[dbg] = atoi( argv[1] );
serprintf("[%d]\r\n", Debug[dbg] );	
			return;
		}
	}

	if ( Debug[dbg] ) {
		 Debug[dbg] = 0;
serprintf("OFF\r\n");
	} else {
		 Debug[dbg] = 1;
serprintf("ON\r\n");
	}		 
} 

#ifdef SIM
#ifndef UCLIBC
//------------------------------------------------------------------------
//	Trace
//------------------------------------------------------------------------
void Trace( void )
{
	void *array[10];
	size_t size = backtrace( array, 10 );
	char **strings = backtrace_symbols( array, size );
	
	serprintf( "Obtained %zd stack frames.\n", size );
	
	size_t i;
	for( i = 0; i < size; i++ )
		serprintf( "%s\n", strings[i] );
	
	free( strings );
}
#endif // UCLIBC
#endif  // SIM

//------------------------------------------------------------------------
//	DumpLine
//------------------------------------------------------------------------
void DumpLine( const unsigned char *buf, int size, int max)
{
	char printable[] = "abcdefghijklmnopqrstuvwxyz!/\\\"#$%&'()*?+,-=|[]ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:^._<>{} ";
	char *c;
	int j;
	for ( j = 0; j < max; j++ ) {
		if ( j < size )
			serprintf("%02X", buf[j] );		
		else
			serprintf("  ");		
		if ( ( ( j - 3 ) % 4 ) == 0 ) {
			serprintf("|");
		} else {
			serprintf(" ");
		}
	}
	serprintf(" |");

	for ( j = 0; j < max; j++ ) {
		c = printable;
		while ( *c != ' ' ) {
			if (*c == buf[j] ) {
				break;
			}
			c++;
		}
		if ( j < size )
			serprintf("%c", *c );
		else
			serprintf(" ");
	}
	serprintf("|\r\n");
}

//------------------------------------------------------------------------
//	Dump
//------------------------------------------------------------------------
void Dump( const unsigned char *buf , int size)
{
	int i = 0;

	while( i < size ) {
		if ((i % 512) == 0) {
			serprintf("\r\n[%08X] 00 01 02 03|04 05 06 07|08 09 0A 0B|0C 0D 0E 0F\r\n", (buf + i) );
			serprintf("           -----------+-----------+-----------+-----------\r\n" );
			if (i) {
				msec_sleep( 100 );	
			}
		}		
		serprintf("    [%04X] ", i );
		DumpLine( buf + i, ((size - i) < 16) ? size - i : 16, 16 );
		i += 16;
	}
	serprintf("\r\n\r\n");
}

//------------------------------------------------------------------------
//	Dump16
//------------------------------------------------------------------------
void Dump16( const unsigned short *buf , int size)
{
	int i = 0;

    	size &= ~1;

	while( i < size ) {
		if ((i % 512) == 0) {
			serprintf("\r\n[%08X] 00    02   |04    06   |08    0A   |0C    0E   \r\n", (buf + i) );
			serprintf("           -----------+-----------+-----------+-----------\r\n" );
			if (i) {
				msec_sleep( 100 );	
			}
		}		
		serprintf("    [%04X] ", i );
		int j;
		for ( j = i; j < i + 16; j += 2 ) {
			if ( j < size )
				serprintf("%04X", *buf++ );		
			else
				serprintf("    ");		
			if ( ( ( j/2 - 1 ) % 2 ) == 0 ) {
				serprintf(" |");
			} else {
				serprintf("  ");
			}
		}
	        serprintf("\r\n");
		i += 16;
	}
	serprintf("\r\n\r\n");
}

//------------------------------------------------------------------------
//	Dump32
//------------------------------------------------------------------------
void Dump32( const unsigned long *buf , int size)
{
	int i = 0;
   	
	size &= ~3;

	while( i < size ) {
		if ((i % 512) == 0) {
			serprintf("\r\n[%08X] 00         |04         |08         |0C        \r\n", (buf + i) );
			serprintf("           -----------+-----------+-----------+-----------\r\n" );
			if (i) {
				msec_sleep( 100 );	
			}
		}		
		serprintf("    [%04X] ", i );
		int j;
		for ( j = i; j < i + 16; j+= 4 ) {
			if ( j < size )
				serprintf("%08X", *buf++ );		
			else
				serprintf("        ");		
			serprintf("   |");
		}
	        serprintf("\r\n");
		i += 16;
	}
	serprintf("\r\n\r\n");
}

static DEBUG_REG_COMMAND *_debug_reg_command = NULL;

// ************************************************************
//
//	debug_register_cmd
//
// ************************************************************
void debug_register_cmd( DEBUG_REG_COMMAND *reg )
{
	if( !_debug_reg_command ) {
		_debug_reg_command = reg;
	} else {
		DEBUG_REG_COMMAND *head = _debug_reg_command;
		while( head->next ) {
			head = head->next;
		} 
		head->next = reg;
	}
	reg->next = NULL;
	return;
}

// ************************************************************
//
//	debug_param
//
// ************************************************************
void debug_param( int argc, char *argv[], int *param, char *name )
{
	if( !param )
		return;
	if( argc > 1 )
		*param = atoi(argv[1]);
serprintf("%s: %d\r\n", name, *param );
}

// ************************************************************
//
//	debug_toggle
//
// ************************************************************
void debug_toggle( int *param, char *name )
{
	if( !param )
		return;
	*param = *param ? 0 : 1;
serprintf("%s: %d\r\n", name, *param );
}

// ************************************************************
//
//	_help
//
// ************************************************************
static void _help( int argc, char *argv[] )
{
	serprintf("Command:          Description:                               Source:\r\n");	
	serprintf("--------------------------------------------------------------------\r\n");	
	DEBUG_REG_COMMAND *reg = _debug_reg_command;
	while ( reg ) {
		int print = 1;
		if( argc > 1 ) {
			int i;
			for ( i = 1; i < argc; i ++ ) {
				if( !strstr( reg->name, argv[i] ) && !strstr( reg->help, argv[i] ) && !strstr( reg->cmd_name, argv[i] ) ) {
					print = 0;
					break;
				}
			}
		}
		if( print ) {
			int l = 16 - strlen( reg->name );
			serprintf( "%s ", reg->name );
			for ( ; l > 0 ; --l) {
				serprintf(".");
			}
			if( !strncmp( reg->cmd_name, "_tgl_", 5 ) ) {
				serprintf( " [TOGGLE]: %-30s  ", reg->cmd_name + 5 );		
			} else if( !strncmp( reg->cmd_name, "_prm_", 5 ) ) {
				serprintf( " [PARAM]:  %-30s  ", reg->cmd_name + 5 );		
			} else {
				serprintf( " %-40s  ", reg->cmd_name );		
			}
			serprintf( " %s\r\n", reg->help );		
		}
		reg = reg->next;
	}
}

DECLARE_DEBUG_COMMAND("help", _help );

static int argc = 0;
static unsigned char argv[16][MAX_COMMAND_LEN];

static int get_param( const unsigned char *cmd )
{
	int i = 0, j = 0;

	argc = 0;

	/* remove white spaces at begin */
	while ( isspace( ( int ) cmd[i] ) )
		i++;

	while ( 1 ) {
		if ( isspace( ( int ) cmd[i] ) || cmd[i] == 0 ) {
			argv[argc][j] = 0;

			++argc;

			if ( cmd[i] == 0 )
				break;

			j = 0;

			while ( isspace( ( int ) cmd[i] ) )
				++i;
			continue;
		}

		argv[argc][j] = cmd[i];
		++i;
		++j;
	}

	return argc;
}

static void _do_system_cmd( const unsigned char* cmd )
{
	int ret = system( cmd );
	if (ret < 0)
		serprintf("ERROR: %s\r\n", strerror(errno));
}

void debug_do_cmd(const unsigned char *cmd)
{
	int j;

	if( cmd[0] == '!' ) {
		_do_system_cmd( cmd + 1 );
		return;
	}
	
	get_param(cmd);
    
	DEBUG_REG_COMMAND *reg = _debug_reg_command;

	while( reg  ) {
#if 0 // if you like to execute the first chars of a command
		j = 0;
		int k = 0;
	
		while (reg->name[j] != 0 && argv[0][j] != 0) {
			if (reg->name[j] != argv[0][j]) {
				k++;
				break;
	    		}
	    		j++;
		}	
	
		if (k != 0) {
			reg = reg->next;
			continue;
		}
#else
		if (strcmp(reg->name, argv[0]) != 0) {
			reg = reg->next;
			continue;
		}
		// check for double commands!
		DEBUG_REG_COMMAND *reg2 = reg->next;
		while( reg2 ) {
			if(!strcmp(reg->name, reg2->name) ) {
				serprintf("WARNING: %s also defined here: %s\n", reg->name, reg->help);
			}
			reg2 = reg2->next;
		}
#endif	
		// reorganize the data
		char *argv_ptr[16];

		for( j = 0 ; j < 16 ; j++ ){
			argv_ptr[j] = argv[j];
		}
		(reg->cmd)(argc, argv_ptr);
	
		return;
	}
    
    serprintf("%s: command not found\n", argv[0]);
}
static void _ping() {}
DECLARE_DEBUG_COMMAND_VOID( "ping", 	_ping );

#else // DEBUG_MSG not defined ----------------------------------------

#endif

// fake debug flags for a release version
#ifdef CONFIG_RELEASE
const int Debug[DBG_MAX_ENTRIES];
#endif

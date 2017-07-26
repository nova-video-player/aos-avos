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

/* ****************************

 Defines for DEBUG options

 ****************************
*/
#ifndef _DEBUG_H_
#define _DEBUG_H_

#include "global.h"
#include "log.h"

#include <stdarg.h>

enum {
	DBG_HD = 0,
	DBG_DSP,
	DBG_VID,
	DBG_PARSER,
	DBG_CHU,
	DBG_WMA,
	DBG_KEY,
	DBG_PARAMS,
	DBG_PATH,
	DBG_I18N,
	DBG_DRM,
	DBG_FILE,
	DBG_V4L,
	DBG_IMG,
	DBG_STREAM,
	DBG_MALLOC,
	DBG_THUMB,
	DBG_AUD,
	DBG_AUDIO_PLAYER,
	DBG_CA,		// codec audio
	DBG_CV,		// codec video
	DBG_DSS,	// DSS subsystem
	DBG_SINK,
	DBG_PM,		// POWER MANAGEMENT ( HDD etc..)
	DBG_MANGLER,	// video mangler
	DBG_PSI,	// PSI handling
	DBG_STATE,
	DBG_AGC,
	DBG_DV,		// DataViews
	DBG_DVS,	// DVSource
	DBG_AS,		// apps & screens
	DBG_LCD,	// drawing operations
	DBG_MMS,	// microsoft streaming operations
	DBG_REDRAW,	// debug redraws
	DBG_SUB,
	DBG_PROBE,
	DBG_COMP,
	DBG_DE,		// data events
	DBG_TC,		// threadcom
	DBG_LAYOUT,
	DBG_BLENDING,
	DBG_RESMGNR,
	DBG_FB,		// frame buffer driver
	DBG_VIDEO_PLAYER,
	DBG_RESIZER,
	DBG_AUDIODEVICE,
	DBG_AUDIO_ENGINE,
	DBG_DOUBLEBROWSER,
	DBG_SYNC,
	DBG_Q,		// ha, Q!
	DBG_THREADS,
	DBG_MAX_ENTRIES	// must alway be last !
};

#ifdef CONFIG_RELEASE
extern const int Debug[DBG_MAX_ENTRIES];
#else
extern int Debug[DBG_MAX_ENTRIES];
#endif

#if defined( SIM ) && defined ( DEBUG_MSG ) && !( UCLIBC )
void Trace( void ); // dumps the callstack
#else
static inline void Trace( void ) {}
#endif

#ifdef DEBUG_MSG
void 	DumpLine( const unsigned char *buf , int size, int max);
void 	Dump( const unsigned char *buf , int size);
void 	Dump16( const unsigned short *buf , int size);
void 	Dump32( const unsigned long *buf , int size);

void 	DebugSwitch( char *name, int dbg, int argc, char *argv[] );

#define MAX_COMMAND_LEN   	512

typedef void (*DEBUG_COMMAND)( int argc, char *argv[] );

typedef struct DEBUG_REG_COMMAND {
	const char 		*name;
	DEBUG_COMMAND		cmd;
	const char		*cmd_name;
	const char		*help;
	struct DEBUG_REG_COMMAND *next;
} DEBUG_REG_COMMAND;

void debug_register_cmd( DEBUG_REG_COMMAND *cmd );
void debug_param( int argc, char *argv[], int *param, char *name );
void debug_toggle( int *param, char *name );
void debug_do_cmd(const unsigned char *cmd);

#define STR(x) #x
#define XSTR(x) STR(x)

#define DECLARE_DEBUG_COMMAND( name, fn ) \
		static DEBUG_REG_COMMAND _reg_##fn = { \
			name, \
			fn,\
			#fn,\
			__FILE__", line " XSTR(__LINE__),\
			NULL\
		}; \
		static void _fn_reg_##fn( void ) __attribute__((constructor));\
		static void _fn_reg_##fn( void )\
		{ \
			debug_register_cmd( &_reg_##fn ); \
		}

#define DECLARE_DEBUG_COMMAND_VOID( name, fn ) \
		static DEBUG_REG_COMMAND _regvoid_##fn = { \
			name, \
			(DEBUG_COMMAND)fn,\
			#fn,\
			__FILE__", line " XSTR(__LINE__),\
			NULL\
		}; \
		static void _fn_regvoid_##fn( void ) __attribute__((constructor));\
		static void _fn_regvoid_##fn( void )\
		{ \
			debug_register_cmd( &_regvoid_##fn ); \
		}

#define DECLARE_DEBUG_SWITCH( name, debug ) \
		static void debug_switch_##debug( int argc, char *argv[] )\
		{ \
			DebugSwitch( #debug, debug, argc, argv);\
		} \
		static DEBUG_REG_COMMAND _reg_##debug = { \
			name, \
			debug_switch_##debug,\
			#debug,\
			__FILE__", line " XSTR(__LINE__),\
			NULL\
		}; \
		static void _fn_reg_##debug( void ) __attribute__((constructor));\
		static void _fn_reg_##debug( void )\
		{ \
			debug_register_cmd( &_reg_##debug ); \
		}

#define DECLARE_DEBUG_PARAM( name, param ) \
		static void _prm_##param( int argc, char *argv[] ) { \
			debug_param( argc, argv, &param, XSTR(param) ); \
		} \
		DECLARE_DEBUG_COMMAND( name, _prm_##param );

#define DECLARE_DEBUG_TOGGLE( name, param ) \
		static void _tgl_##param( int argc, char *argv[] ) { \
			debug_toggle( &param, XSTR(param) ); \
		} \
		DECLARE_DEBUG_COMMAND_VOID( name, _tgl_##param );

#define TRC	serprintf("%s:%i\n", __FUNCTION__, __LINE__);

#else
static inline void DumpLine( unsigned char *buf , int size, int max ) {}
static inline void Dump( unsigned char *buf , int size) {}
static inline void Dump16( unsigned short *buf , int size) {}
static inline void Dump32( unsigned long *buf , int size) {}

#define DECLARE_DEBUG_COMMAND( name, fn )
#define DECLARE_DEBUG_SWITCH( name, debug )
#define DECLARE_DEBUG_COMMAND_VOID( name, fn )
#define DECLARE_DEBUG_PARAM( name, param )
#define DECLARE_DEBUG_TOGGLE( name, param )
#define TRC

#endif

#endif // _DEBUG_H_


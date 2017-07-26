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

#ifndef _STREAM_CONFIG_H
#define _STREAM_CONFIG_H

#include "stream.h"

struct STREAM_REG_PARSER;

typedef struct STREAM_REG_PARSER {
	int			etype;
	const STREAM_PARSER 	*parser;
	struct STREAM_REG_PARSER *next;
} STREAM_REG_PARSER;

int stream_register_parser( STREAM_REG_PARSER *reg );
int stream_unregister_parser( int etype );
STREAM_PARSER *stream_get_parser( int etype );

#define STREAM_REGISTER_PARSER( etype, parser ) \
		static STREAM_REG_PARSER _reg_##etype##parser = { \
			etype, \
			&parser,\
			NULL\
		}; \
		static void _fn_reg_##etype##parser( void ) __attribute__((constructor));\
		static void _fn_reg_##etype##parser( void )\
		{ \
			stream_register_parser( &_reg_##etype##parser ); \
		}

struct STREAM_REG_DUMPER;

typedef struct STREAM_REG_DUMPER {
	int			type;
	int			format;
	const STREAM_DUMPER 	*dumper;
	struct STREAM_REG_DUMPER *next;
} STREAM_REG_DUMPER;

int stream_register_dumper( STREAM_REG_DUMPER *reg );
STREAM_DUMPER *stream_get_dumper( int type, int format );

#define STREAM_REGISTER_DUMPER( type, format, dumper ) \
		static STREAM_REG_DUMPER _reg_##type##format##dumper = { \
			type, \
			format, \
			&dumper,\
			NULL\
		}; \
		static void _fn_reg_##type##format##dumper( void ) __attribute__((constructor));\
		static void _fn_reg_##type##format##dumper( void )\
		{ \
			stream_register_dumper( &_reg_##type##format##dumper ); \
		}

struct STREAM_REG_DEC_AUDIO;

typedef struct STREAM_REG_DEC_AUDIO {
	int			format;
	const STREAM_DEC_AUDIO 	*decoder;
	int			max_channels;
	struct STREAM_REG_DEC_AUDIO *next;
} STREAM_REG_DEC_AUDIO;

int stream_register_dec_audio_head( STREAM_REG_DEC_AUDIO *reg );
int stream_register_dec_audio( STREAM_REG_DEC_AUDIO *reg );
int stream_unregister_dec_audio( int format );
STREAM_DEC_AUDIO *stream_get_audio_dec( AUDIO_PROPERTIES *audio );

#define STREAM_REGISTER_DEC_AUDIO( fmt, dec, channels ) \
		static STREAM_REG_DEC_AUDIO _reg_##fmt##dec = { \
			fmt, \
			&dec,\
			channels,\
			NULL\
		}; \
		static void _fn_reg_##fmt##dec( void ) __attribute__((constructor));\
		static void _fn_reg_##fmt##dec( void )\
		{ \
			stream_register_dec_audio( &_reg_##fmt##dec ); \
		}

typedef STREAM_DEC_VIDEO *(*STREAM_DEC_VIDEO_NEW)( void );

typedef enum
{
	GPP  = STREAM_CPU_ARM,
	GPP2 = STREAM_CPU_ARM2,
	DSP2 = STREAM_CPU_DSP2,
	DSP3 = STREAM_CPU_DSP3,
	DSP4 = STREAM_CPU_DSP4,
	DSP  = STREAM_CPU_DSP4,
	LIBAV = STREAM_CPU_ARM,
	OMXSW = STREAM_CPU_ARM2,
	SFDEC_OMXCODEC = STREAM_CPU_DSP2,
	SFDEC_MEDIACODEC = STREAM_CPU_DSP3,
	OMXHW = STREAM_CPU_DSP4,
} CPU_TYPE;

struct STREAM_REG_DEC_VIDEO;
typedef struct STREAM_REG_DEC_VIDEO {
	int				format;
	int				subfmt;
	int				max_width;
	int				max_height;
	int				max_profile;
	int				cpu;
	STREAM_DEC_VIDEO_NEW 		new;
	const char			*name;
	const STREAM_VIDEO_MANGLER 	*mangler;
	struct STREAM_REG_DEC_VIDEO 	*next;
} STREAM_REG_DEC_VIDEO;

int stream_register_dec_video( STREAM_REG_DEC_VIDEO *reg );
int stream_unregister_dec_video( int format );

#define STREAM_REGISTER_DEC_VIDEO2( fmt, subfmt, w, h, profile, cpu, dec, name, mangler ) \
		static STREAM_REG_DEC_VIDEO _reg_##fmt##subfmt##dec = { \
			fmt, \
			subfmt, \
			w, \
			h, \
			profile, \
			cpu, \
			&dec, \
			name, \
			mangler, \
			NULL\
		}; \
		static void _fn_reg_##fmt##subfmt##dec( void ) __attribute__((constructor));\
		static void _fn_reg_##fmt##subfmt##dec( void )\
		{ \
			stream_register_dec_video( &_reg_##fmt##subfmt##dec ); \
		}

#define STREAM_REGISTER_DEC_VIDEO( fmt, subfmt, w, h, cpu, dec, name, mangler ) STREAM_REGISTER_DEC_VIDEO2( fmt, subfmt, w, h, 0, cpu, dec, name, mangler ) 

typedef STREAM_DEC_SUB *(*STREAM_DEC_SUB_NEW)( void );

struct STREAM_REG_DEC_SUB;
typedef struct STREAM_REG_DEC_SUB {
	int				format;
	STREAM_DEC_SUB_NEW 		new;
	const char			*name;
	struct STREAM_REG_DEC_SUB 	*next;
} STREAM_REG_DEC_SUB;

int stream_register_dec_sub( STREAM_REG_DEC_SUB *reg );

#define STREAM_REGISTER_DEC_SUB( fmt, dec, name ) \
		static STREAM_REG_DEC_SUB _regsub_##fmt##dec = { \
			fmt, \
			&dec, \
			name, \
			NULL\
		}; \
		static void _fn_regsub_##fmt##dec( void ) __attribute__((constructor));\
		static void _fn_regsub_##fmt##dec( void )\
		{ \
			stream_register_dec_sub( &_regsub_##fmt##dec ); \
		}

typedef STREAM_IO *(*STREAM_IO_NEW)( STREAM_URL *src );

enum {
	STREAM_IO_LOCAL = 0,
	STREAM_IO_NONLOCAL,
};

struct STREAM_REG_IO;
typedef struct STREAM_REG_IO {
	const char 		*protocol;
	STREAM_IO_NEW		new;
	int			nonlocal;
	int			etype;
	struct STREAM_REG_IO 	*next;
} STREAM_REG_IO;

int stream_register_io( STREAM_REG_IO *reg );

STREAM_IO *stream_get_new_io( STREAM_URL *src );
int stream_get_io_nonlocal( const char *path );
int stream_get_io_etype( const char *path );

#define STREAM_REGISTER_IO( proto, new, nonlocal, etype ) \
		static STREAM_REG_IO _reg_##proto##new = { \
			proto, \
			&new, \
			nonlocal, \
			etype, \
			NULL\
		}; \
		static void _fn_reg_##proto##new( void ) __attribute__((constructor));\
		static void _fn_reg_##proto##new( void )\
		{ \
			stream_register_io( &_reg_##proto##new ); \
		}


#endif

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
#include "device_config.h"
#include "util.h"
#include "stream.h"

#ifdef CONFIG_ANDROID
#include "android_config.h"
int acodecs_is_supported(int format, int is_video, int is_sw_allowed);
#endif

#define DBGS 	if(Debug[DBG_STREAM])

#ifdef CONFIG_STREAM

static STREAM_REG_PARSER *_stream_reg_parser = NULL;

// ************************************************************
//
//	stream_register_parser
//
// ************************************************************
int stream_register_parser( STREAM_REG_PARSER *reg )
{
	if( !_stream_reg_parser ) {
		_stream_reg_parser = reg;
	} else {
		STREAM_REG_PARSER *head = _stream_reg_parser;
		while( head->next ) {
			head = head->next;
		} 
		head->next = reg;
	}
	reg->next = NULL;
	return 0;
}

// ************************************************************
//
//	stream_unregister_parser
//
// ************************************************************
int stream_unregister_parser( int etype )
{
	STREAM_REG_PARSER *now  = _stream_reg_parser;
	STREAM_REG_PARSER *prev = NULL;
	while( now ) {
		if( now->etype == etype ) {
			if( prev ) {
				prev->next = now->next;
			} else {
				_stream_reg_parser = now->next;
			}
		} else {
			prev = now;
		}
		now = now->next;
	}

	return 0;
}

// *****************************************************************************
//
//	stream_get_parser
//
// *****************************************************************************
STREAM_PARSER *stream_get_parser( int etype )
{
DBGS serprintf("stream_get_parser( etype %d )\r\n", etype ); 
	STREAM_REG_PARSER *p = _stream_reg_parser;
	while( p ) {
		if( etype == p->etype ) {
			return (STREAM_PARSER*) p->parser;
		}
		p = p->next;
	}
	return NULL;
}

static STREAM_REG_DUMPER *_stream_reg_dumper = NULL;

// ************************************************************
//
//	stream_register_dumper
//
// ************************************************************
int stream_register_dumper( STREAM_REG_DUMPER *reg )
{
	if( !_stream_reg_dumper ) {
		_stream_reg_dumper = reg;
	} else {
		STREAM_REG_DUMPER *head = _stream_reg_dumper;
		while( head->next ) {
			head = head->next;
		} 
		head->next = reg;
	}
	reg->next = NULL;
	return 0;
}

// *****************************************************************************
//
//	stream_get_dumper
//
// *****************************************************************************
STREAM_DUMPER *stream_get_dumper( int type, int format )
{
DBGS serprintf("stream_get_dumper( type %d  format %d )\r\n", type, format ); 
	STREAM_REG_DUMPER *p = _stream_reg_dumper;
	while( p ) {
		if( type == p->type && format == p->format ) {
			return (STREAM_DUMPER*) p->dumper;
		}
		p = p->next;
	}
	return NULL;
}

static STREAM_REG_DEC_AUDIO *_stream_reg_dec_audio = NULL;

// ************************************************************
//
//	stream_register_dec_audio
//
// ************************************************************
int stream_register_dec_audio_head( STREAM_REG_DEC_AUDIO *reg )
{
	if( !_stream_reg_dec_audio ) {
		_stream_reg_dec_audio = reg;
		reg->next = NULL;
	} else {
		STREAM_REG_DEC_AUDIO *prev = _stream_reg_dec_audio;
		_stream_reg_dec_audio = reg;
		reg->next = prev;
	}
	return 0;
}

int stream_register_dec_audio( STREAM_REG_DEC_AUDIO *reg )
{
	if( !_stream_reg_dec_audio ) {
		_stream_reg_dec_audio = reg;
	} else {
		STREAM_REG_DEC_AUDIO *head = _stream_reg_dec_audio;
		while( head->next ) {
			head = head->next;
		} 
		head->next = reg;
	}
	reg->next = NULL;
	return 0;
}

// ************************************************************
//
//	stream_unregister_dec_audio
//
// ************************************************************
static int _unregister_dec_audio( int format )
{
	STREAM_REG_DEC_AUDIO *now  = _stream_reg_dec_audio;
	STREAM_REG_DEC_AUDIO *prev = NULL;
	while( now ) {
		if( now->format == format ) {
			if( prev ) {
				prev->next = now->next;
			} else {
				_stream_reg_dec_audio = now->next;
			}
		} else {
			prev = now;
		}
		now = now->next;
	}

	return 0;
}

int stream_unregister_dec_audio( int format )
{
	return _unregister_dec_audio( format );
}

// ************************************************************
//
//	stream_get_audio_dec
//
// ************************************************************
static STREAM_DEC_AUDIO *_get_audio_dec( AUDIO_PROPERTIES *audio )
{
DBGS serprintf("stream_get_audio_dec(%s)\r\n", audio_get_format_name(audio) ); 

	STREAM_REG_DEC_AUDIO *a = _stream_reg_dec_audio;
	while( a ) {
		if( a->format == audio->format && a->max_channels >= audio->channels ) {
DBGS serprintf("Trying codec %s\n", a->decoder->name);
			if(!a->decoder->is_supported || a->decoder->is_supported(audio)) {
DBGS serprintf("Using codec %s\n", a->decoder->name);
				return (STREAM_DEC_AUDIO*)a->decoder;
			}
		}
		a = a->next;
	}
	return NULL;
}

STREAM_DEC_AUDIO *stream_get_audio_dec( AUDIO_PROPERTIES *audio )
{
	return _get_audio_dec( audio );
}

STREAM_DEC_AUDIO *audio_get_audio_dec( AUDIO_PROPERTIES *audio )
{
	return _get_audio_dec( audio );
}

static STREAM_REG_DEC_VIDEO *_stream_reg_dec_video = NULL;

// ************************************************************
//
//	stream_register_dec_video
//
// ************************************************************
int stream_register_dec_video( STREAM_REG_DEC_VIDEO *reg )
{
	if( !_stream_reg_dec_video ) {
		_stream_reg_dec_video = reg;
	} else {
		STREAM_REG_DEC_VIDEO *head = _stream_reg_dec_video;
		while( head->next ) {
			head = head->next;
		} 
		head->next = reg;
	}
	reg->next = NULL;
	return 0;
}

// ************************************************************
//
//	stream_unregister_dec_video
//
// ************************************************************
static int _unregister_dec_video( int format, int profile )
{
	STREAM_REG_DEC_VIDEO *now  = _stream_reg_dec_video;
	STREAM_REG_DEC_VIDEO *prev = NULL;
	while( now ) {
		if( now->format == format && (!profile || profile == now->max_profile) ) {
			if( prev ) {
				prev->next = now->next;
			} else {
				_stream_reg_dec_video = now->next;
			}
		} else {
			prev = now;
		}
		now = now->next;
	}

	return 0;
}

int stream_unregister_dec_video( int format )
{
	return _unregister_dec_video( format, 0 );
}

static STREAM_REG_DEC_VIDEO *_get_dec_video( VIDEO_PROPERTIES *video, int cpu, const char *dec_name )
{
	STREAM_REG_DEC_VIDEO *best = NULL;
	STREAM_REG_DEC_VIDEO *any  = NULL;

	STREAM_REG_DEC_VIDEO *v = _stream_reg_dec_video;
	int best_width  = 9999;
	int best_height = 9999;
	while( v ) {
		int sub = v->subfmt;
		if( v->format == video->format && ( sub == VIDEO_SUBFMT_NONE || sub == video->subfmt ) && (v->cpu == cpu) ) {
			int match_profile = v->max_profile ? video->profile <= v->max_profile : 1;
			if( video->width <= v->max_width && video->height <= v->max_height && match_profile && (!dec_name || strcasecmp(dec_name, v->name) == 0)) {
				// matches
				if( v->max_width < best_width && v->max_height < best_height ) {
					// this one is better because it has a smaller max size
					best = v;
					best_width  = v->max_width;
					best_height = v->max_height;
				}
			} else {
				// this one is at least matching the format+subfmt
				any = v;
			}
		}
		v = v->next;
	}
	return best ? best : any;
}

// *****************************************************************************
//
//	stream_get_new_dec_video
//
// *****************************************************************************
STREAM_DEC_VIDEO *stream_get_new_dec_video( VIDEO_PROPERTIES *video, STREAM_VIDEO_MANGLER **mangler, int cpu, int forced, const char *dec_name)
{
DBGS serprintf("stream_get_new_dec_video( %d [%s], %d, %d x %d  cpu %d  forced %d name %s)\r\n", video->format, video_get_format_name(video), video->subfmt, video->width, video->height, cpu, forced , dec_name);
	STREAM_REG_DEC_VIDEO *v = _get_dec_video( video, cpu, dec_name );
	if( v && (forced
#ifdef CONFIG_ANDROID
	          || (android_can_hw_run_dec(cpu) && (
	           ( (cpu!=LIBAV) && ((acodecs_is_supported(video->format, 1, 0) || (device_config_has_pluginlib() && !acodecs_is_supported(video->format, 1, 1) ) ) ) )
		  || ( cpu==LIBAV)))
#else
		  || 1
#endif
	         )
	) {
		if( mangler )
			*mangler = (STREAM_VIDEO_MANGLER*)v->mangler;
		STREAM_DEC_VIDEO *dec = v->new();
		dec->cpu = cpu;
		return dec;
	}
	if( mangler )
		*mangler = NULL;
	
	return NULL;
}

int stream_get_dec_video_res( VIDEO_PROPERTIES *video, int *width, int *height, int cpu, const char *dec_name )
{
	// try any cpu priority until we find a decoder:
	while( cpu ) {
		STREAM_REG_DEC_VIDEO *v = _get_dec_video( video, cpu, dec_name );
		if( v ) {
			if( width )
				*width = v->max_width;
			if( height )
				*height = v->max_height;
			return 0;
		} 
		cpu--;
	}

	if( width )
		*width = 0;
	if( height )
		*height = 0;
	return 1;
}

static STREAM_REG_DEC_SUB *_stream_reg_dec_sub = NULL;

// ************************************************************
//
//	stream_register_dec_sub
//
// ************************************************************
int stream_register_dec_sub( STREAM_REG_DEC_SUB *reg )
{
	if( !_stream_reg_dec_sub ) {
		_stream_reg_dec_sub = reg;
	} else {
		STREAM_REG_DEC_SUB *head = _stream_reg_dec_sub;
		while( head->next ) {
			head = head->next;
		} 
		head->next = reg;
	}
	reg->next = NULL;
	return 0;
}

// *****************************************************************************
//
//	stream_get_new_dec_sub
//
// *****************************************************************************
STREAM_DEC_SUB *stream_get_new_dec_sub( int format )
{
DBGS serprintf("stream_get_sub_dec( %d )\r\n", format ); 

	STREAM_REG_DEC_SUB *s = _stream_reg_dec_sub;
	while( s ) {
		if( s->format == format ) {
			return s->new();
		}
		s = s->next;
	}
	return NULL;
}

static STREAM_REG_IO *_stream_reg_io = NULL;

// ************************************************************
//
//	stream_register_io
//
// ************************************************************
int stream_register_io( STREAM_REG_IO *reg )
{
	if( !_stream_reg_io ) {
		_stream_reg_io = reg;
	} else {
		STREAM_REG_IO *head = _stream_reg_io;
		while( head->next ) {
			head = head->next;
		} 
		head->next = reg;
	}
	reg->next = NULL;
	return 0;
}

// *****************************************************************************
//
//	stream_get_new_io
//
// *****************************************************************************
STREAM_IO *stream_get_new_io( STREAM_URL *src )
{
DBGS serprintf("stream_get_new_io( %s )\r\n", src->url ); 
	STREAM_REG_IO *io = _stream_reg_io;
	while( io ) {
		if( !strncmp( src->url, io->protocol, strlen( io->protocol ) ) ) {
			return io->new( src );
		}
		io = io->next;
	}
	return NULL;
}

// *****************************************************************************
//
//	stream_get_io_nonlocal
//
// *****************************************************************************
int stream_get_io_nonlocal( const char *path )
{
DBGS serprintf("stream_get_io_nonlocal( %s )\r\n", path ); 
	STREAM_REG_IO *io = _stream_reg_io;
	while( io ) {
		if( !strncmp( path, io->protocol, strlen( io->protocol ) ) ) {
			return io->nonlocal;
		}
		io = io->next;
	}
	return 0;
}

// *****************************************************************************
//
//	stream_get_io_etype
//
// *****************************************************************************
int stream_get_io_etype( const char *path )
{
DBGS serprintf("stream_get_io_etype( %s )\r\n", path ); 
	STREAM_REG_IO *io = _stream_reg_io;
	while( io ) {
		if( !strncmp( path, io->protocol, strlen( io->protocol ) ) ) {
			return io->etype;
		}
		io = io->next;
	}
	return 0;
}

#ifdef DEBUG_MSG
// ************************************************************
//
//	_stream_reg_dump
//
// ************************************************************
static void _stream_reg_dump( void )
{
	STREAM_REG_PARSER *p = _stream_reg_parser;
serprintf("\r\nParser:\r\n" );	
	while( p ) {
serprintf("        %-8s  %4d %-8s\r\n", p->parser->name, p->etype, av_get_etype_name( p->etype ) + strlen("ETYPE_") );	
		p = p->next;
	}
/*
	STREAM_REG_DUMPER *d = _stream_reg_dumper;
serprintf("\r\nDumper:\r\n" );	
	while( d ) {
serprintf("        %-10s  %s  %04X %s\r\n", d->dumper->name, d->type == TYPE_VID ? "VIDEO" : "AUDIO", d->format, d->type == TYPE_VID ? video_get_format_name( d->format ) : audio_get_format_name( d->format ) );	
		d = d->next;
	}
*/
	STREAM_REG_DEC_AUDIO *a = _stream_reg_dec_audio;
serprintf("\r\nDecAudio:             fmt               channels\r\n" );	
	while( a ) {
		AUDIO_PROPERTIES audio;
		audio.format = a->format;
serprintf("        %-8s %4X %-16s  %d\r\n", a->decoder->name, a->format, audio_get_format_name( &audio ), a->max_channels );	
		a = a->next;
	}
	STREAM_REG_DEC_VIDEO *v = _stream_reg_dec_video;
serprintf("\r\nDecVideo:         fmt               sub  max       pro  cpu   mangler\r\n" );
	while( v ) {
		VIDEO_PROPERTIES video;
		video.format = v->format;
serprintf("        %-8s  %-16s  %d   %4dx%4d  %3d  %s%d  %-8s\r\n", 
		v->name, 
		video_get_format_name( &video ), v->subfmt,
		v->max_width, v->max_height, v->max_profile,
		(v->cpu <= GPP2) ? "ARM" : "HW ", v->cpu, 
		v->mangler ? v->mangler->name : "---");	
		v = v->next;
	}
	STREAM_REG_DEC_SUB *s = _stream_reg_dec_sub;
serprintf("\r\nDecSub:           fmt\r\n" );
	while( s ) {
		SUB_PROPERTIES sub;
		sub.format = s->format;
serprintf("        %-8s  %-16s\r\n", 
		s->name, 
		sub_get_format_name( &sub ) ); 
		s = s->next;
	}
	STREAM_REG_IO *io = _stream_reg_io;
serprintf("\r\nIO:                           nonlocal  etype       parser\r\n" );	
	while( io ) {
		STREAM_PARSER *p = stream_get_parser( io->etype );
serprintf("        %-20s  %d         %-8s    %s\r\n", io->protocol, io->nonlocal, av_get_etype_name( io->etype ) + strlen("ETYPE_"), p ? p->name : "---"  );	
		io = io->next;
	}
serprintf("\r\n" ); 
}


void _stream_reg_test( void ) 
{
	VIDEO_PROPERTIES v1 = { .format = VIDEO_FORMAT_H264, .width =  720, .height = 400, .subfmt = VIDEO_SUBFMT_NONE };
	VIDEO_PROPERTIES v2 = { .format = VIDEO_FORMAT_H264, .width =  720, .height = 576, .subfmt = VIDEO_SUBFMT_NONE };
	VIDEO_PROPERTIES v3 = { .format = VIDEO_FORMAT_H264, .width = 1280, .height = 400, .subfmt = VIDEO_SUBFMT_NONE };
	VIDEO_PROPERTIES v4 = { .format = VIDEO_FORMAT_H264, .width = 1280, .height = 720, .subfmt = VIDEO_SUBFMT_NONE };

	STREAM_DEC_VIDEO *d;
	d = stream_get_new_dec_video( &v1, NULL, STREAM_CPU_ANY, 0, NULL );
serprintf("%s\r\n", d ? d->name: "---" );
	d = stream_get_new_dec_video( &v2, NULL, STREAM_CPU_ANY, 0, NULL );
serprintf("%s\r\n", d ? d->name: "---" );
	d = stream_get_new_dec_video( &v3, NULL, STREAM_CPU_ANY, 0, NULL );
serprintf("%s\r\n", d ? d->name: "---" );
	d = stream_get_new_dec_video( &v4, NULL, STREAM_CPU_ANY, 0, NULL );
serprintf("%s\r\n", d ? d->name: "---" );
}

void _stream_unreg_dec_video( int argc, char **argv ) 
{
	if( argc > 1 ) {
		int format  = atoi(argv[1]);
		int profile = (argc > 2) ? atoi(argv[2]) : 0;
		_unregister_dec_video( format, profile );
	}
}

void _stream_unreg_dec_audio( int argc, char **argv ) 
{
	if( argc > 1 ) {
		int format  = atoi(argv[1]);
		_unregister_dec_audio( format );
	}
}

void _stream_unreg_parser( int argc, char **argv ) 
{
	if( argc > 1 ) {
		int etype = atoi( argv[1] );
		stream_unregister_parser( etype );
	}
}

DECLARE_DEBUG_COMMAND_VOID( "sreg",  _stream_reg_dump );
DECLARE_DEBUG_COMMAND_VOID( "sregt", _stream_reg_test );
DECLARE_DEBUG_COMMAND_VOID( "surv",  _stream_unreg_dec_video );
DECLARE_DEBUG_COMMAND_VOID( "sura",  _stream_unreg_dec_audio );
DECLARE_DEBUG_COMMAND_VOID( "surp",  _stream_unreg_parser );

#endif
#endif

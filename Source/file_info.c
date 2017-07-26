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

#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>           /* For O_* constants */
#include <sys/select.h>

#include "global.h"
#include "types.h"
#include "debug.h"
#include "browse.h"
#include "file_info.h"
#include "file.h"
#include "util.h"
#include "app_av.h"
#include "subtitle_format.h"
#include "file_type.h"
#include "stream.h"

#define ERR	if(1)
#define DBGP	if(Debug[DBG_PARSER])
#define DBG	if(Debug[DBG_PARSER] > 1)

#define DBGGFI	DBG

// ************************************************
//
//	clear_info
//
// ************************************************
void clear_info( FILE_INFO *info )
{
	memset( info, 0, sizeof( FILE_INFO ) );
	av_init_props( info );
#ifndef CONFIG_ANDROID
	// set unknown genre
	strcpy( info->id3_tag.genre, "Unknown" );
#endif
}

// ************************************************
//
//	__get_info_generic
//
// ************************************************
static int _get_info_generic( const char *full_path, FILE_INFO *info, APIC *apic, FILE_INFO_ABORT abort )
{
	STAT st;

	clear_info( info );
	if( file_stat( full_path, &st ) ) {
		return 1;
	}
	info->size = st.st_size;

	return 0;
}

static FILE_INFO_REG *_fi_reg = NULL;

// ************************************************************
//
//	file_info_register
//
// ************************************************************
void file_info_register( FILE_INFO_REG *reg )
{
	if( !_fi_reg ) {
		_fi_reg = reg;
	} else {
		FILE_INFO_REG *head = _fi_reg;
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
//	file_info_unregister
//
// ************************************************************
int file_info_unregister( int type, int etype )
{
	FILE_INFO_REG *now = _fi_reg;
	FILE_INFO_REG *prev = NULL;
	while( now ) {
		if( now->type == type && (now->etype == ETYPE_ANY || now->etype == etype) ) {
			if( prev ) {
				prev->next = now->next;
			} else {
				_fi_reg = now->next;
			}
		} else {
			prev = now;
		}
		now = now->next;
	}

	return 0;
}

// ************************************************************
//
//	_fi_get_fn
//
// ************************************************************
static int _fi_get_fn( int type, int etype, FILE_INFO_PATH *info_path, FILE_INFO_IO *info_io, FILE_INFO_MMAP *info_mmap )
{
	if( info_path ) {
		*info_path = NULL;
	}
	if( info_io ) {
		*info_io = NULL;
	}
	if( info_mmap ) {
		*info_mmap = NULL;
	}
	
	FILE_INFO_REG *reg = _fi_reg;
	while( reg ) {
		if( reg->type == type && (reg->etype == ETYPE_ANY || reg->etype == etype) ) {
			if( reg->info_path && info_path ) {
				*info_path = reg->info_path;
				return 0;
			} else if( reg->info_io && info_io ) {
				*info_io = reg->info_io;
				return 0;
			} else if( reg->info_mmap && info_mmap ) {
				*info_mmap = reg->info_mmap;
				return 0;
			}
			break;
		} 
		reg = reg->next;
	}

	return 1;
}

#ifdef DEBUG_MSG
static void _fi_dump_reg( void )
{
	FILE_INFO_REG *reg = _fi_reg;
serprintf("\r\nFileInfo:\r\n" );	
	while( reg ) {
serprintf("\t%-8s  %-16s  %s: %s\r\n", av_get_type_name(reg->type), av_get_etype_name(reg->etype), 
					reg->info_path ? "PATH" :(
					reg->info_io   ? "IO  " :( 
					reg->info_mmap ? "MMAP" : 
					                 "")),
					reg->info_path ? reg->info_path_name :( 
					reg->info_io   ? reg->info_io_name   :( 
					reg->info_mmap ? reg->info_mmap_name :
					                 "")));	
		reg = reg->next;
	}
}

DECLARE_DEBUG_COMMAND_VOID( "fireg", _fi_dump_reg );
#endif

// ************************************************
//
//	get_info_io
//
// ************************************************
static int get_info_io( STREAM_URL *src, FILE_INFO *info, APIC *apic, FILE_INFO_ABORT abort, FILE_INFO_IO info_io )
{
	int err = 1;

DBGP serprintf("%s:\n", __FUNCTION__);
	if( !src || !info || !info_io ) {
		return 1;
	}
	STREAM_IO *io = stream_get_new_io( src );
	if( !io ) {
ERR serprintf("cannot create io for: %s\n", src->url );
		return 1;
	}
	
	if( io->open( io, O_RDONLY ) ) {
ERR serprintf("cannot open: %s %s\n", src->url, strerror(errno) );
		goto ErrorExit;
	}		

	strnZcpy( info->full_path, src->url, MAX_NAME_LEN );
	
	err = info_io( io, info, apic, abort );	
		
ErrorExit:
	if( io ) {
		if( io->is_open( io ) ) {
			io->close( io );
		}
		io->delete( io );
	}
	return err;

}

// ************************************************
//
//	get_info_mmap
//
// ************************************************
static int UNUSED get_info_mmap( const char *full_path, FILE_INFO *info, APIC *apic, FILE_INFO_ABORT abort, FILE_INFO_MMAP info_mmap )
{
DBGP serprintf("%s:\r\n", __FUNCTION__);
	if( !full_path || !info || !info_mmap ) {
		return 1;
	}

	int fd;
	if( (fd = file_open( full_path, O_RDONLY, 0 )) == -1 ) {
ERR serprintf("cannot open: %s %s\r\n", full_path, strerror(errno) );
		return 1;
	}

	STAT st;
	if( file_fstat( fd, &st ) ) {
		file_close( fd );
		return 1;
	}

	info->size = st.st_size;
	strnZcpy( info->full_path, full_path, MAX_NAME_LEN );
	
	if ( !st.st_size ) {
		file_close( fd );
		return 1;
	}
		
	UINT64 size = MIN( 1024 * 1024 * 1024, st.st_size );
	UCHAR *buffer = mmap( 0, size, PROT_READ, MAP_SHARED, fd, 0 );	

	if( buffer == MAP_FAILED ) {
ERR serprintf("mmap failed for: size %lld, file %s\r\n", size, full_path);
		file_close( fd );
		return 1;
	}
	int err = info_mmap( buffer, size, info, apic, abort );	

	if( buffer )
		munmap( buffer, size );
		
	file_close( fd );

	return err;
}

// ************************************************
//
//	get_info_mmap_io
//
// ************************************************
static int get_info_mmap_io( STREAM_URL *src, FILE_INFO *info, APIC *apic, FILE_INFO_ABORT abort, FILE_INFO_MMAP info_mmap )
{
	int err = 1;

DBGP serprintf("%s:\n", __FUNCTION__);
	if( !src || !info || !info_mmap ) {
		return 1;
	}
	
	STREAM_IO *io = stream_get_new_io( src );
	if( !io ) {
ERR serprintf("cannot create io for: %s\n", src->url );
		return 1;
	}
	
	if( !io->mmap ) {
ERR serprintf("io cannot mmap for: %s\n", src->url );
		return 1;
	}

	if( io->open( io, O_RDONLY ) ) {
ERR serprintf("cannot open: %s %s\n", src->url, strerror(errno) );
		goto ErrorExit;
	}		

	info->size = io->get_size( io );
	strnZcpy( info->full_path, src->url, MAX_NAME_LEN );
	
	if ( !info->size ) {
		goto ErrorExit;
	}
		
	UINT64 size = MIN( 1024 * 1024 * 1024, info->size  );
	UCHAR *buffer = io->mmap( io, size, 0 );	

	if( buffer == MAP_FAILED ) {
ERR serprintf("io->mmap failed for: size %lld, file %s\n", size, src->url);
		goto ErrorExit;
	}
	
	err = info_mmap( buffer, size, info, apic, abort );	

	if( buffer && io->munmap ) {
		io->munmap( io, buffer, size );
	}
		
ErrorExit:
	if( io ) {
		if( io->is_open( io ) ) {
			io->close( io );
		}
		io->delete( io );
	}
	return err;
}

// ************************************************
//
//	get_info_subtitle
//
// ************************************************
static int get_info_subtitle(const char *full_path, FILE_INFO *info)
{
	const char *path[2] = { full_path, NULL };
	
	subtitle_files *sub_info = subtitle_check_files(path, cut_path(full_path));
	if (sub_info) {
		converted_subs *sub_conv = subtitle_get_converted( sub_info, 0 );
		if (sub_conv) {
			int i;
			struct subt_orig_t *sub_files = sub_info->files;

			for (i = 0; i < sub_conv->cnt; ++i) {
				SUB_PROPERTIES *sub = &info->av.sub[i+info->av.subs_max];
				sub->format = SUB_FORMAT_EXT;
				sub->gfx = 0;
				sub->ext = 1;
				strnZcpy(sub->name, sub_conv->converted[i]->identifier, AV_NAME_LEN);

				if (sub_files) {
					if (sub_files->org_name) {
						strnZcpy(sub->path, sub_files->org_name, MAX_NAME_LEN);
					}
					sub_files = sub_files->next;
				}
			}
			info->av.subs_max += sub_info->count;
			subtitle_free_converted(sub_conv);
		}
		subtitle_free_files(sub_info);
	}
	return 0;
}

// ************************************************
//
//	get_url_info
//
// ************************************************
int get_url_info( STREAM_URL *src, int type, int etype, FILE_INFO *info, APIC *apic, FILE_INFO_ABORT abort )
{
DBG serprintf("get_url_info: %s %d/%d\r\n", src->url, type, etype );

	clear_info( info );

	info->type  = type;
	info->etype = etype;
	
	// if this is a directory, nothing to do
	if(type == TYPE_DIR){
		return 0;
	}

	FILE_INFO_PATH info_path;
	FILE_INFO_IO   info_io;
	FILE_INFO_MMAP info_mmap;
	int err = 0;
	
	if( _fi_get_fn( type, etype, &info_path, &info_io, &info_mmap ) ) {
		// no info_path or info_mmap, use generic:
DBG serprintf("generic: %s\r\n", src->url);
		err = _get_info_generic( src->url, info, apic, abort );
	} else if( info_path ) {
DBG serprintf("path: %s\r\n", src->url);
		err = info_path( src->url, info, apic, abort );
	} else if( info_io ) {
DBG serprintf("io: %s\r\n", src->url);
		err = get_info_io( src, info, apic, abort, info_io );
	} else if( info_mmap ) {
DBG serprintf("mmap: %s\r\n", src->url);
		err = get_info_mmap_io( src, info, apic, abort, info_mmap );
	}

	if (type == TYPE_VID) {
		get_info_subtitle( src->url, info );
	}
	
if( err ) {
//ERR serprintf("get_url_info ERR: %s\r\n", src->url );
}
	if( !err && type == TYPE_VID ) {
		// special case for video, update some fields that some info functions do not set:
		info->video->format = video_get_format_from_fourcc( info->video->fourcc, &info->video->subfmt );

		if ( info->video->scale && info->video->rate ) {
			info->video->framesPerSec = (UINT64)info->video->rate / (UINT64)info->video->scale;
		} 
	}

	return err;
}

// ************************************************
//
//	get_file_info_clean
//
// ************************************************
int get_file_info_clean( const char *path, int type, int etype, FILE_INFO *info, APIC *apic, FILE_INFO_ABORT abort )
{
	STREAM_URL src;
	stream_url_cpy_url( &src, path);
	return get_url_info( &src, type, etype, info, apic, abort );
}

// ************************************************
//
//	get_file_info
//
//	the original one, with dirty VideoApp GUI dependance inside (TM)
//
// ************************************************
int get_file_info( const char *path, int type, int etype, FILE_INFO *info, APIC *apic, FILE_INFO_ABORT abort )
{
DBG serprintf("get_file_info: %s %d/%d\r\n", path, type, etype );

	clear_info( info );

	info->type  = type;
	info->etype = etype;

	STREAM *s;
	if( AV_get_state() == AV_PLAYING && AV_get_type() == TYPE_VID && ( s = AV_get_ctx() )
#ifdef CONFIG_VIDEOBROWSER
		&& OBJ_IS( VideoPlayerScreen, Screen_find( VIDEOPLAYER_SCREEN_NAME ) )
#endif
	) {
		// shortcut for playing video, get the info from the current stream
		info->size           = s->size;
		info->duration       = s->duration;
		info->video->crypted = s->video->crypted;
		info->id3_tag        = s->tag;

		memcpy( &info->av, &s->av, sizeof( AV_PROPERTIES ) );
DBGP serprintf("video %d/%d  audio %d/%d\r\n", info->av.vs_max, info->video->valid, info->av.as_max, info->audio->valid);

		return 0;
	}

	return get_file_info_clean(path, type, etype, info, apic, abort);
}

void file_info_dump( FILE_INFO *info, APIC *apic ) 
{
	show_short_av_props( &info->av );
	
	if( info->id3_tag.valid ) {
		serprintf("tags\n" );
		serprintf("\tartist: %s\n", info->id3_tag.artist );
		serprintf("\talbum:  %s\n", info->id3_tag.album  );
		serprintf("\ttitle:  %s\n", info->id3_tag.title  );
	}
	if( apic && apic->valid ) {
		serprintf("\tAPIC:   %s  size %d\n", av_get_etype_name( apic->etype), apic->size );	
	}
}

void file_info_dump_for_path( const char *path, int verbose )
{
	int type;
	int etype;
		
	if( get_file_type( path, &type, &etype ) ) {
//serprintf("cannot get type/etype\r\n");
		return;
	} 

	FILE_INFO info;
	APIC apic = { 0 };
	apic.buffer_size = 512 * 1024;
		
	if( get_file_info_clean( path, type, etype, &info, &apic, NULL) ){
serprintf("cannot get info: %s\r\n", path);
		return;
	}
	
	if( verbose ) {
		file_info_dump( &info, &apic );
	} else {
serprintf("\t%-80s  %s  %s\n", cut_path( path ), info.id3_tag.valid ? "TAG" : "   ", apic.valid ? av_get_etype_name( apic.etype) : "" );	
	}
	
	if( apic.buffer ) {
		afree( apic.buffer );		
	}
}


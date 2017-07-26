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
#include "stream.h"
#include "debug.h"
#include "file.h"
#include "astdlib.h"
#include "util.h"

#include <string.h>
#include <errno.h>
#include <sys/mman.h>

#ifdef CONFIG_STREAM

#define DBGS if(Debug[DBG_STREAM])

#define DBGERR if( 1 )

#define FILE_LOCALHOST "file://localhost/"

extern volatile int stream_io_fail;
extern volatile int stream_io_hang;

typedef struct s_FILE_PRIV {
	int		fd;
	int		mode;
	int		o_direct;
	int		progressive;	// the file is growing as we read it
	UINT64		real_size;
} FILE_PRIV;

static int _open( STREAM_IO *io, int mode )
{
	FILE_PRIV *priv = (FILE_PRIV *)io->priv;

DBGS serprintf("stream_io_file_open: %s\r\n", io->src.url);

	if( priv->fd < 0 ) {
		const char *name = io->src.url;
		// open the file
		if( name == strstr( name, FILE_LOCALHOST ) ) {
			name += strlen( FILE_LOCALHOST ) - 1;
			strcpy( io->src.url, name );
		}

		priv->mode = mode;
		
#ifndef SIM
		if( priv->mode & O_DIRECT ) {
			priv->o_direct = 1;
serprintf("io_file: use O_DIRECT!\r\n" );
		}
RETRY:
#endif		
		if( (priv->fd = file_open( io->src.url, priv->mode, 0600 )) == -1 ) {
serprintf("stream_io_file_open ERR: %s\r\n", strerror( errno ) );
#ifndef SIM
			if( priv->o_direct ) {
serprintf("try without O_DIRECT!\r\n" );
				// failed to open, might be because of O_DIRECT, try without!
				priv->o_direct = 0;
				priv->mode &= ~O_DIRECT;
				goto RETRY;
			}
#endif		
			return 1;		
		}
  		
		io->_is_open = 1;
		io->pos = 0;
		// get the size, we cannot read beyond it
		io->size = file_get_total_size( io->src.url, &priv->progressive );
     	 	
		if( priv->progressive ) {	
			// get the real size, for progressive playback
			priv->real_size = file_get_real_size( io->src.url );
			if( priv->real_size == -1 ){
				// Glups, make as if it was a normal file
				priv->real_size = io->size;
			}
		} 
		
		if( !io->num_parts )
			io->num_parts = 1;
			
		io->now_part  = 0;
		io->now_start = 0;
		if( io->num_parts > 1 ) {
			io->now_size  = io->parts[io->now_part].real_size;
		} else {
			io->now_size  = io->size;
		}
		return 0;
	} 
serprintf("err, already open! %s %d \r\n", io->src.url, priv->fd );
	return 1;
}

static int _is_open( STREAM_IO *io )
{
	return io ? io->_is_open : 0;
}

static int _close( STREAM_IO *io )
{
	FILE_PRIV *priv = (FILE_PRIV *)io->priv;

DBGS serprintf("stream_io_file_close\r\n");

	if( !io->_is_open ) {
serprintf("io not open!\r\n");
		return 1;
	}
	file_close( priv->fd );
	priv->o_direct = 0;
	priv->fd       = -1;
	io->_is_open = 0;
	return 0;
}

static void *_mmap( STREAM_IO *src, UINT64 size, UINT64 offset )
{
	if( !src->_is_open ) {
		return MAP_FAILED;
	}	
	
	FILE_PRIV *priv = (FILE_PRIV *)src->priv;
	
	return mmap( 0, size, PROT_READ, MAP_SHARED, priv->fd, offset );	
}

static int _munmap( STREAM_IO *src, void *buf, UINT64 size )
{
	return munmap( buf, size );
}


#define DBGM if(0)

static UINT64 _seek_multi( STREAM_IO *io, UINT64 pos)
{
	UINT64 	ret;
	UINT64 	offset;
	int 	part;
	UINT64 	start = 0;
	FILE_PRIV *priv = (FILE_PRIV *)io->priv;
	
DBGM serprintf("seek_multi: %lld", pos );
	for( part = 0; part < io->num_parts; part++ ) {
		if ( pos < start + io->parts[part].pad_size )
			break;
		start += io->parts[part].pad_size;
	}
	if( part == io->num_parts ) {
DBGM serprintf("  at end\r\n");
		return io->pos;
	}
		
DBGM serprintf("  in part: %d  start %lld", part, start );

	if( part != io->now_part ) {
		// we need to open another file
		char file[MAX_NAME_LEN + 1];
		io->get_part_name( file, io->src.url, part );
		
		file_close( priv->fd );
		priv->fd = file_open( file, priv->mode, 0600 );
		io->now_part  = part;
		io->now_start = start;
		io->now_size  = io->parts[part].real_size;
DBGM serprintf("  open new: %s  fd %d  size %d", file, priv->fd, io->now_size );		
	}
DBGM serprintf("\r\n" );		
	
	offset = pos - start;
	
	ret = file_seek( priv->fd, offset, SEEK_SET );
DBGERR {
	if( ret != offset ) {
serprintf("stream_io_file_seek ERR pos %lld  ofs %d  ret %lld\r\n", pos, offset, ret );	
	}
	io->pos = pos;
	if( io->pos > io->size )
		io->size = io->pos;
}
	return start + ret;
}

static inline int pad_to_chunk( STREAM_IO *io, int size )
{
	if( !io->chunk_size ) {
		return size;
	}
	return ((size + io->chunk_size - 1 ) / io->chunk_size) * io->chunk_size;
}

static int _read_multi( STREAM_IO *io, UCHAR *buffer, int count)
{
	UINT64 ret = 0;
	FILE_PRIV *priv = (FILE_PRIV *)io->priv;
	
DBGM serprintf("__");
	while( count > 0 ) {
		UINT32 rest     = io->now_start + io->now_size - io->pos;
		UINT32 pad_rest = io->now_start + pad_to_chunk( io, io->now_size ) - io->pos;
		UINT32 to_read  = MIN( pad_rest, count );
		
DBGM serprintf("[count %d  pos %lld  rest %u/%u  tr %u]", count, io->pos, rest, pad_rest, to_read ); 		
		if( rest <= 0 ) {
DBGM serprintf("done\r\n"); 			
			// nothing to read any more, give up 
			break;
		}

		// do not read over end of file
		UINT32 aligned = to_read;
		if( priv->o_direct ) {
			// we have to read in 512 multiples!
			aligned = (aligned + 511) / 512 * 512;
DBGM serprintf("(al %d)", aligned );
		}
		while( stream_io_hang ) {
serprintf("ioh ");
			msec_sleep( 100 );
		}
		
		UINT32 bytes = file_read( priv->fd, buffer, aligned );
DBGM serprintf("(%u)", bytes );
		if( to_read - bytes > 0 ) {
			// zero fill the padding area!
serprintf("FILL_ZERO: pos %8lld  rest %d\r\n", io->pos, to_read - bytes );		
			memset( buffer + bytes, 0, to_read - bytes );
		} 
		ret     += to_read;	// NOT += bytes! (we want to pad the files, so NO error when reading over eof!)
		count   -= to_read;
		buffer  += to_read;
		io->pos += to_read;
		if( to_read == pad_rest ) {
DBGM serprintf("seek to %lld", io->pos);		
			_seek_multi( io, io->pos );
		}
	}
DBGM serprintf("[ret %d]", ret); 			
	return ret;
}

static UINT64 _seek( STREAM_IO *io, UINT64 pos)
{
	UINT64 ret;
	FILE_PRIV *priv = (FILE_PRIV *)io->priv;
	
	if( io->num_parts > 1 )
		return _seek_multi( io, pos );
		
	ret = file_seek( priv->fd, pos, SEEK_SET );
DBGERR {
	if( ret != pos ) {
serprintf("stream_io_file_seek ERR pos %lld  ret %lld\r\n", pos, ret );	
	}
	io->pos = ret;
	if( io->pos > io->size )
		io->size = io->pos;
}
	return ret;
}

static int _read( STREAM_IO *io, UCHAR *buffer, UINT count)
{
	UCHAR *p;
	int to_read, aligned, len, ret;
	FILE_PRIV *priv = (FILE_PRIV *)io->priv;
	
	if( stream_io_fail ) {
		stream_io_fail = 0;
		return -1;
	}
	
	// do not read over end of file
	to_read = MIN( count, io->size - io->pos);
	if( to_read < 0 )
		return 0; 

	if( io->num_parts > 1 )
		return _read_multi( io, buffer, count );
		
	ret = to_read;
	p = buffer;
	while( to_read > 0 ) {
if( to_read == 0 ) {
DBGS serprintf("(tr %d  %d %d %d)", to_read, count, io->size, io->pos );
}
		aligned = to_read;
		if( priv->o_direct ) {
			// we have to read in 512 multiples!
			aligned = (aligned + 511) / 512 * 512;
		}
		while( stream_io_hang ) {
			serprintf("ioh ");
			msec_sleep( 100 );
		}
		len = file_read( priv->fd, p, aligned );
		if (len == -1) {
			if (errno == EINTR && (!io->abort || io->abort( io->abort_ctx ) == STREAM_BUFFER_NO_ABORT)) {
				// signal but no abort
				continue;
			}
			// user abort
			return -1;
		} else if ( !len ) {
			// we read nothing, bail out
			return -1;
		}
		to_read -= len;
		io->pos += len;
		p += len;
	}
	return ret;
}

static int _write( STREAM_IO *io, UCHAR *buffer, UINT count)
{
	FILE_PRIV *priv = (FILE_PRIV *)io->priv;
	int ret = file_write( priv->fd, buffer, count );

DBGERR {
	if( ret < count ) {
serprintf("stream_io_file_write ERR count %d  ret %d  pos %d\r\n", count, ret, io->pos );	
	}
	io->pos += ret;
	if( io->pos > io->size )
		io->size = io->pos;
}
	return ret;
}

static int _power_state( STREAM_IO *io )
{
	return 1;
}

static int _seekable( STREAM_IO *io )
{
	return 1;
}

static int _sleepable( STREAM_IO *io, int *sleep_time, int *wake_time )
{
	if ( sleep_time )
		*sleep_time = 0;
	if ( wake_time )
		*wake_time = 0;
	return 1;
}

static int _delete( STREAM_IO *io )
{
	if( io && io->priv )
		afree( io->priv );

	if( io )
		afree( io );
	return 0;
}

static int _can_read( STREAM_IO *io, UINT64 pos, UINT64 count )
{
	FILE_PRIV *priv = (FILE_PRIV *)io->priv;

	if( io->num_parts > 1 ){
		// Assume multipart files are not going to support progressive playback
		return 1;
	}

	if( priv->progressive ) {
		// normal file, use the size
		if( pos + count <= io->size ){
			return 1;
		}
	} else {
		// progressive
		if( pos + count <= priv->real_size ){
			return 1;
		}
	
		off64_t real_size = file_get_real_size( io->src.url );

		if ( real_size == -1 ){
			// assume we can read...
			return 1;
		}

		priv->real_size = real_size;

		if( pos + count <= priv->real_size ){
			return 1;
		}
	}

//serprintf("pos %llu count %llu size %llu\r\n", pos, count, io->size);
	return 0;
}

static STREAM_IO *_new( STREAM_URL *src ) 
{
	STREAM_IO *io = stream_io_new( src );
	if( !io )
		return NULL;
	
	io->delete	= _delete,
	io->open        = _open;
	io->is_open     = _is_open;
	io->close       = _close;
	io->get_size    = stream_io_get_size;
	io->seek        = _seek;
	io->read        = _read;
	io->write       = _write;
	io->power_state = _power_state;
	io->seekable    = _seekable;
	io->sleepable   = _sleepable;
	io->can_read	= _can_read;
	io->mmap	= _mmap;
	io->munmap	= _munmap;

	// allocate private data
	if( !(io->priv = amalloc( sizeof( FILE_PRIV ) ) ) ) {
		afree( io );
		return NULL;
	}
	memset( io->priv, 0, sizeof( FILE_PRIV ) );
	((FILE_PRIV *)io->priv)->fd = -1;

	return io;
}

static char proto[] = "/";
STREAM_REGISTER_IO( proto, _new, STREAM_IO_LOCAL, ETYPE_NONE );
static char proto2[] = FILE_LOCALHOST;
STREAM_REGISTER_IO( proto2, _new, STREAM_IO_LOCAL, ETYPE_NONE );
#endif

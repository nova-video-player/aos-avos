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

#include <limits.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>

#ifdef CONFIG_STREAM

#define DBGS if(Debug[DBG_STREAM])

#define DBGERR if( 1 )

extern volatile int stream_io_fail;
extern volatile int stream_io_hang;

typedef struct s_FILE_PRIV {
	int	fd;
	int64_t	offset;
} FILE_PRIV;

static int _open( STREAM_IO *io, int mode )
{
	FILE_PRIV *priv = (FILE_PRIV *)io->priv;

DBGS serprintf("stream_io_file_open: %s\r\n", io->src.url);
	if (mode != O_RDONLY) {
		serprintf("err, only O_RDONLY supported\n");
		return 1;
	}

	if( priv->fd < 0 ) {
		const char *end, *p;
		char *endptr;
		int oldfd;
		long int val;

		// url format: "fd://<fd>:<offset>:<length>
		serprintf("stream_io_file_open:: %s\n", io->src.url);
		end = io->src.url + strlen(io->src.url);
		errno = 0;
		p = io->src.url + strlen("fd://");
		val = strtol(p, &endptr, 10);
		if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN))
		    || (errno != 0 && val == 0) || endptr > end)
			return 1;
		oldfd = val;
		p = endptr + 1;
		val = strtol(p, &endptr, 10);
		if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN))
		    || (errno != 0 && val == 0) || endptr > end)
			return 1;
		priv->offset = val;
		p = endptr + 1;
		val = strtol(p, NULL, 10);
		if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN))
		    || (errno != 0 && val == 0))
			return 1;
		io->size = val;

		priv->fd = dup(oldfd);
		if (lseek64(priv->fd, priv->offset, SEEK_SET) != priv->offset) {
			serprintf("file_open %s: lseek failed\n", io->src.url);
			close(priv->fd);
			priv->fd = -1;
			return 1;
		}

		io->_is_open = 1;
		io->pos = 0;
		io->num_parts = 0;
     	 	
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
	close( priv->fd );
	priv->fd = -1;
	priv->offset = 0;
	io->_is_open = 0;
	return 0;
}

static void *_mmap( STREAM_IO *src, UINT64 size, UINT64 offset )
{
	if( !src->_is_open ) {
		return MAP_FAILED;
	}	
	
	FILE_PRIV *priv = (FILE_PRIV *)src->priv;
	
	return mmap( 0, size, PROT_READ, MAP_SHARED, priv->fd, offset + priv->offset );	
}

static int _munmap( STREAM_IO *src, void *buf, UINT64 size )
{
	return munmap( buf, size );
}


#define DBGM if(0)

static UINT64 _seek( STREAM_IO *io, UINT64 pos)
{
	UINT64 ret;
	FILE_PRIV *priv = (FILE_PRIV *)io->priv;
	
	ret = file_seek( priv->fd, priv->offset + pos, SEEK_SET ) -  priv->offset;
	if( ret != pos ) {
serprintf("stream_io_file_seek ERR pos %lld  ret %lld\r\n", pos, ret );	
	}
	io->pos = ret;
	if( io->pos > io->size ) {
		serprintf("stream_io_file_seek: io->pos > io->size: should not happen\n");
		return -1;
	}
	return ret;
}

static int _read( STREAM_IO *io, UCHAR *buffer, UINT count)
{
	UCHAR *p;
	int to_read, len, ret;
	FILE_PRIV *priv = (FILE_PRIV *)io->priv;
	
	if( stream_io_fail ) {
		stream_io_fail = 0;
		return -1;
	}
	
	// do not read over end of file
	to_read = MIN( count, io->size - io->pos);
	if( to_read < 0 )
		return 0; 

	ret = to_read;
	p = buffer;
	while( to_read > 0) {
		while( stream_io_hang ) {
	serprintf("ioh ");
			msec_sleep( 100 );
		}
		len = file_read( priv->fd, p, to_read );
		if (len == -1) {
			if (errno == EINTR && (!io->abort || io->abort( io->abort_ctx ) == STREAM_BUFFER_NO_ABORT)) {
				// signal but no abort
				continue;
			}
			// user abort
			return -1;
		}
		to_read -= len;
		io->pos += len;
		p += len;
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
	// normal file, use the size
	if( pos + count <= io->size ){
		return 1;
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
	io->write       = NULL;
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

static char proto[] = "fd://";
STREAM_REGISTER_IO( proto, _new, STREAM_IO_LOCAL, ETYPE_NONE );

#endif

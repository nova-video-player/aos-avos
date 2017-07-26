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

#include "types.h"
#include "global.h"
#include "stream.h"
#include "debug.h"
#include "astdlib.h"
#include "file.h"

#include <stdio.h>
#include <string.h>

#define DBGP	if(Debug[DBG_PARSER])
#define DBGPP	if(Debug[DBG_PARSER] > 1)
#define DBGP3	if(Debug[DBG_PARSER] > 2)
#define DBGS	if(Debug[DBG_STREAM])
#define DBGV	if(Debug[DBG_VID])

#ifdef CONFIG_VIDEO

static int fd = -1;
static int frames;

// ***************************************************************************
//
//	__open
//
// ***************************************************************************
static int __open( STREAM *s, const char *ext )
{
	if( !s )
		return 1;
	
	char path[STREAM_MAX_PATH_LEN + 1];
	if( !s->src.url[0] || s->src.url[0] != '/' ) {
		strcpy( path, HDD_ROOT"video" );
	} else {
		strcpy( path, s->src.url );
	}
	strcat( path, "_dump" );
	strcat( path, ext );
DBGS serprintf("stream_dumper_open: ext %s -> %s\r\n", ext, path );
	
	fd = file_open( path, O_WRONLY | O_CREAT | O_TRUNC, 0600 );
	if( fd == -1 ) {
serprintf("stream_dumper_open: cannot open: %s\r\n", path );
		return 1;
	}
	
	return 0;
}

static int _open_raw( STREAM *s, int buffer_size, int flags )
{
	return __open( s, ".raw" );
}

static int _open_mpeg2( STREAM *s, int buffer_size, int flags )
{
	return __open( s, ".m2v" );
}

static int _open_mpeg4( STREAM *s, int buffer_size, int flags )
{
	return __open( s, ".mpg4" );
}

static int _open_h264( STREAM *s, int buffer_size, int flags )
{
	return __open( s, ".h264" );
}

typedef struct __PACKED__ RCV_V1_HEADER {
	UCHAR	frames[3];
	UCHAR	flags;
	UINT	ext_len;
	UCHAR	extra[4];
	UINT	height;
	UINT	width;
	
} RCV_V1_HEADER;

static int _open_rcv( STREAM *s, int buffer_size, int flags )
{
	if( __open( s, ".rcv" ) ) {
		return 1;
	}
	
	RCV_V1_HEADER rcv = { 0 };
	
	rcv.flags   = 0x85;
	rcv.ext_len = 4;
	memcpy( rcv.extra, s->video->extraData, 4 );
	rcv.height  = s->video->height;
	rcv.width   = s->video->width;
	
	file_write( fd, &rcv, 20 );
	
	frames = 0;
	
	return 0;
}

// ***************************************************************************
//
//	_close
//
// ***************************************************************************
static int _close( STREAM *s )
{
DBGS serprintf("stream_dumper_close_RAW\r\n" );
	if( !s )
		return 1;
		
	if( fd != -1 ) {
		file_close( fd );
		fd = -1;
	}
	
	return 0;
}

static int _close_rcv( STREAM *s )
{
DBGS serprintf("stream_dumper_close_RCV\r\n" );
	if( !s || fd == -1 )
		return 1;
		
	file_seek( fd, 0, SEEK_SET );
	
	char f[4];
	f[0] = (frames       ) & 0xFF;
	f[1] = (frames >>  8 ) & 0xFF;
	f[2] = (frames >> 16 ) & 0xFF;

	file_write( fd, f, 3 );
	
	_close( s );
	
	return 0;
}

// ***************************************************************************
//
//	_write
//
// ***************************************************************************
static int _write( STREAM *s, UCHAR *data, STREAM_CDATA *cdata )
{
	file_write( fd, data, cdata->size );
	return 0;
}

// ***************************************************************************
//
//	_write_h264
//
// ***************************************************************************
static int _write_h264( STREAM *s, UCHAR *data, STREAM_CDATA *cdata )
{
	char AUD[] = { 0x00, 0x00, 0x00, 0x01, 0x09, 0x30 };
	if( memcmp( data, AUD, 5 ) ) {
		// no AUD, add one
		file_write( fd, AUD, 6 );
	}
	file_write( fd, data, cdata->size );
	return 0;
}

// ***************************************************************************
//
//	_write_rcv
//
// ***************************************************************************
static int _write_rcv( STREAM *s, UCHAR *data, STREAM_CDATA *cdata )
{
	char size[4];
	size[0] = (cdata->size      ) & 0xFF;
	size[1] = (cdata->size >> 8 ) & 0xFF;
	size[2] = (cdata->size >> 16) & 0xFF;
	size[3] = (cdata->size >> 24) & 0xFF;

	file_write( fd, size, 4 );
	
	file_write( fd, data, cdata->size );

	frames ++;
	return 0;
}

static STREAM_DUMPER stream_dumper_RAW = {
	"RAW",
	_open_raw,
	_close,
	_write,
};
 
static STREAM_DUMPER stream_dumper_MPEG2_RAW = {
	"MPEG2_RAW",
	_open_mpeg2,
	_close,
	_write,
};

static STREAM_DUMPER stream_dumper_MPEG4_RAW = {
	"MPEG4_RAW",
	_open_mpeg4,
	_close,
	_write,
};

static STREAM_DUMPER stream_dumper_H264_RAW = {
	"H264_RAW",
	_open_h264,
	_close,
	_write_h264,
};

static STREAM_DUMPER stream_dumper_RCV = {
	"RCV",
	_open_rcv,
	_close_rcv,
	_write_rcv,
};

STREAM_REGISTER_DUMPER( TYPE_VID, VIDEO_FORMAT_UNKNOWN, stream_dumper_RAW );
STREAM_REGISTER_DUMPER( TYPE_VID, VIDEO_FORMAT_MPEG,    stream_dumper_MPEG2_RAW );
STREAM_REGISTER_DUMPER( TYPE_VID, VIDEO_FORMAT_MPG4,    stream_dumper_MPEG4_RAW );
STREAM_REGISTER_DUMPER( TYPE_VID, VIDEO_FORMAT_H264,    stream_dumper_H264_RAW );
STREAM_REGISTER_DUMPER( TYPE_VID, VIDEO_FORMAT_WMV3,    stream_dumper_RCV );
STREAM_REGISTER_DUMPER( TYPE_VID, VIDEO_FORMAT_VC1,     stream_dumper_RCV );


#endif

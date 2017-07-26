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

#ifndef _STREAM_BUFFER_H
#define _STREAM_BUFFER_H

#include "astdlib.h"

#include <pthread.h>

struct STREAM_BUFFER;

struct STREAM;

#define STREAM_BUFFER_NO_SLEEP			0x01
#define STREAM_BUFFER_NO_WRAP			0x02
#define STREAM_BUFFER_O_DIRECT			0x04
#define STREAM_BUFFER_NO_YIELD			0x10
#define STREAM_BUFFER_MMAP			0x20
#define STREAM_BUFFER_MMAP_FILE			0x40

//
// STREAM_BUFFER
//
typedef int (*STREAM_BUFFER_DELETE) ( struct STREAM_BUFFER *src );
typedef int (*STREAM_BUFFER_CLOSE)  ( struct STREAM_BUFFER *src );
typedef int (*STREAM_BUFFER_BUFFER) ( struct STREAM_BUFFER *buffer, int sleep );
typedef int (*STREAM_BUFFER_SET_POS)( struct STREAM_BUFFER *buffer, UINT64 pos, int force_reload, int resize );
typedef int (*STREAM_BUFFER_COOK)   ( void *ctx, UINT64 in_pos, const UCHAR *in_data, int *in_size, UCHAR **out_data, int *out_size );

typedef struct STREAM_BUFFER {
	// methods
	STREAM_BUFFER_DELETE	delete;
	STREAM_BUFFER_BUFFER	buffer;
	STREAM_BUFFER_CLOSE	close;
	STREAM_BUFFER_COOK	cook;
	STREAM_BUFFER_SET_POS	set_pos;
	
	// members
	char		tag[16];
	
	pthread_t 	thread_handle;
	pthread_mutex_t mutex;
	int		run;
	int		paused;
	int		sleep_time;
	
	struct STREAM	*s;
	STREAM_IO 	*io;
	UINT32		flags;
	
	int		abort;
	
	void		*priv;
	int		is_open;

	unsigned char  *data;
	unsigned int 	buffer_size;
	unsigned int 	overlap_size;
	int		virtual;
	
	UINT64		data_start;
	UINT64		data_end;

	UINT64		buf_file_pos;
	
	volatile int 	buf_write;
	volatile int	buf_write_cls;
	volatile UINT64	buf_write_pos;
	
	volatile int 	buf_hdd;
	volatile UINT64	buf_hdd_pos;
	volatile UINT64	buf_hdd_now_pos;
	volatile UINT64	buf_hdd_start_pos;
	volatile int	buf_hdd_fd;
	
	volatile int	buf_read;

	volatile int 	buf_scan;
	UINT64		buf_scan_pos;
	
	volatile int 	buf_tail;
	
	volatile int	buf_wrap;
	volatile int	buf_end;
	
	volatile int 	state;
	volatile int 	state_changed;
	int		no_sleep;
	
	int		data_rate;
	int		last_size;
	int		last_time;
	
	UINT64		vid_last_pos;
	unsigned int	vid_last_buf;
	UINT64		aud_last_pos;
	unsigned int	aud_last_buf;
	UINT64		sub_last_pos;
	unsigned int	sub_last_buf;
	
	int		audio;
	int		video;
	int		subtitle;
	
	void		*cook_ctx;
	UINT64		bite_file_pos;
	int		bite_size;
	int		bite_pos;
	UCHAR		*bite;

	const char	*mmap_file;
	int		mmap_fd;
	int		mmap_size;
	
	int 		stat_time;
	int		stat_bytes;

} STREAM_BUFFER;

enum {
	STREAM_BUFFER_NO_ABORT = 0,
	STREAM_BUFFER_ABORT_FINAL,
	STREAM_BUFFER_ABORT_THIS,
};

// constructors
STREAM_BUFFER *new_stream_sink_buffer( void );
STREAM_BUFFER *new_stream_buffer_raw( void );
STREAM_BUFFER *new_stream_buffer_raw_non_blocked( void );
STREAM_BUFFER *new_stream_buffer_cooked( void );

int 	stream_buffer_set_cook( STREAM_BUFFER *buffer, STREAM_BUFFER_COOK cook, void *cook_ctx, int bite_size );
void 	stream_buffer_pause( STREAM_BUFFER *buffer );
void 	stream_buffer_un_pause( STREAM_BUFFER *buffer );
int 	stream_buffer_get_write_pointer( STREAM_BUFFER *buffer, UCHAR **data );
int 	stream_buffer_update_write_pointer( STREAM_BUFFER *buffer, int count );
int 	stream_buffer_alloc( STREAM_BUFFER *buffer, int size );
void 	stream_buffer_free( STREAM_BUFFER *buffer );
int     stream_buffer_resize( STREAM_BUFFER *buffer, int new_size );

static inline int stream_buffer_set_mmap_file( STREAM_BUFFER *buffer, const char *mmap_file )
{
	if( !buffer ) {
		return 1;
	}
	buffer->mmap_file = mmap_file;

	return 0;
}

static inline void stream_buffer_state_set( STREAM_BUFFER *buffer, int new )
{
	buffer->state = new;
	buffer->state_changed = 1;
}

#endif

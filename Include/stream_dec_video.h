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

#ifndef _STREAM_DEC_VIDEO_H
#define _STREAM_DEC_VIDEO_H

#include "av.h"

struct STREAM_DEC_VIDEO;
struct STREAM_VIDEO_MANGLER;
struct STREAM_DEINT;
struct STREAM;
struct STREAM_RC;
struct STREAM_SINK_VIDEO;

//
// DEC_VIDEO
//
typedef int (*DEC_VIDEO_DESTROY)( struct STREAM_DEC_VIDEO *dec );
typedef int (*DEC_VIDEO_OPEN )  ( struct STREAM_DEC_VIDEO *dec, VIDEO_PROPERTIES *video, void *ctx, int *need_flush, int *need_reorder );
typedef int (*DEC_VIDEO_PREPARE)( struct STREAM_DEC_VIDEO *dec, VIDEO_FRAME **frames, int num_frames );
typedef int (*DEC_VIDEO_CLEANUP)( struct STREAM_DEC_VIDEO *dec, VIDEO_FRAME **frames, int num_frames );
typedef int (*DEC_VIDEO_CLOSE)  ( struct STREAM_DEC_VIDEO *dec );
typedef int (*DEC_VIDEO_SEEK)  ( struct STREAM_DEC_VIDEO *dec, int time );
typedef int (*DEC_VIDEO_FLUSH)  ( struct STREAM_DEC_VIDEO *dec );
typedef int (*DEC_VIDEO_DECODE) ( struct STREAM_DEC_VIDEO *dec, UCHAR *data, int size, VIDEO_FRAME **pframe, int *decoded, int *time );
typedef int (*DEC_VIDEO_DECODE2)( struct STREAM_DEC_VIDEO *dec, UCHAR *data, int size, VIDEO_FRAME **in_frame, VIDEO_FRAME **out_frame, int *decoded, int *time );
typedef int (*DEC_VIDEO_DEC_IN) ( struct STREAM_DEC_VIDEO *dec, VIDEO_FRAME **data_frame, int *decoded, int *time );
typedef int (*DEC_VIDEO_PUT_OUT)( struct STREAM_DEC_VIDEO *dec, VIDEO_FRAME **in_frame );
typedef int (*DEC_VIDEO_GET_OUT)( struct STREAM_DEC_VIDEO *dec, VIDEO_FRAME **out_frame );
typedef int (*DEC_VIDEO_GET_RC) ( struct STREAM_DEC_VIDEO *dec, struct STREAM_RC *rc );
typedef int (*DEC_VIDEO_RENDER) ( struct STREAM_DEC_VIDEO *dec, VIDEO_FRAME *dst, VIDEO_FRAME *src );
typedef int (*DEC_VIDEO_NEED_REALLOC) ( struct STREAM_DEC_VIDEO *dec, VIDEO_PROPERTIES *video, int *num_frames );
typedef struct STREAM_SINK_VIDEO *(*DEC_VIDEO_GET_SINK) ( struct STREAM_DEC_VIDEO *dec );

typedef struct STREAM_DEC_VIDEO {
	const char	  *name;
	DEC_VIDEO_DESTROY destroy;
	DEC_VIDEO_OPEN    open;
	DEC_VIDEO_PREPARE prepare;
	DEC_VIDEO_PREPARE cleanup;
	DEC_VIDEO_CLOSE   close;
	DEC_VIDEO_DECODE  decode;
	DEC_VIDEO_DECODE2 decode2;
	DEC_VIDEO_DEC_IN  dec_in;
	DEC_VIDEO_PUT_OUT put_out;
	DEC_VIDEO_GET_OUT get_out;
	DEC_VIDEO_SEEK    seek;
	DEC_VIDEO_FLUSH   flush;
	DEC_VIDEO_GET_RC  get_rc;
	DEC_VIDEO_RENDER  render;
	DEC_VIDEO_NEED_REALLOC need_realloc;
	DEC_VIDEO_GET_SINK get_sink;
	
	// members
	VIDEO_PROPERTIES _video;
	VIDEO_PROPERTIES *video;
	int		is_open;
	int 		async;
	int 		no_extra;
	int		cpu;
	void		*ctx;
	void		*priv;
} STREAM_DEC_VIDEO;

STREAM_DEC_VIDEO *stream_get_new_dec_video( VIDEO_PROPERTIES *video, struct STREAM_VIDEO_MANGLER **mangler, int best_cpu, int forced, const char *dec_name );
int stream_get_dec_video_res( VIDEO_PROPERTIES *video, int *width, int *height, int best_cpu, const char *dec_name );

#endif

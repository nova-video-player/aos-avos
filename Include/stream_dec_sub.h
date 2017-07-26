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

#ifndef _STREAM_DEC_SUB_H
#define _STREAM_DEC_SUB_H

#include "av.h"

struct STREAM_DEC_SUB;
struct STREAM;

//
// DEC_SUB
//
typedef int (*DEC_SUB_DESTROY)( struct STREAM_DEC_SUB *dec );
typedef int (*DEC_SUB_OPEN )  ( struct STREAM_DEC_SUB *dec, SUB_PROPERTIES *subtitle, void *ctx );
typedef int (*DEC_SUB_CLOSE)  ( struct STREAM_DEC_SUB *dec );
typedef int (*DEC_SUB_FLUSH)  ( struct STREAM_DEC_SUB *dec );
typedef int (*DEC_SUB_DECODE) ( struct STREAM_DEC_SUB *dec, UCHAR *data, int size, int time, VIDEO_FRAME **pframe );

typedef struct STREAM_DEC_SUB {
	const char	  *name;
	DEC_SUB_DESTROY destroy;
	DEC_SUB_OPEN    open;
	DEC_SUB_CLOSE   close;
	DEC_SUB_DECODE  decode;
	DEC_SUB_FLUSH   flush;
	
	// members
	void		*priv;
	SUB_PROPERTIES _subtitle;
	SUB_PROPERTIES *subtitle;
	int		is_open;
	void		*ctx;
} STREAM_DEC_SUB;

STREAM_DEC_SUB *stream_get_new_dec_sub( int format );

#endif

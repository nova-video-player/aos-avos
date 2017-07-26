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

#ifndef _STREAM_READ_CTX_H_
#define _STREAM_READ_CTX_H_

#include "types.h"

typedef struct STREAM_READ_CTX {
	void	*ctx;
	UCHAR 	(*readc)(void *context);
	int  	(*read) (void *context, unsigned char *data, int count);
	UINT64 	(*seek) (void *context, UINT64 count, int whence );
	UINT64 	(*tell) (void *context, unsigned int *bpos);
	int   	(*eof)  (void *context);
	int   	(*end)  (void *context);
	UINT64 	(*head) (void *context);
} STREAM_READ_CTX;

// helper defines to not have to type too much all the time:
#define SRC_readc( src )              (src)->readc( (src)->ctx )
#define SRC_read( src, data, count )  (src)->read( (src)->ctx, (data), (count) )
#define SRC_seek( src, count, whence )(src)->seek( (src)->ctx, (count), (whence) )
#define SRC_tell( src, bpos )         (src)->tell( (src)->ctx, (bpos) )
#define SRC_eof( src )                (src)->eof( (src)->ctx )
#define SRC_end( src )                (src)->end( (src)->ctx )
#define SRC_head( src )               (src)->head( (src)->ctx )

#endif 

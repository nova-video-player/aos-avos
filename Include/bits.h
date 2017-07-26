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

#ifndef BITS_H
#define BITS_H

#include "types.h"

typedef struct BITS {
	unsigned char 	*byte;
	unsigned char 	*start;
	int 		bit;
	int 		index;
	int 		size;
	int 		h264;
	unsigned char 	zero_count;
	
} BITS;

void BITS_init  ( BITS *ctx, UCHAR *data, int size );
void BITS_init_h264( BITS *ctx, UCHAR *data, int size );
void BITS_align ( BITS *ctx );
UINT BITS_get1  ( BITS *ctx );
UINT BITS_peek1 ( BITS *ctx );
void BITS_poke1 ( BITS *ctx, int bit );
UINT BITS_get   ( BITS *ctx, int count );
void BITS_skip  ( BITS *ctx, int count );
UINT BITS_offset( BITS *ctx );
UCHAR *BITS_current( BITS *ctx, int *bit );

#endif

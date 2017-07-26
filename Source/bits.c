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
#include "bits.h"

void BITS_init( BITS *ctx, UCHAR *data, int size )
{
	ctx->byte  = data;
	ctx->start = data;
	ctx->bit   = 0x80;
	ctx->index = 0;
	ctx->size  = size;
	ctx->h264  = 0;
}

void BITS_init_h264( BITS *ctx, UCHAR *data, int size )
{
	ctx->byte  = data;
	ctx->start = data;
	ctx->bit   = 0x80;
	ctx->index = 0;
	ctx->size  = size;
	ctx->h264  = 1;
	ctx->zero_count = *ctx->byte ? 0 : 1;
}

void BITS_align( BITS *ctx )
{
	if( ctx->bit != 0x80 ) {
		ctx->byte++;
		ctx->bit = 0x80;
	}
}

UINT BITS_get1( BITS *ctx )
{
	int b = ((*ctx->byte & ctx->bit) != 0);
	ctx->bit /= 2;
	ctx->index++;
	if( ctx->bit == 0 ) {
		ctx->bit = 0x80;
		ctx->byte++;
		if( ctx->h264 ) {
			// H264 style emulation prevention, 0x00 0x00 0x00 is prevented by inserting
			// 0x03 after 0x00 0x00 -> 0x00 0x00 0x03 0x00
			if( *ctx->byte == 0x00 ) {
				ctx->zero_count++;
			} else {
				if( ctx->zero_count == 2 && *ctx->byte == 0x03 ) {
					ctx->byte++;
				}
				ctx->zero_count = 0;
			}
		}
	}
	return b;
}

UINT BITS_get( BITS *ctx, int count )
{
	UINT b = 0;
	while( count-- )
		b = (b << 1) | BITS_get1( ctx );
	return b;
}

void BITS_skip( BITS *ctx, int count )
{
	while ( count && ctx->bit != 0x80 ) {
		// not aligned, skip one bit at a time
		ctx->bit /= 2;
		ctx->index++;
		if( ctx->bit == 0 ) {
			ctx->byte ++;
			ctx->bit = 0x80;
		}
		count --;
	} 
	if( count >= 8 ) {
		// skip whole bytes
		ctx->byte +=  count / 8;
		count     -= (count / 8) * 8;
		ctx->index += 8;
	}
	while( count ) {
		// skip the rest
		ctx->bit /= 2;
		ctx->index++;
		if( ctx->bit == 0 ) {
			ctx->byte ++;
			ctx->bit = 0x80;
		}
		count --;
	}
}

UINT BITS_peek1( BITS *ctx )
{
	return ((*ctx->byte & ctx->bit) != 0);
}

void BITS_poke1( BITS *ctx, int bit )
{
	UCHAR byte = *ctx->byte & ~ctx->bit;
	if( bit )
		byte |= ctx->bit;
		 
	*ctx->byte = byte;
}

UINT BITS_offset( BITS *ctx )
{
	return ctx->byte - ctx->start;
}

UCHAR *BITS_current( BITS *ctx, int *bit )
{
	if( bit )
		*bit = ctx->bit; 
	return ctx->byte;
}

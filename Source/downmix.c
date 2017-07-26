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
#include "types.h"
#include "debug.h"
#include "downmix.h"
#include "get.h"

#include <math.h>

#define DBGCA3 	if(Debug[DBG_CA] > 2 )

static inline int clamp( int v )
{
	if( v > 32767 ) {
		return 32767;
	} else if ( v < -32768 ) {
		return -32768;
	}
	return v;
}

static int no_downmix = 0;

// ******************************************
//
//	downmix
//
// ******************************************
void downmix( uint16_t *pcm, uint8_t *src, int samples, int channels, int bits, int *map )
{
DBGCA3 serprintf("dmix: num %5d  ch %d  bits %d\r\n", samples, channels, bits );	
	if ( no_downmix || !pcm || !src || channels > 8 ) {
		return;
	}
	
	int shift = (bits == 32) ? 16 : (bits == 24 ) ? 8 : 0;
	int i;
	for( i = 0; i < samples; i ++ ) {
		int32_t ch[9] = { 0 };
		if( channels == 1 ) {
			if( bits == 16 ) {
				ch[map[0]] = getS16LE( src ); src += 2;
			} else if( bits == 24 ) {
				ch[map[0]] = getS24LE( src ); src += 3;
			} else if( bits == 32 ) {
				ch[map[0]] = getS32LE( src ); src += 4;
			} 
			*pcm++ = clamp( ch[CH_FL] >> shift );
			*pcm++ = clamp( ch[CH_FL] >> shift );
		} else {
			int c;
			for( c = 0; c < channels; c++ ) {
				if( bits == 16 ) {
					ch[map[c]] = getS16LE( src ); src += 2;
				} else if( bits == 24 ) {
					ch[map[c]] = getS24LE( src ); src += 3;
				} else if( bits == 32 ) {
					ch[map[c]] = getS32LE( src ); src += 4;
				}
			}
			*pcm++ = clamp( (ch[CH_FL] + ch[CH_CTR] + ch[CH_SUB] + ch[CH_BL] + ch[CH_SL]) >> shift ); // left
			*pcm++ = clamp( (ch[CH_FR] + ch[CH_CTR] + ch[CH_SUB] + ch[CH_BR] + ch[CH_SR]) >> shift ); // right
		}
	}
}

// ******************************************
//
//	downmix_planar
//
// ******************************************
void downmix_planar( uint16_t *pcm, uint8_t *_src[], int samples, int channels, int bits, int *map )
{
DBGCA3 serprintf("dmixP: num %5d  ch %d  bits %d\r\n", samples, channels, bits );	
	if ( no_downmix || !pcm || !_src || channels > 8 ) {
		return;
	}
	
	uint8_t *src[8];
	int c;
	for( c = 0; c < channels; c++ ) {
		src[c] = _src[c];
	}

	int shift = (bits == 32) ? 16 : (bits == 24 ) ? 8 : 0;
	int i;

	for( i = 0; i < samples; i ++ ) {
		int32_t ch[9] = { 0 };
		if( channels == 1 ) {
			if( bits == 16 ) {
				ch[map[0]] = getS16LE( src[0] ); src[0] += 2;
			} else if( bits == 24 ) {
				ch[map[0]] = getS24LE( src[0] ); src[0] += 3;
			} else if( bits == 32 ) {
				ch[map[0]] = getS32LE( src[0] ); src[0] += 4;
			} 
			*pcm++ = clamp( ch[CH_FL] >> shift );
			*pcm++ = clamp( ch[CH_FL] >> shift );
		} else {
			int c;
			for( c = 0; c < channels; c++ ) {
				if( bits == 16 ) {
					ch[map[c]] = getS16LE( src[c] ); src[c] += 2;
				} else if( bits == 24 ) {
					ch[map[c]] = getS24LE( src[c] ); src[c] += 3;
				} else if( bits == 32 ) {
					ch[map[c]] = getS32LE( src[c] ); src[c] += 4;
				}
			}
			*pcm++ = clamp( (ch[CH_FL] + ch[CH_CTR] + ch[CH_SUB] + ch[CH_BL] + ch[CH_SL]) >> shift ); // left
			*pcm++ = clamp( (ch[CH_FR] + ch[CH_CTR] + ch[CH_SUB] + ch[CH_BR] + ch[CH_SR]) >> shift ); // right
		}
	}
}

void downmix_float( uint16_t *pcm, uint8_t *src, int samples, int channels, int bits, int *map )
{
DBGCA3 serprintf("dmix_flt: num %5d  ch %d  bits %d\r\n", samples, channels, bits );	
	if ( no_downmix || !pcm || !src || channels > 8 ) {
		return;
	}
	
	int i;
	for( i = 0; i < samples; i ++ ) {
		int32_t ch[9] = { 0 };
		if( channels == 1 ) {
			if( bits == 32 ) {
				ch[map[0]] = clamp( lrintf( *(float*)src * (1 << 15))); src += 4;
			} else if ( bits == 64 ) {
				ch[map[0]] = clamp( lrintf( *(double*)src * (1 << 15))); src += 8;
			}
			*pcm++ = clamp( ch[CH_FL] );
			*pcm++ = clamp( ch[CH_FL] );
		} else {
			int c;
			for( c = 0; c < channels; c++ ) {
				if( bits == 32 ) {
					ch[map[c]] = clamp( lrintf( *(float*)src * (1 << 15))); src += 4;
				} else if( bits == 64 ) {
					ch[map[c]] = clamp( lrintf( *(double*)src * (1 << 15))); src += 8;
				}  
			}
			*pcm++ = clamp( (ch[CH_FL] + ch[CH_CTR] + ch[CH_SUB] + ch[CH_BL] + ch[CH_SL]) ); // left
			*pcm++ = clamp( (ch[CH_FR] + ch[CH_CTR] + ch[CH_SUB] + ch[CH_BR] + ch[CH_SR]) ); // right
		}
	}
}

void downmix_float_planar( uint16_t *pcm, uint8_t *_src[], int samples, int channels, int bits, int *map )
{
DBGCA3 serprintf("dmix_fltP: num %5d  ch %d  bits %d\r\n", samples, channels, bits );	
	if ( no_downmix || !pcm || !_src || channels > 8 ) {
		return;
	}
	
	uint8_t *src[8];
	int c;
	for( c = 0; c < channels; c++ ) {
		src[c] = _src[c];
	}

	int i;
	for( i = 0; i < samples; i ++ ) {
		int32_t ch[9] = { 0 };
		if( channels == 1 ) {
			if( bits == 32 ) {
				ch[map[0]] = clamp( lrintf( *(float*)src[0] * (1 << 15))); src[0] += 4;
			} else if ( bits == 64 ) {
				ch[map[0]] = clamp( lrintf( *(double*)src[0] * (1 << 15))); src[0] += 8;
			}
			*pcm++ = clamp( ch[CH_FL] );
			*pcm++ = clamp( ch[CH_FL] );
		} else {
			int c;
			for( c = 0; c < channels; c++ ) {
				if( bits == 32 ) {
					ch[map[c]] = clamp( lrintf( *(float*)src[c] * (1 << 15))); src[c] += 4; 
				} else if( bits == 64 ) {
					ch[map[c]] = clamp( lrintf( *(double*)src[c] * (1 << 15))); src[c] += 8;
				}  
			}
			*pcm++ = clamp( (ch[CH_FL] + ch[CH_CTR] + ch[CH_SUB] + ch[CH_BL] + ch[CH_SL]) ); // left
			*pcm++ = clamp( (ch[CH_FR] + ch[CH_CTR] + ch[CH_SUB] + ch[CH_BR] + ch[CH_SR]) ); // right
		}
	}
}


#ifdef DEBUG_MSG
static void _no_downmix( void )
{
	no_downmix = 1 - no_downmix;
serprintf("no_downmix: %d\r\n", no_downmix );
}

DECLARE_DEBUG_COMMAND_VOID( "nodm", _no_downmix );
#endif

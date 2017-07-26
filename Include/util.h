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

#ifndef _UTIL_H
#define _UTIL_H

#include "types.h"
#include "awchar.h"
#include <string.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef ABS
#define ABS( a )  ( (a) >= 0  ? (a) :-(a) )
#endif

#define SGN( a )  ( (a) == 0 ? 0 : ( (a) > 0 ? 1 : -1) )
#ifndef MAX
  #define MAX(a, b) ( (a) > (b) ? (a) : (b) )
#endif
#ifndef MIN
  #define MIN(a, b) ( (a) < (b) ? (a) : (b) )
#endif
#define SWAP(a, b) { a ^= b; b ^= a; a ^= b; }  // be careful to swap only variables, not expressions
#define ALIGN(n, a) (((n)+((a)-1))&~((a)-1))

void swap16_buf( UCHAR *buffer, int size );
char *int2dez(int x, char* buf, int minstellen);
int  strcmpNC( const char* s1, const char * s2 ); 
int  strncmpNC( const char* s1, const char * s2, int length );
int  strcmpNC_suffix(const char * string, const char * suffix);
char *strstrNC(char *string, char *ToFind );

int  strcmp_dict( const char* s1, const char * s2 );
void chomp(char* str);
int  alog2( unsigned int v );

int  unicode_utf16_to_utf8(unsigned char *utf8, const unsigned short *utf16, int len);
int  utf8_to_utf16(unsigned short *utf16, const unsigned char  *utf8,  int utf16_max );
void utf16_to_utf8(unsigned char *utf8,  const unsigned short *utf16, int utf8_max );
void latin1_to_utf8( char *d, const UCHAR *s, int max_len);
const unsigned char *u8_to_u16(unsigned short *utf16, const unsigned char *utf8);

// UTF-8 coded "prime"=min and "double prime"=sec chars 
#define UTF8_MIN "\342\200\262\000"	// unicode 2032
#define UTF8_SEC "\342\200\263\000"	// unicode 2033

// ATTENTION : make sure that the size of the dst buffer is at least (n + 1) bytes!!!
static inline char *strnZcpy( char *dst, const char *src, size_t n )
{
	strncpy( dst, src, n );
	dst[n] = '\0';

	return dst;
}

void memset16(uint16_t *dst, uint16_t value, int count);
void memset32(uint32_t *dst, uint32_t value, int count);

void sec_to_hms( int *hour, int *min, int *sec );

int adjust_oom(pid_t pid, int value);

#endif

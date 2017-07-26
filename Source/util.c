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
#include "util.h"
#include "app.h"
#include "debug.h"
#include "awchar.h"
#include "file.h"
#include "atime.h"
#include "neon.h"

#include "astdlib.h"

#include <ctype.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>

#define ERR	if (0)
#define DBG	if (0)

//------------------------------------------------------------------------
//	swap16
//------------------------------------------------------------------------
void swap16_buf( UCHAR *buffer, int size )
{
	UCHAR *p    = buffer;
	UCHAR *pend = buffer + size;

	while ( p < pend ) {
		UCHAR tmp  = p[0];
		      p[0] = p[1];
		      p[1] = tmp;
		p+= 2;
	}
}

//------------------------------------------------------------------------
//	int2dez
//------------------------------------------------------------------------
char *int2dez(int x, char* buf, int min_digit) 
{
  	int d_digit;
  	int r_digit = 0;
  	int tmp = x;
  	char* ret;
  	char* pnt;

	if ( min_digit < 0 ) {
		min_digit *= -1;
	} 
		
	if (tmp < 0)
		tmp = (-1) * tmp;
		
  	if (min_digit == 0) 
		do {
    			tmp /= 10;
    			r_digit ++;
    		} while (tmp > 0 );
  	
  	d_digit = MAX(min_digit, r_digit);

	r_digit = d_digit - r_digit;
	
  	pnt = buf + d_digit;
	
	if (x < 0) {
		pnt ++;
		buf[0] = '-';
		x = (-1) * x;
	}
	ret = pnt;
	
  	do {
		*(--pnt) = (x % 10) + '0';
		x /= 10;
		d_digit --;
    	} while ( d_digit > 0 );
    	
  	*ret = '\0';
  	return ret;
}

//------------------------------------------------------------------------
//	strcmpNC
//------------------------------------------------------------------------
int strcmpNC ( const char* s1, const char * s2 )
{
 	while (*s1 != '\0' && toupper(*s1) == toupper(*s2) ) {
      		s1++;
      		s2++;
    	}
 	return toupper( (*(unsigned char *) s1) ) - toupper( (*(unsigned char *) s2) );
}

//------------------------------------------------------------------------
//	strcmpNC_suffix
//------------------------------------------------------------------------
int strcmpNC_suffix(const char * string, const char * suffix)
{
	unsigned int str_len = strlen(string);
	unsigned int suf_len = strlen(suffix);
	if( suf_len > str_len){
		return -2;
	}
	while(suf_len > 0 && toupper(string[str_len-1]) == toupper(suffix[suf_len-1]) ) {
		--str_len;
		--suf_len;
	}
	if(suf_len == 0){
		return 0;
	} else {
		return toupper(string[str_len-1]) - toupper(suffix[suf_len-1]);
	}
}

//------------------------------------------------------------------------
//	strncmpNC
//
//	Warning : if a string is stored in buffer 'length' must be the length of the
//	string and not the size of the buffer. The algorithm would otherwise check the
//	characters until the end of the buffer and the result would probably be wrong.
//
//------------------------------------------------------------------------
int strncmpNC ( const char* s1, const char * s2, int length )
{
	while ( (length != 0) && toupper(*s1) == toupper(*s2) )
	{
      		s1++;
      		s2++;
      		length--;
	}
	if ( length == 0 ) {
		s1--; s2--;
	}
 	return toupper( (*(unsigned char *) s1) ) - toupper( (*(unsigned char *) s2) );
}


//------------------------------------------------------------------------
//	strstrNC
//------------------------------------------------------------------------
char *strstrNC(char *string, char *ToFind )
{
	char *cpToFind = ToFind;
	char *FoundAt = NULL;
	
	while ( *string != '\0' ) {
		if( toupper(*string) == toupper(*cpToFind) ) {
			if ( FoundAt == NULL )
				FoundAt = string;
			cpToFind++;
			if ( *cpToFind == '\0' )
				return( FoundAt );
		}
		else {
			cpToFind = ToFind;
			FoundAt = NULL;
		}
		string++;
	}	
	
	return( NULL );
}

//------------------------------------------------------------------------
//	UNICODE UTF8 <-> UTF16 conversions
//------------------------------------------------------------------------
//
//	UTF-8 is :
//	U-00000000 - U-0000007F:  0xxxxxxx  
//	U-00000080 - U-000007FF:  110xxxxx 10xxxxxx  
//	U-00000800 - U-0000FFFF:  1110xxxx 10xxxxxx 10xxxxxx  
//	U-00010000 - U-001FFFFF:  11110xxx 10xxxxxx 10xxxxxx 10xxxxxx  
//	U-00200000 - U-03FFFFFF:  111110xx 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx  
//	U-04000000 - U-7FFFFFFF:  1111110x 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx  
//
//	UTF-16 is :
//	U-00000000 - U-0000FFFF:  xxxxxxxx xxxxxxxx 
//------------------------------------------------------------------------

// utf8 to utf16, prevent dest overflow (utf16_max), adds an ending 0 always
// returns the actual number of characters (not counting the ending 0)
int utf8_to_utf16(unsigned short *utf16, const unsigned char *utf8, int utf16_max )
{
	unsigned short *utf16_0;
	unsigned char 	c;

	utf16_0 = utf16;
	while ( (c = *utf8++) && utf16_max-- > 0 ) {
		unsigned short w = 0;
		if ( (c & 0x80) == 0 ) {
			w = c;			
		} else if ( (c & 0xE0) == 0xC0) {
			w = (c & 0x1F);
			w <<= 6;
			c = *utf8++;
			w |= (c & 0x3F);
		} else if ( (c & 0xF0) == 0xE0) {
			w = (c & 0x1F);
			w <<= 6;
			c = *utf8++;
			w |= (c & 0x3F);
			w <<= 6;
			c = *utf8++;
			w |= (c & 0x3F);
		} else {
			w = '?';
		}
		*utf16++ = w;
	}
	
	*utf16 = 0;
	return ( utf16 - utf16_0 );
}

// UTF-16 --> UTF-8
// this function returns the byte count of the string after conversion (not including the ending 0)
// the output utf8 string includes a terminating 0x00
int unicode_utf16_to_utf8(unsigned char *utf8, const unsigned short *utf16, int len)
{
	const unsigned short *p16 = utf16;
	unsigned char  	*p8  = utf8;
	unsigned short 	w;
	unsigned int 	i;
	int 		bcnt;
	
	if (len <= 0) {
		*p8++ = 0;
		return 0;
	}
		
	for (i = 0; i < len;i++) {
		w = *p16++;
		if (w <= 0x7F) {
			*p8++ = w;
		} else if (w <= 0x7FF) {
			*p8++ = 0xC0 | (w >> 6);
			*p8++ = 0x80 | (w & 0x3F);
		} else {
			*p8++ = 0xE0 | (w >> 12);
			*p8++ = 0x80 | ((w >> 6) & 0x3F);
			*p8++ = 0x80 | (w & 0x3F);
		}
	}
	bcnt = (int)(p8 - utf8);
	*p8++ = 0;	
	return bcnt;	
}

void utf16_to_utf8(unsigned char *utf8, const unsigned short *utf16, int utf8_max)
{
	while( *utf16 ) {
		USHORT w = *utf16++;
		if (w <= 0x7F) {
			if( utf8_max < 1 )
				break;
			*utf8++ = w;
			utf8_max --;
		} else if (w <= 0x7FF) {
			if( utf8_max < 2 )
				break;
			*utf8++ = 0xC0 | ( w >> 6);
			*utf8++ = 0x80 | ( w        & 0x3F);
			utf8_max -= 2;
		} else {
			if( utf8_max < 3 )
				break;
			*utf8++ = 0xE0 | ( w >> 12);
			*utf8++ = 0x80 | ((w >> 6)  & 0x3F);
			*utf8++ = 0x80 | ( w        & 0x3F);
			utf8_max -= 3;
		}
	}
	*utf8 = 0;	
}

// ---------------------------------------------------
//
// 	latin1_to_utf8
//
//	convert input string "s" from Latin1 encoding into
//	UTF-8. Output string is max. max_len and will be 0
//	terminated (like strnZcpy)
//
// ---------------------------------------------------
void latin1_to_utf8( char *d, const UCHAR *s, int max_len)
{
	int len = strlen( s );
	int new_len = 0;
	while ( len>0 && max_len>1 )
	{
		UCHAR c=*s++;
		len--;	

		if (c < 0x80) {	// if first 128 chars, just copy over
			if( max_len < 1 )
				break;
			*d++ = c;
			max_len--;
			new_len++;
		} else if (c >= 0xA0) 
		{
			if( max_len < 2 )
				break;
			*d++ = 0xC0 + ( ( c>>6 ) &0x3 );
			*d++ = 0x80 + ( c & 0x3f );
			max_len -= 2;
			new_len+=2;
		}
	}
	*d = 0;
}

// convert 1-3 unsigned char from utf8 to utf16, return pointer to next u8 char
const unsigned char *u8_to_u16(unsigned short *utf16, const unsigned char *utf8)
{
	unsigned short w = 0;

	if( utf8 && *utf8 ) {
		unsigned char c = *utf8++;
	
		if ( (c & 0x80) == 0 ) {
			w = c;			
		} else if ( (c & 0xE0) == 0xC0) {
			w = (c & 0x1F);
			w <<= 6;
			if( !*utf8 ) {
				goto ErrorExit;
			}
			c = *utf8++;
			w |= (c & 0x3F);
		} else if ( (c & 0xF0) == 0xE0) {
			w = (c & 0x1F);
			w <<= 6;
			if( !*utf8 ) {
				goto ErrorExit;
			}
			c = *utf8++;
			w |= (c & 0x3F);
			w <<= 6;
			if( !*utf8 ) {
				goto ErrorExit;
			}
			c = *utf8++;
			w |= (c & 0x3F);
		} else {
			w = '?';
		}
	}
	
ErrorExit:
	if( utf16 )
		*utf16 = w;
		
	return utf8;
}

static int cmp_dict( const wchar w1, const wchar w2 )
{
	// simple ascii order dummy for now - Roland make it better!
	return toupper( w1 ) - toupper( w2 );
}

// **************************************************************
//
//      strcmp_dict
//
//	compare two strings by the correct alphabetic order
//	( like in a dictionary )
//
// ****************************************************************
int strcmp_dict( const char *u1, const char *u2 )
{
	wchar w1 = 0;
	wchar w2 = 0;
	
	while( *u1 && *u2 ) {
		int ret;
		wchar w1;
		wchar w2;
		u1 = u8_to_u16( &w1, u1 );
		u2 = u8_to_u16( &w2, u2 );
		if ( (ret = cmp_dict( w1, w2 )) != 0 )
			return ret;
	}
	if( *u1 )
		u8_to_u16( &w1, u1 );
	if( *u2 )
		u8_to_u16( &w2, u2 );

	return cmp_dict( w1, w2 );
}

// **************************************************************
//
//      memset16
//
//	Same as memset() but fills the buffer with 16-bit values
//
// ****************************************************************
void memset16( uint16_t *addr, uint16_t value, int n )
{
	uint32_t pattern, *pl;
	uint16_t *ps;

	// It is faster to copy 32-bit words at addresses multiple of 4
	// => if the starting address is not a multiple of 4 copy a first 16-bit value to be properly aligned
	if ( (size_t)addr & 0x02 ) {
		ps = addr;
		*ps++ = value;
		n--;
		addr = ps;
	}

	// Then copy 32-bit words
	pl = (uint32_t*)addr;
	pattern = ( value << 16 ) | value;
	while ( n > 1 ) {
		*pl++ = pattern;
		n -= 2;
	}

	// And copy the last 16-bit value if there is one left 
	if ( n > 0 ) {
		ps = (uint16_t *)pl;
		*ps = value;
	}
}

// **************************************************************
//
//      memset32
//
//	Same as memset() but fills the buffer with 32-bit values
//
// ****************************************************************
void memset32( uint32_t *ptr, uint32_t value, int n )
{
#ifdef CONFIG_NEON
	neon_memset32( ptr, value, n );
#else
	while ( n--  ) {
		*ptr++ = value;
	}
#endif
}

// *********************************************************************
//
//      sec_to_hms
//
// *********************************************************************
void sec_to_hms( int *hour, int *min, int *sec )
{
	*min  = *sec / 60;
	*hour = *min / 60;
	*sec  = *sec % 60;
	*min  = *min % 60;
}

// For this to work you need a linux kernel running this patch:
// http://lwn.net/Articles/104180/
//
// pid is the id of the process we wan't to adjust the oom value for.
// Value must be in [-16..15]. -16 is least likely to be slain by the
// oom killer, 15 i most likely.
int adjust_oom(pid_t pid, int value)
{
	unsigned char path[32];
	snprintf(path, sizeof(path), "/proc/%i/oom_adj", pid);

	FILE *f = fopen(path, "w");
	if (f == NULL) {
		ERR serprintf("fopen failed: %s (%s:%i)\n", strerror(errno), __FILE__, __LINE__);
		return -1;
	}
	fprintf(f, "%i\n", value);
	fclose(f);
	return 0;
}

// remove a trailing newline
void chomp(char* str)
{
	int len = strlen(str);
	if (str[len - 1] == '\n') {
		str[len - 1 ] = '\0';
	}
}

static const UCHAR _log2_tab[256]={
        0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
        5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
        6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
        6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
};

int alog2( unsigned int v )
{
	int n;

	n = 0;
	if ( v & 0xffff0000 ) {
		v >>= 16;
		n += 16;
	}
	if ( v & 0xff00 ) {
		v >>= 8;
		n += 8;
	}
	n += _log2_tab[v];

	return n;
}


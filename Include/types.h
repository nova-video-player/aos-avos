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

/*
 * types.h
 */
#ifndef __TYPES_H__
#define __TYPES_H__

#include <stdint.h>
#include <stddef.h>

#ifndef min
static inline int min(int a, int b){
        return (a) < (b) ? (a):(b);
}
#endif

#ifndef max
static inline int max(int a, int b){
        return (a) < (b) ? (b):(a);
}
#endif

#ifndef TRUE
	#define FALSE 	0
	#define TRUE 	1
#endif


typedef void (*fp)  ();		// void function pointer
typedef void (*fpx) (void*);	// void function pointer with context

typedef unsigned int 		BOOL;
typedef char            	BYTE;
typedef char            	CHAR;
typedef unsigned char   	UCHAR;
typedef unsigned char   	UBYTE;
typedef int             	INT;
typedef unsigned int    	UINT;
typedef short           	SHORT;
typedef unsigned short  	USHORT;
typedef long            	LONG;
typedef unsigned long  	 	ULONG;

typedef int8_t            	INT8;
typedef uint8_t   		UINT8;
typedef int16_t           	INT16;
typedef uint16_t  		UINT16;
typedef int32_t            	INT32;
typedef uint32_t   		UINT32;
typedef int64_t		       	INT64;
typedef uint64_t 		UINT64;

#define DEPRECATED __attribute__((deprecated))
#define __PACKED__  __attribute__ ((__packed__))
#define UNUSED __attribute__((unused))

#endif /* __TYPES_H__ */

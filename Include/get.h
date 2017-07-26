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

#ifndef _GET_H
#define _GET_H

#include <stdint.h>

static inline unsigned char get8( const uint8_t *c )
{
	return *c;
}

static inline unsigned int get16LE( const uint8_t *c )
{
	unsigned int ui; 
	
	ui  =  *c++;
	ui += (*c++ << 8);
	
	return ui;
}

static inline int getS16LE( const uint8_t *c)
{
	int ui = 0;

	// we shift 8 more and then back in order to get the sign right!
	ui += (*c++ << 16);
	ui += (*c++ << 24);
	
	return (ui >> 16);
}

static inline unsigned int get24LE( const uint8_t *c )
{
	unsigned int ui;

	ui  =  *c++;
	ui += (*c++ <<  8);
	ui += (*c++ << 16);
	
	return ui;
}

static inline int getS24LE( const uint8_t *c)
{
	int ui = 0;

	// we shift 8 more and then back in order to get the sing right!
	ui += (*c++ <<  8);
	ui += (*c++ << 16);
	ui += (*c++ << 24);
	
	return (ui >> 8);
}

static inline unsigned int get32LE( const uint8_t *c )
{
	unsigned int ui;

	ui  =  *c++;
	ui += (*c++ <<  8);
	ui += (*c++ << 16);
	ui += (*c   << 24);
	
	return ui;
}

static inline int getS32LE( const uint8_t *c)
{
	int ui = 0;

	ui +=  *c++;
	ui += (*c++ <<  8);
	ui += (*c++ << 16);
	ui += (*c++ << 24);
	
	return ui;
}

static inline unsigned int get16BE( const uint8_t *c )
{
	unsigned int size  = *c++;
	size = (size << 8) | *c;
	
	return size;
}

static inline unsigned int get24BE( const uint8_t *c )
{
	unsigned int size  = *c++;
	size = (size << 8) | *c++;
	size = (size << 8) | *c;
	
	return size;
}

static inline unsigned int get32BE( const uint8_t *c )
{
	unsigned int size  = *c++;
	size = (size << 8) | *c++;
	size = (size << 8) | *c++;
	size = (size << 8) | *c;
	
	return size;
}

static inline UINT64 get64BE( const uint8_t *c )
{
	UINT64 size  = *c++;
	size = (size << 8) | *c++;
	size = (size << 8) | *c++;
	size = (size << 8) | *c++;
	size = (size << 8) | *c++;
	size = (size << 8) | *c++;
	size = (size << 8) | *c++;
	size = (size << 8) | *c;
	
	return size;
}
#endif

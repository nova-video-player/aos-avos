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

// ****************************************************************************
//
//	ID3TAG.H : header for ID3V2 tag parsing
//
// ****************************************************************************

#ifndef _ID3TAG_H_
#define _ID3TAG_H_

#include <time.h>

// *****************************************************
//
// ID3 Tag information, if it's there then the first
// field reads "TAG"
//
// *****************************************************

#define MAX_TAG_LENGTH 255
#define ID3_YEAR_LENGTH 4
#define TAG_DESCRIPTION_LEN	512
#define APIC_MAX_SIZE (2 * 1024 * 1024)

typedef struct id3_tag {
	int		valid;
	char 		title[MAX_TAG_LENGTH + 1];
	char 		artist[MAX_TAG_LENGTH + 1];
	char 		album[MAX_TAG_LENGTH + 1];
	char 		album_artist[MAX_TAG_LENGTH + 1];
	char 		composer[MAX_TAG_LENGTH + 1];
	char 		author[MAX_TAG_LENGTH + 1];
	char 		writer[MAX_TAG_LENGTH + 1];
	char 		genre[MAX_TAG_LENGTH + 1];
	char 		year[ID3_YEAR_LENGTH + 1];
	char 		comment[MAX_TAG_LENGTH + 1];
	char		description[ TAG_DESCRIPTION_LEN + 1];
	char		compilation[MAX_TAG_LENGTH + 1];
	char		location[MAX_TAG_LENGTH + 1];	
	int		track;
	int		discnumber;
	unsigned int	msec;				// Total length of the track in milliseconds
	time_t		date;
	int		apic;
	int		clippable;
	int		id3_ver;
} ID3_TAG;

typedef struct apic_str {
	unsigned char  *buffer;
	unsigned int 	buffer_size;
	unsigned int 	size;
	unsigned int 	etype;
	unsigned int 	valid;
} APIC;

unsigned int ID3V2_GetSize( unsigned char *buffer );

// ****************************************************************************
//
//	ID3V2_Parse
//
//	OUT	ID3_TAG *tag		where to store the tag data 
//	IN	_buffer,		(circular) buffer with tag data 
//	IN	_buffer_size		size of buffer
//	IN	_buf_read		read pointer in buffer
//	OUT	APIC *apic		where to store the APIC, if NULL no apic extracted
//
// ****************************************************************************
int ID3V2_Parse(ID3_TAG *tag, 
		unsigned char *_buffer, unsigned int _buffer_size, int _buf_read, 
		APIC *apic );

unsigned int ID3V1_GetSize( unsigned char *buffer );

// ****************************************************************************
//
//	ID3V1_Parse
//
//	OUT	tag	where to store tag data
//	IN	buffer	buffer with 128 bytes tag data
//
// ****************************************************************************
int ID3V1_Parse(ID3_TAG *tag, unsigned char *buffer );

void ID3_show_tag( ID3_TAG *tag );

const char *ID3_get_genre( int genre );
void ID3_set_get_genre_from_ID( char *tag );

#endif	// _ID3TAG_H_

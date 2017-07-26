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
// ****************************************************************************
//
//	ID3TAG.C : ID3V2 tag parser
//
// ****************************************************************************
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "types.h"
#include "app.h"
#include "i18n.h"
#include "debug.h"
#include "mp3.h"
#include "id3tag.h"
#include "util.h"
#include "file_info.h"
#include "file_type.h"
#include "wchar.h"

#define DBG if(Debug[DBG_PARSER])

#ifdef CONFIG_AUDIO

typedef struct ID3_CTX {
	unsigned char 	*buffer;
	unsigned int	buffer_size;
	int 		buf_read;
	int 		toRead;
} ID3_CTX;

// ****************************************************************************
// ID3TAG v2 descriptors
// ****************************************************************************
const char *ID3TAG_APIC_Descriptor[2]		= {"APIC", "PIC"};
const char *ID3TAG_TITLE_Descriptor[2]  	= {"TIT2", "TT2"};
const char *ID3TAG_ALBUM_Descriptor[2]  	= {"TALB", "TAL"};
const char *ID3TAG_ARTIST_Descriptor[2] 	= {"TPE1", "TP1"};
const char *ID3TAG_ALBUM_ARTIST_Descriptor[2] 	= {"TPE2", "TP2"};
const char *ID3TAG_COMPOSER_Descriptor[2] 	= {"TCOM", "TCM"};
const char *ID3TAG_AUTHOR_Descriptor[2] 	= {"TEXT", "TXT"};
const char *ID3TAG_LENGTH_Descriptor[2] 	= {"TLEN", "TLE"};
const char *ID3TAG_TRACK_Descriptor[2]  	= {"TRCK", "TRK"};
const char *ID3TAG_DISCNUMBER_Descriptor[2]  	= {"TPOS", "TPA"};
const char *ID3TAG_CONTENT_Descriptor[2] 	= {"TCON", "TCO"};
const char *ID3TAG_YEAR_Descriptor[2]  		= {"TYER", "TYE"};
const char *ID3TAG_TDRC_Descriptor[2]  		= {"TDRC", "^^^"};
const char *ID3TAG_TENC_Descriptor[2]  		= {"TENC", "^^^"};
const char *ID3TAG_COMPILATION_Descriptor[2]	= {"TCMP", "TCP"}; // non standard, used by itunes.

#define GENRE_NUM 148
static const char* genre_table[GENRE_NUM] = {	
"Blues",	"Classic Rock",		"Country",		"Dance",	"Disco",
"Funk",		"Grunge",		"Hip-Hop",		"Jazz",		"Metal",
"New Age",	"Oldies",		"Other",		"Pop",		"R&B",
"Rap",		"Reggae",		"Rock",			"Techno",	"Industrial",
"Alternative",	"Ska",			"Death Metal",		"Pranks",	"Soundtrack",
"Euro-Techno",	"Ambient",		"Trip-Hop",		"Vocal",	"Jazz+Funk",
"Fusion",	"Trance",		"Classical",		"Instrumental",	"Acid",
"House",	"Game",			"Sound Clip",		"Gospel",	"Noise",
"AlternRock",	"Bass",			"Soul",			"Punk",		"Space",
"Meditative",	"Instrum. Pop",		"Instrum. Rock",	"Ethnic",	"Gothic",
"Darkwave",	"Techno-Industr.",	"Electronic",		"Pop-Folk",	"Eurodance",
"Dream",	"Southern Rock",	"Comedy",		"Cult",		"Gangsta",
"Top 40",	"Christian Rap",	"Pop/Funk",		"Jungle",	"Native American",
"Cabaret",	"New Wave",		"Psychadelic",		"Rave",		"Showtunes",
"Trailer",	"Lo-Fi",		"Tribal",		"Acid Punk",	"Acid Jazz",
"Polka",	"Retro",		"Musical",		"Rock & Roll",	"Hard Rock",
"Folk",		"Folk-Rock",		"National Folk",	"Swing",	"Fast Fusion",
"Bebob",	"Latin",		"Revival",		"Celtic",	"Bluegrass",
"Avantgarde",	"Gothic Rock",		"Progress. Rock",	"Psychedel. Rock",	"Symphonic Rock",
"Slow Rock",	"Big Band",		"Chorus",		"Easy Listening",	"Acoustic",
"Humour",	"Speech",		"Chanson",		"Opera",	"Chamber Music",
"Sonata",	"Symphony",		"Booty Bass",		"Primus",	"Porn Groove",
"Satire",	"Slow Jam",		"Club",			"Tango",	"Samba",
"Folklore",	"Ballad",		"Power Ballad",		"Rhythmic Soul","Freestyle",
"Duet",		"Punk Rock",		"Drum Solo",		"Acapella",	"Euro-House",
"Dance Hall",	"Goa",			"Drum & Bass",		"Club-House",	"Hardcore",
"Terror",	"Indie",		"BritPop",		"Negerpunk",	"Polsk Punk",
"Beat",		"Christ. Gangsta Rap",	"Heavy Metal",		"Black Metal",	"Crossover",
"Contemp. Christian","Christian Rock",	"Merengue",		"Salsa",	"Trash Metal",
"Anime",	"Jpop",			"Synthpop"
};

// ****************************************************************************
//
// 	ID3_get_genre
//
// ****************************************************************************
const char *ID3_get_genre( int genre )
{
	if( genre < 0 || genre > (GENRE_NUM - 1) ) {
		return NULL;
	}
	return genre_table[genre];
}

// ****************************************************************************
//
// 	ID3_set_get_genre_from_ID
//
// ****************************************************************************
void ID3_set_get_genre_from_ID( char *genre )
{
	// check for genre number given in ASCII "(xxx)" and replace with real genre name
	if ( (genre[0] == '(') && (genre[1] >= '1') && (genre[1] <= '9') ) {
		int num = atoi( genre + 1 );
		if( num >= 0 && num < GENRE_NUM ) {
DBG serprintf("replace Genre %s [%d] with %s \r\n", genre, num, genre_table[num] );		
			strcpy( genre, genre_table[num] );
		}
	}
}

// ****************************************************************************
//
// 	_peek
//
//	Get a byte from the ID3TAG buffer at "offset",
//	does not move the read pointer
//
// ****************************************************************************
static unsigned char _peek(ID3_CTX *c, int offset)
{
	int tmp = c->buf_read + offset;
	if (tmp >= c->buffer_size)
		tmp -= c->buffer_size;
		
	return c->buffer[tmp];
}

// ****************************************************************************
//
// 	_get_p
//
//	Get a pointer to the ID3TAG buffer at "offset",
//
// ****************************************************************************
static unsigned char *_get_p(ID3_CTX *c, int offset)
{
	int tmp = c->buf_read + offset;
	if (tmp >= c->buffer_size)
		tmp -= c->buffer_size;
		
	return c->buffer + tmp;
}

// ****************************************************************************
//
// 	_skip
//
// 	move read pointer in the ID3TAG buffer
//
// ****************************************************************************
static void _skip(ID3_CTX *c, int number)
{
	if ( number > c->toRead )
		number = c->toRead;
	c->toRead -= number;
	
	c->buf_read += number;
	if (c->buf_read >= c->buffer_size)
		c->buf_read -= c->buffer_size;
}

// ****************************************************************************
//
// 	GetEntry
//
//	copy "max" chars to entry
//
// ****************************************************************************
static int GetEntry(ID3_CTX *c, unsigned int flen, int start, char *entry, int max  )
{
#define BOM_LE	0xFFFE
#define BOM_BE	0xFEFF

	int written = 0;
	
	wchar unicode[MAX_TAG_LENGTH + 1];
	wchar *uc = unicode;

	// Fill the unused part of the tag with 0
	memset( entry, 0, max );
	
	// Check text encoding format
	int enc = _peek(c, start);
DBG serprintf("\tEntry: len %d  enc %02X  start %d: ", flen, enc, start + 1 );
	
	if ( enc == 0x00 ) {
		// Plain ASCII
		//---------------------------------
		// 8-bits ASCII text encoding
		// ( x1 x2 x3 ... )
		//---------------------------------
		int i = start + 1; 
		while ( i < flen ) {
#ifdef CONFIG_I18N
			// take care about wide codepage chars!
			char t[2];
			t[0] = _peek(c, i);
			t[1] = _peek(c, i + 1);
			
			i += I18N_codepage_to_unicode( t, uc );
			uc++; 
#else
			*uc++ = _peek(c, i);
			i++;
#endif
			if (++written >= max) {
				break;
			}
		}
		*uc = '\0';
		// convert utf16 to utf8:
		utf16_to_utf8( (UCHAR*)entry, unicode, max );
		
	} else {
		if( enc == 0x01 || enc == 0x02 ) {
			// UNICODE!
			USHORT bom;
			int i;
			if ( enc == 0x01 ) {
				bom = (_peek(c, start + 1) << 8) | _peek(c, start + 2);
				i = start + 3;
			} else {
				bom = BOM_BE;
				i = start + 1;
			} 
		
			while ( i < flen ) {
				if ( bom == BOM_BE )
					*uc++ = (_peek(c, i) << 8) | _peek(c, i + 1);
				else
					*uc++ = (_peek(c, i + 1) << 8) | _peek(c, i);

				if (++written == max) {
					break;
				}
				i += 2;
			}
			*uc = '\0';
			// convert utf16 to utf8:
			utf16_to_utf8( (UCHAR*)entry, unicode, max );
		} else {
			// UTF8
			char *e = entry;
			int i;
			for( i = 0; i < MIN( max, flen - start - 1); i++ ) {
				*e++ = _peek( c, start + 1 + i );
			}
			*e = '\0';
		}	
	}

DBG serprintf("%s\n", entry);

	return 0;
}

// ****************************************************************************
//
// 	_get_size
//
// ****************************************************************************
static unsigned int _get_size( unsigned char *buffer, int *flags )
{
	unsigned int size = 0;
DBG serprintf("CID3V2");
	// Check for ID3V2 Tag
	if ( buffer[0] == 'I' ) {
	  if ( buffer[1] == 'D' ) {
	    if ( buffer[2] == '3' ) {
	    	if( flags )
			*flags = buffer[5];
		
		size =      (unsigned int) buffer[9]
			| ( (unsigned int) buffer[8] <<  7 )
			| ( (unsigned int) buffer[7] << 14 )
			| ( (unsigned int) buffer[6] << 21 );
		size += 10;
DBG serprintf(" size %d", size);
	    }
	  } 
	}
DBG serprintf("\n");		
	return size;	
}

// ****************************************************************************
//
// 	ID3V2_GetSize
//
// ****************************************************************************
unsigned int ID3V2_GetSize( unsigned char *buffer )
{
	return _get_size( buffer, NULL );
}

// ****************************************************************************
//
//	ID3V2_Parse
//
//	OUT	ID3_TAG *tag		where to store the tag data 
//	IN	_buffer,		(circular) buffer with tag data 
//	IN	_buffer_size		size of buffer
//	IN	_buf_read		read pointer in buffer
//	OUT	APIC *apic		where to store the APIC, if NULL no apic extracted
///
// ****************************************************************************
int ID3V2_Parse(	ID3_TAG *tag, 
			unsigned char *_buffer, unsigned int _buffer_size, volatile int _buf_read, 
			APIC *apic )
{
	int 	tagfound = 0;
	int	flags;
	int     buf_read0 = _buf_read;
	
	ID3_CTX _c;
	ID3_CTX *c = &_c;
	
	c->buffer      = _buffer;
	c->buffer_size = _buffer_size;
	c->buf_read    = _buf_read;
	c->toRead      = _get_size( c->buffer + c->buf_read, &flags );

	if( !c->toRead ) {
serprintf("no ID3V2 tag!\r\n");
		return 1;	
	} 

	if( apic )
		apic->valid = 0;
		
 	// get ID3V2 version
	int v2_ver = _peek( c, 3 );
DBG serprintf("ID3V2.%d_Parse\r\n", v2_ver );

	int hdr_len = 10;
	int fr_id   = 0;
	int fr_len  = 4;
	if( v2_ver == 2 ) {
		hdr_len = 6;
		fr_id   = 1;
		fr_len  = 3;
	}
		
	// skip the ID3v2 header
	_skip( c, 10 );
	
	// skip a potential extended header
	if ( flags & 0x40 ) {
		int size  =  (unsigned int) _peek( c, 3 )
			 | ( (unsigned int) _peek( c, 2 ) <<  7 )
			 | ( (unsigned int) _peek( c, 1 ) << 14 )
			 | ( (unsigned int) _peek( c, 0 ) << 21 );
		_skip( c, size );
	}


	// parse the frames of the ID3v2 tag
	while ( c->toRead >= 8 ) {
		unsigned int 	frame_size;
		unsigned char 	frame[4];
	
		frame[0] = _peek( c, 0 );
		frame[1] = _peek( c, 1 );
		frame[2] = _peek( c, 2 );
		frame[3] = _peek( c, 3 );
		
		if (!memcmp( frame, "\000\000\000\000", 4)) {
DBG serprintf("padding, rest %d\r\n", c->toRead);		
			// _skip( toRead ); // we skip remaining bytes after the loop
			break;
		}
		
		if( v2_ver >= 4 ) {
			// 2.4 has "sync safe" integers, top bit is always 0
			frame_size =   	 (unsigned int) (_peek( c, 7 ) & 0x7F )
				     | ( (unsigned int) (_peek( c, 6 ) & 0x7F ) <<  7 )
				     | ( (unsigned int) (_peek( c, 5 ) & 0x7F ) << 14 )
				     | ( (unsigned int) (_peek( c, 4 ) & 0x7F ) << 21 );
			     
		} else if( v2_ver >= 3 ) {
			frame_size =   	 (unsigned int) _peek( c, 7 )
				     | ( (unsigned int) _peek( c, 6 ) <<  8 )
				     | ( (unsigned int) _peek( c, 5 ) << 16 )
				     | ( (unsigned int) _peek( c, 4 ) << 24 );
			     
		} else {
			frame_size =   	 (unsigned int) _peek( c, 5 )
				     | ( (unsigned int) _peek( c, 4 ) <<  8 )
				     | ( (unsigned int) _peek( c, 3 ) << 16 );
		}
		frame_size += hdr_len;
	
		// make sure the frame size makes sense!
		if( frame_size > c->toRead ) {
			frame_size = c->toRead;
		}
DBG serprintf("[%.4s]  at %6d  size %6d  rest %3d\r\n", frame, c->buf_read - buf_read0, frame_size, c->toRead );
		if (!memcmp( frame, ID3TAG_TITLE_Descriptor[fr_id], fr_len)) {
			// Get the title of the track
			GetEntry( c, frame_size, hdr_len, tag->title, MAX_TAG_LENGTH );
			tagfound = 1;
		} else 	if (!memcmp( frame, ID3TAG_ARTIST_Descriptor[fr_id], fr_len)) {
			// Get the artist name of the track
			GetEntry( c, frame_size, hdr_len, tag->artist, MAX_TAG_LENGTH );
			tagfound = 1;
		} else 	if (!memcmp( frame, ID3TAG_ALBUM_Descriptor[fr_id], fr_len)) {
			// Get the album name of the track
			GetEntry( c, frame_size, hdr_len, tag->album, MAX_TAG_LENGTH );
			tagfound = 1;
		} else 	if (!memcmp( frame, ID3TAG_ALBUM_ARTIST_Descriptor[fr_id], fr_len)) {
			GetEntry( c, frame_size, hdr_len, tag->album_artist, MAX_TAG_LENGTH );
			tagfound = 1;
		} else 	if (!memcmp( frame, ID3TAG_COMPOSER_Descriptor[fr_id], fr_len)) {
			GetEntry( c, frame_size, hdr_len, tag->composer, MAX_TAG_LENGTH );
			tagfound = 1;
		} else 	if (!memcmp( frame, ID3TAG_AUTHOR_Descriptor[fr_id], fr_len)) {
			GetEntry( c, frame_size, hdr_len, tag->author, MAX_TAG_LENGTH );
			tagfound = 1;
		} else 	if (!memcmp( frame, ID3TAG_YEAR_Descriptor[fr_id], fr_len)) {
			// Get the year of the track
			GetEntry( c, frame_size, hdr_len, tag->year, ID3_YEAR_LENGTH );
			tagfound = 1;
		} else 	if (!memcmp( frame, ID3TAG_TDRC_Descriptor[fr_id], fr_len)) {
			// Get the recording date (only 1st 4 chars = year)
			GetEntry( c, frame_size, hdr_len, tag->year, ID3_YEAR_LENGTH );
			tagfound = 1;
		} else 	if (!memcmp( frame, ID3TAG_TENC_Descriptor[fr_id], fr_len)) {
			// Get the TENC, will only show up in the DBGP logs though
			char tenc[128+1];
			GetEntry( c, frame_size, hdr_len, tenc, sizeof( tenc ) - 1 );
		} else 	if (!memcmp( frame, ID3TAG_CONTENT_Descriptor[fr_id], fr_len)) {
			// Get the genre name of the track
			GetEntry( c, frame_size, hdr_len, tag->genre, MAX_TAG_LENGTH );

			ID3_set_get_genre_from_ID( tag->genre );
			tagfound = 1;
		} else 	if (!memcmp( frame, ID3TAG_LENGTH_Descriptor[fr_id], fr_len)) {
			// Get the length of the track
			char msec[MAX_TAG_LENGTH];
			GetEntry( c, frame_size, hdr_len, msec, MAX_TAG_LENGTH );
			tag->msec = atoi( msec );
DBG serprintf("\tTLEN: %d\r\n", tag->msec );		
		} else 	if (!memcmp( frame, ID3TAG_TRACK_Descriptor[fr_id], fr_len)) {
			// Get the track number
			char track[MAX_TAG_LENGTH];
			GetEntry( c, frame_size, hdr_len, track, MAX_TAG_LENGTH );
			tag->track = atoi( track );
DBG serprintf("\tTRCK: %02d\r\n", tag->track );		
		} else 	if (!memcmp( frame, ID3TAG_DISCNUMBER_Descriptor[fr_id], fr_len)) {
			// Get the disc number
			char disc[MAX_TAG_LENGTH];
			GetEntry( c, frame_size, hdr_len, disc, MAX_TAG_LENGTH );
			tag->discnumber = atoi( disc );
DBG serprintf("\tTPOS: %02d\r\n", tag->discnumber );
		} else 	if (!memcmp( frame, ID3TAG_COMPILATION_Descriptor[fr_id], fr_len)) {
			GetEntry( c, frame_size, hdr_len, tag->compilation, MAX_TAG_LENGTH );
			tagfound = 1;
		} else 	if (!memcmp( frame, ID3TAG_APIC_Descriptor[fr_id], fr_len)) {
			// Get the APIC of the track
			int type;
			int pos = 0;
			int etype;

			UCHAR *p = _get_p( c, hdr_len + 1 );
			if ( v2_ver >= 3 ) {
				if( !get_file_type_from_mime_type( p, NULL, &etype ) ) {
					pos  = hdr_len + strlen( p ) + 3;
					type = _peek( c, pos - 1 );
				}
			} else if ( v2_ver == 2 ) {
				// Header: "PIC" +  (Text encoding $xx) + (image type) +
				// (Picture type  $xx) + ( <description text> $00 ) + <binary picture data> 
				char ext[4];
				ext[0] = _peek(c, hdr_len + 1);
				ext[1] = _peek(c, hdr_len + 2);
				ext[2] = _peek(c, hdr_len + 3);
				ext[3] = '\0';
				if( !get_file_type_from_ext( ext, NULL, &etype, NULL, NULL ) ) { 
					pos  = hdr_len + 5;
					type = _peek( c, pos - 1 );
				}
			}

			if ( !pos ) {
DBG Dump( _get_p( c, 0 ), 32 );
			} else {
				// skip a possible description text
				while ( _peek(c, pos) && pos < frame_size ) {
					pos++;
				}
				// skip 0x00 
				pos++;
				// remember the image size
				int size = frame_size - pos;
DBG serprintf("\tAPIC at %d  type %d  size %d  etype %d\r\n", pos, type, size, etype );				

				if( apic && !apic->valid && size > 4 && size <= apic->buffer_size ) {
DBG serprintf("\tAPIC at %d  type %d  size %d/%d  etype %d\r\n", pos, type, size, apic->buffer_size, etype );				
					if( !apic->buffer ) {
						// we need to alloc the buffer here
						apic->buffer_size = size;
						apic->buffer = amalloc( apic->buffer_size );
					}
					if( apic->buffer ) {
						UCHAR *buf = apic->buffer;
						// copy image to jpg buffer
						while( pos < frame_size ) {
							*buf++ = _peek(c, pos++);
						}

						tagfound    = 1;
						tag->apic   = 1;
						apic->size  = size;
						apic->etype = etype;
						apic->valid = 1;
					}
				} else {
					if( apic && apic->valid ) {
DBG serprintf("\tignored\n");					
					} else {
//DBG serprintf("\ttoo small/large %d/%d\n", size, apic->buffer_size);					
					}
					size = 0;
				}
			}
		} else {
			// We don't recognize any valid frame (it might also be padding or footer)
			//size = 1;
		}	
		
		_skip( c, frame_size );
	}

	if ( c->toRead > 0 )
		_skip( c, c->toRead );

	if ( tagfound ) {
		tag->id3_ver = 2;
		tag->valid = 1;
	}
	return tagfound;
}

// ****************************************************************************
//
// 	ID3_show_tag
//
// ****************************************************************************
void ID3_show_tag( ID3_TAG *tag )
{
	serprintf("ART: [%s]\r\n", tag->artist );		
	serprintf("ALB: [%s]\r\n", tag->album );		
	serprintf("TIT: [%s]\r\n", tag->title );		
	serprintf("GNR: [%s]\r\n", tag->genre );
	serprintf("YER: [%s]\r\n", tag->year );
	serprintf("TRK: [%d]\r\n", tag->track );
}

// ****************************************************************************
//
// 	ID3V1_GetSize
//
// ****************************************************************************
unsigned int ID3V1_GetSize( unsigned char *buffer )
{
	if ( !strncmp( buffer, "TAG", 3 ) )
		return 128;
	else
		return 0;
}

// ****************************************************************************
//
// 	fix_encoding
//
// ****************************************************************************
static void fix_encoding( char *tag, int len )
{
#ifdef CONFIG_I18N
	unsigned char *c = (UCHAR*)tag;
	// copy ASCII tag to UTF-8, taking codepage into account
	I18N_codepage_to_utf8( tag, c, MAX_TAG_LENGTH );
#endif
	return; 
}

// ****************************************************************************
//
//	ID3V1_Parse
//
//	OUT	tag	where to store tag data
//	IN	buffer	buffer with 128 bytes tag data
//
// ****************************************************************************
int ID3V1_Parse(ID3_TAG *tag, unsigned char *buffer )
{
DBG serprintf("ID3V1_Parse\n");
	
	memset( tag, 0, sizeof(ID3_TAG) );
	
	if ( !strncmp( buffer, "TAG", 3 ) ) {
		unsigned char genre;
		memcpy ( tag->title,   buffer +  3, 30);
		fix_encoding( tag->title, 30 );

		memcpy ( tag->artist,  buffer + 33, 30);
		fix_encoding( tag->artist, 30 );
		
		memcpy ( tag->album,   buffer + 63, 30);
		fix_encoding( tag->album, 30 );
		
		memcpy ( tag->year,    buffer + 93,  4);
		strnZcpy( tag->comment, buffer + 97, 30 );
		if ( buffer[125] == 0 ) // ID3V1.1?
			tag->track = buffer[126];

		genre = buffer[127];

		if ( genre < GENRE_NUM ) {
			strcpy( tag->genre, genre_table[ genre ] );
		} else {
			strcpy( tag->genre, "" );
		}
DBG ID3_show_tag( tag );
		
		tag->id3_ver = 1;
		tag->valid   = 1;
		tag->apic    = 0;
		tag->msec    = 0;
		return 0;
	}
	return 1;
}
#endif

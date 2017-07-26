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
#include "debug.h"
#include "subtitle_format.h"
#include "stream_subtitle.h"
#include "util.h"
#include "astdlib.h"
#include "browse.h"
#include "iso639.h"

#include <ctype.h>
#include <string.h>
#include <stdint.h>

#define DBG  if(Debug[DBG_SUB])
#define DBG2 if(Debug[DBG_SUB] > 1)

char *subtitle_get_next_line( char *start, int len, FILE *fd );

static int detect_IDX( FILE * file )
{
	// make sure that read starts at the beginning of file
	fseek( file, 0, SEEK_SET );
 
	char _line[ LINE_LEN + 1 ];
	char* line = _line;
	line = subtitle_get_next_line( line, LINE_LEN, file );

	if ( line && strstr( line, "VobSub index file" ) ) {
DBG serprintf( "IDX: found!\n" );
		return 0;
	}

DBG printf( "IDX: not IDX\n" );
	return 1;

}

static sub_coding_style **info_IDX( FILE * file, int *cnt, uint32_t *palette, int *has_palette )
{
DBG serprintf( "IDX: info\n" );
	int styles = 0;
	sub_coding_style **style_arr = NULL;

	//make sure that reading start from the begining of file
	fseek( file, 0, SEEK_SET );

	char _line[LINE_LEN + 1];
	char* line = _line;

	line = subtitle_get_next_line( line, LINE_LEN, file );

	while ( line ) {
		if( !strncmp( line, "langidx:", strlen("langidx:") ) ) {
DBG serprintf("%s", line );
		} else if( !strncmp( line, "time offset:", strlen("time offset:") ) ) {
DBG serprintf("%s", line );
		} else if( !strncmp( line, "palette: ", strlen("palette: ") ) ) {
DBG serprintf("%s", line );
			if( has_palette )
				*has_palette = 1;
			if( palette ) {
				sscanf(line + strlen("palette: "), "%06X, %06X, %06X, %06X, %06X, %06X, %06X, %06X, %06X, %06X, %06X, %06X, %06X, %06X, %06X, %06X", 
				&palette[0],  &palette[1],  &palette[2],  &palette[3],
				&palette[4],  &palette[5],  &palette[6],  &palette[7],
				&palette[8],  &palette[9],  &palette[10], &palette[11],
				&palette[12], &palette[13], &palette[14], &palette[15] );
DBG { int i; for( i = 0; i < 16; i++ ) serprintf("%06X ", palette[i] ); serprintf("\n"); }
			}
		} else if( !strncmp( line, "id:", strlen("id:") ) ) {
DBG serprintf("%s", line );
			char id[LINE_LEN + 1];
			if( sscanf( line, "id: %2s", id ) == 1 ) {
				sub_coding_style *style = acalloc(1, sizeof( sub_coding_style ) );

				style->id = astrdup(id);

				const char *code;
				if( id[0] && ( code = map_ISO639_code( id ) ) != id && code[0] != '\0' ) {
					style->lang = astrdup( code );
					style->name = astrdup( code );
				} else {				
					style->lang = astrdup(id);
					style->name = astrdup(id);
				}
DBG serprintf( "lang: '%s' '%s' '%s'\n", style->id, style->name, style->lang );

				if(style){
					style_arr = arealloc(style_arr,(styles + 1) * sizeof(sub_coding_style*));
					style_arr[styles++] = style;
				}
			}
		}
		line = subtitle_get_next_line( line,  LINE_LEN , file );
	}
	*cnt = styles;

	return style_arr;
}

static void store_line(char *line, sub_line *sub)
{
	sub->top = acalloc(strlen(line) + 1, 1);
	strcpy(sub->top,line);
}

static int get_timestamp_and_pos( const char *line, uint32_t *pos ) 
{
	int h, m, s, ms;
	
	if( sscanf(line,"timestamp: %u:%u:%u:%u, filepos: %X", &h, &m, &s, &ms, pos) != 5) {
		*pos = 0;
		return -1;
	}
	
	return ms + 1000 * (s + m * 60 + h * 3600 );
}

static uni_sub *parse_IDX( subt_orig *spex, int clean_tags )
{
DBG serprintf( "IDX: parse [%s]  %s %s\n", spex->filename, spex->language_name, spex->default_language );
	if ( !spex ) {
serprintf( "IDX: Invalid parameter\n" );
		return 0;
	}

	if ( !spex->filename ) {
serprintf( "IDX: Invalid filename\n" );
		return 0;
	}
	
	FILE *fd = fopen( spex->filename, "r" );
	if ( !fd ) {
serprintf( "IDX: cannot read %s\n", spex->filename );
		return 0;
	}

	// try to open the .sub file:
	char subname[MAX_NAME_LEN + 1];
	cut_n_extension_r( spex->filename, subname, MAX_NAME_LEN );
	strcat(subname, ".sub" );
	int vobsub_fd = file_open( subname, O_RDONLY, 0 );
	if( vobsub_fd < 0 ) {
serprintf( "IDX: could not read file %s\n", subname );
		return 0;
	}

	uni_sub *sub = acalloc(1, sizeof( uni_sub ) );
	sub->vobsub = 1;
	sub->vobsub_fd = vobsub_fd;
	
	sub->vobsub_data = NULL;
	
	sub->format = spex->format;
	
	char _line[LINE_LEN + 1];
	char* line = _line;
	int parse = 0;
	line = subtitle_get_next_line( line, LINE_LEN, fd );

	while ( line ) {
		if( !strncmp( line, "time offset:", strlen("time offset:") ) ) {
DBG serprintf("%s", line );
		} else if( !strncmp( line, "id:", strlen("id:") ) ) {
DBG serprintf("%s", line );
			char id[LINE_LEN + 1];
			if( (sscanf( line, "id: %2s", id ) == 1) && !strcmp( id, spex->default_language ) ) {
DBG serprintf("start\n");			
				parse = 1;
			} else if( parse ) {
DBG serprintf("stop\n");			
				break;
			}
		} else if( parse && !strncmp( line, "timestamp:", strlen("timestamp:") ) ) {
			uint32_t pos;
			int timestamp = get_timestamp_and_pos( line, &pos );
DBG serprintf("time: %8d  %08X\n", timestamp, pos );
			sub_line *new = acalloc(1, sizeof( sub_line ) );
			new->start = timestamp;
			new->end   = timestamp + 100;	// set 100ms, vobsub decoder will override it
			new->pos   = pos;
			sprintf(line, "time: %d  pos: %d", timestamp, pos );
			store_line(line, new);
			if ( sub->first == 0 ) {
				sub->first = new;
				sub->last  = new;
			} else {
				sub->last->next = new;
				new->prev       = sub->last;
				sub->last       = new;
			}
		}
		line = subtitle_get_next_line( line,  LINE_LEN , fd );
	}
	fclose(fd);
	if( spex->has_palette ) {
serprintf("IDX has palette!\n");
		sub->has_palette = spex->has_palette;
		memcpy( sub->palette, spex->palette, sizeof( sub->palette ) );
	} 

	return sub;
}

static int parse_mpeg( unsigned char *data, int max, unsigned char *out, int *size )
{
	int pos = 0;
	int vs_size = 0;
	int out_max  = *size;
	int out_size = 0;
	int out_pts  = -1;
	uint32_t head = 0;

	*size = 0;

	while(pos < max) {
		head = (head << 8) | *data++; pos ++;
//printf("[%8d] head: %08X\n", pos - 4, head );
		switch( head ) {
		case 0x000001B9:
DBG2 serprintf("end\n");
			return 1;
		case 0x000001BA:
DBG2 serprintf("start\n");
			break;
		case 0x000001BD:
		{	
			int pkt_size = *data++; pos++;
			pkt_size = (pkt_size << 8) | *data++; pos++;
DBG2 serprintf("packet: size %08X %d\n", pkt_size, pkt_size);
			int hdr = 0;
			int c = *data++; pos++;
			if( (c & 0xC0) == 0x40 ) {
DBG2 serprintf("(4)");		
				c = *data++; pos++;
				c = *data++; pos++;
				hdr += 2;				
			}
			if ((c & 0xf0) == 0x20) {
DBG serprintf("(2)");		
				return 1;
			} else if ((c & 0xf0) == 0x30) {
DBG2 serprintf("(3)");		
				return 1;
			} else if( ( c & 0xC0 ) == 0x80 ) {
DBG2 serprintf("(8)");		
				int flags = *data++; pos++;
				int len   = *data++; pos++;
				int pts = -1;
DBG2 serprintf("(len %d)", len);
				hdr += len + 3;
				if ((flags & 0xc0) == 0x80) {
DBG2 serprintf("(pts)");
					unsigned char buf[5];
					int i;
					for( i = 0; i < 5; i++ ) {
						buf[i] = *data++; pos++; 
						len--;
					}
					if (!(((buf[0] & 0xf0) == 0x20) && (buf[0] & 1) && (buf[2] & 1) &&  (buf[4] & 1))) {
DBG2 serprintf("IDX: PTS error 0x%02x %02x%02x %02x%02x \n", buf[0], buf[1], buf[2], buf[3], buf[4]);
						pts = 0;
					} else {
						pts = ( (buf[0] & 0x0e) << 29 | 
							 buf[1] << 22 | 
							(buf[2] & 0xfe) << 14 | 
							 buf[3] << 7 | 
							(buf[4] >> 1));
					}
DBG2 serprintf("(pts %d)", pts);
				} else {
DBG2 serprintf("(no pts)");
				}

				// skip the rest
DBG2 printf("(skip %d)", len);
				data += len;
				pos  += len;

				if( pts != -1 ) {
					if( out_pts == -1 ) {
						out_pts = pts;
					} else if( pts != out_pts ) {
						// pts changed, we are out here
						pos = max;
						break;
					}
				}
			}
			// skip aid!
			c = *data++; pos++;
			int size = pkt_size - hdr - 1;
DBG2 serprintf("(size %d)", size);
			int copy = MIN( out_max - out_size, size );
			memcpy( out, data, copy ); 
			data += size;
			out_size += copy;
			if( copy < size ) {
				// out_max reached, we are out here
				pos = max;
				break;
			}

			if( !vs_size ) {
				vs_size = out[1] | (out[0] << 8);
DBG2 serprintf("(vs_size %d)", vs_size );
			}
			pos      += size;
			out      += size;
DBG2 serprintf("pos %d\n", pos );
			break;
		}
			break;
		case 0x000001BE:
DBG2 serprintf("padding\n");
			break;
			return 1;
		}
	}

DBG2 serprintf("(done %d)\n", out_size);
	*size = out_size;
	return 0;
}

#define VOBSUB_DATA	(128*1024)
#define VOBSUB_CHUNK	SUBTITLE_CHUNK

static int get_gfx_IDX( uni_sub *sub, uint32_t pos, uint8_t *data, int *size )
{
	int fd = sub->vobsub_fd;
DBG serprintf("get_gfx_IDX: fd %d  pos %d\n", fd, pos );
	
	if( !sub->vobsub_data ) {
		sub->vobsub_data = amalloc( VOBSUB_DATA );
		sub->vobsub_size = 0;
		sub->vobsub_pos  = 0;
	}
	
	// check if our pos is buffered:
	int rebuffer = 0;
	if( pos < sub->vobsub_pos ) {
		rebuffer = 1;
	} else if( pos + VOBSUB_CHUNK > sub->vobsub_pos + sub->vobsub_size ) {
		rebuffer = 1;
	} 
	if( rebuffer ) {
		file_seek( fd, pos, SEEK_SET );
		sub->vobsub_size = file_read( fd, sub->vobsub_data, VOBSUB_DATA );
		sub->vobsub_pos	= pos;
DBG2 serprintf("rebuffer %8d got %6d\n", pos, sub->vobsub_size );
	}
	int offset = pos - sub->vobsub_pos;
DBG2 serprintf("pos %8d offset: %6d\n", pos, offset );

	if( !parse_mpeg( sub->vobsub_data + offset, VOBSUB_CHUNK, data, size ) ) {
		return 0;
	}

	return 1;
}

static int close_IDX( uni_sub *sub )
{
	int fd = sub->vobsub_fd;
DBG serprintf("close_IDX: fd %d\n", fd );
	if( sub->vobsub_fd ) {
		file_close( sub->vobsub_fd );
	}
	if( sub->vobsub_data ) {
		afree( sub->vobsub_data ); 
	}
	return 0;
}

static struct SUBTITLE_FORMAT IDX = {
	"IDX/SUB",
	detect_IDX,
	info_IDX,
	parse_IDX,
	get_gfx_IDX,
	close_IDX,
};

SUBTITLE_REGISTER_FORMAT( IDX );


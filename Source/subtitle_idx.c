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
#include <limits.h>

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

static int get_timestamp_and_pos(const char *line, uint32_t *pos)
{
	unsigned h, m, s, ms;
	if (sscanf(line, "timestamp: %u:%u:%u:%u, filepos: %X", &h, &m, &s, &ms, pos) != 5 ||
	    m >= 60 || s >= 60 || ms >= 1000) return -1;
	int64_t time = ((int64_t)h * 3600 + m * 60 + s) * 1000 + ms;
	return time < INT_MAX ? (int)time : -1;
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
	// UTF-16 conversion changes the IDX path, not its companion SUB path.
	cut_n_extension_r(spex->org_name ? spex->org_name : spex->filename,
		subname, MAX_NAME_LEN - 4);
	strcat(subname, ".sub" );
	int vobsub_fd = file_open( subname, O_RDONLY, 0 );
	if( vobsub_fd < 0 ) {
serprintf( "IDX: could not read file %s\n", subname );
		fclose(fd);
		return 0;
	}

	uni_sub *sub = acalloc(1, sizeof( uni_sub ) );
	if (!sub) { fclose(fd); file_close(vobsub_fd); return NULL; }
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
			if (timestamp < 0 || (sub->last && timestamp < sub->last->start)) {
				line = subtitle_get_next_line(line, LINE_LEN, fd);
				continue;
			}
			sub_line *new = acalloc(1, sizeof( sub_line ) );
			if (!new) break;
			new->start = timestamp;
			// Lookup bounds only: decode the previous SPU when seeking into it.
			// Its real display duration comes from the DVD control sequence.
			new->end = INT_MAX;
			if (sub->last) sub->last->end = timestamp;
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

/* Assemble exactly one complete DVD SPU, never a truncated PES payload. */
static int parse_mpeg(const unsigned char *data, int max, unsigned char *out, int *size)
{
	int capacity = *size, written = 0, expected = 0, aid = -1;
	int64_t first_pts = -1;
	*size = 0;
	if (!data || !out || max <= 0 || capacity < 2)
		return 1;
	const unsigned char *end = data + max;
	while (end - data >= 4) {
		if (data[0] || data[1] || data[2] != 1) {
			data++;
			continue;
		}
		int code = data[3];
		data += 4;
		if (code == 0xb9)
			break;
		if (code == 0xba) {
			if (end - data < 1) return 1;
			int len;
			if ((data[0] & 0xc0) == 0x40) {
				if (end - data < 10) return 1;
				len = 10 + (data[9] & 7);
			} else if ((data[0] & 0xf0) == 0x20) {
				len = 8;
			} else return 1;
			if (end - data < len) return 1;
			data += len;
			continue;
		}
		if (end - data < 2) return 1;
		int length = (data[0] << 8) | data[1];
		data += 2;
		if (length > end - data) return 1;
		const unsigned char *packet_end = data + length;
		if (code != 0xbd) {
			data = packet_end;
			continue;
		}
		// IDX/SUB carries MPEG-2 private_stream_1 PES packets.
		if (length < 4 || (data[0] & 0xc0) != 0x80) return 1;
		int flags = data[1], header = data[2];
		if (header > length - 4) return 1; // reserve the substream id
		int64_t pts = -1;
		if (flags & 0x80) {
			if (header < ((flags & 0x40) ? 10 : 5)) return 1;
			const unsigned char *q = data + 3;
			if ((q[0] >> 4) != ((flags & 0x40) ? 3 : 2) ||
			    !(q[0] & 1) || !(q[2] & 1) || !(q[4] & 1)) return 1;
			pts = ((int64_t)(q[0] & 0x0e) << 29) | ((int64_t)q[1] << 22) |
			      ((int64_t)(q[2] & 0xfe) << 14) | (q[3] << 7) | (q[4] >> 1);
		}
		data += 3 + header;
		int stream = *data++;
		if (stream < 0x20 || stream > 0x3f || (aid >= 0 && stream != aid)) {
			data = packet_end;
			continue;
		}
		aid = stream;
		if (pts >= 0) {
			if (first_pts >= 0 && pts != first_pts) return 1;
			first_pts = pts;
		}
		int payload = packet_end - data;
		while (payload > 0) {
			int copy = MIN(payload, expected ? expected - written : 2 - written);
			if (copy <= 0 || copy > capacity - written) return 1;
			memcpy(out + written, data, copy);
			written += copy;
			data += copy;
			payload -= copy;
			if (!expected && written == 2) {
				expected = (out[0] << 8) | out[1];
				if (expected < 4 || expected > capacity) return 1;
			}
			if (expected && written == expected) {
				*size = written;
				return 0;
			}
		}
		data = packet_end;
	}
	return 1;
}

#define VOBSUB_DATA	(128*1024)
#define VOBSUB_CHUNK	SUBTITLE_CHUNK

static int get_gfx_IDX(uni_sub *sub, uint32_t pos, uint8_t *data, int *size)
{
	int capacity = *size;
	*size = 0;
	if (!sub->vobsub_data) {
		sub->vobsub_data = amalloc(VOBSUB_DATA);
		if (!sub->vobsub_data) return 1;
	}
	// Read from the indexed packet; never parse beyond a short or failed read.
	if (file_seek(sub->vobsub_fd, pos, SEEK_SET) < 0) return 1;
	int available = file_read(sub->vobsub_fd, sub->vobsub_data, VOBSUB_DATA);
	if (available <= 0) return 1;
	*size = capacity;
	return parse_mpeg(sub->vobsub_data, available, data, size);
}

static int close_IDX( uni_sub *sub )
{
	int fd = sub->vobsub_fd;
DBG serprintf("close_IDX: fd %d\n", fd );
	if( sub->vobsub_fd >= 0 ) {
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

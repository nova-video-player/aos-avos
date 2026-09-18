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
#include "global.h"
#include "debug.h"
#include "astdlib.h"
#include "stream.h"
#include "util.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <limits.h>

#ifdef CONFIG_STREAM
#ifdef CONFIG_VOBSUB

#define DBGS	if(Debug[DBG_STREAM])
#define DBG 	if(Debug[DBG_SUB])
#define DBG2 	if(Debug[DBG_SUB] > 1)

typedef struct SSA_PRIV {
	int start;
	int end;
	int text;
} SSA_PRIV;

#define SKIP_SPACE(x) while(*x && isspace(*x)){x++;}

static char *get_line( char *c, char **line )
{
	*line = c;
	while( *c ) {
		if( *c == '\r' && *(c + 1) == '\n' ) {
			// end of line, terminate
			*c = '\0';
			c += 2;
			return c;
		} else if( *c == '\n' || *c == '\r' ) {
			// end of line, terminate
			*c = '\0';
			c++;
			return c;
		}
		c++;
	}
	
	if( c == *line )
		return NULL;
		
	return c;
}

static char *get_word( char *c, char **word )
{
	SKIP_SPACE( c );
	*word = c;
	while( *c ) {
		if( *c == '\r' && *(c + 1) == '\n' ) {
			// end of line, terminate
			*c = '\0';
			c += 2;
			return c;
		} else if( *c == ',' || *c == '\n' ) {
			// end of line, terminate
			*c = '\0';
			c++;
			return c;
		}
		c++;
	}
	if( c == *word )
		return NULL;
	return c;
}

static int SSA_parse_format( char *format, SSA_PRIV *priv )
{	
	char *c = format;
	int count = 0;
	
	// skip the format tag:
	c = strstr( c, ":" );
	if( !c )
		return 1;
	c++;
	SKIP_SPACE( c );
	
	char *word;
	
	while( (c = get_word( c, &word )) ) {
DBG2 serprintf("word: %d [%s]\r\n", count, word );		
		if( !strcmpNC( word, "Start" ) ) {
			priv->start = count;
		} else if( !strcmpNC( word, "End" ) ) {
			priv->end = count;
		} else if( !strcmpNC( word, "Text" ) ) {
			priv->text = count;
		}
		count++;
	}
	return 0;
}

static int SSA_parse_header( char *data, SSA_PRIV *priv )
{
	// check header
	if(!strstrNC(data,"[Script Info]")){
DBG serprintf( "SSA: not SSA!\n" );
		return 1;
	}
DBG serprintf( "SSA: found header!\n" );

	// look for events
	char *c    = data;
	char *line;
	while( (c = get_line( c, &line )) ) {
//printf("%s\r\n", line );
		if( strstrNC( line, "[Events]" ) ) {
			// next line is the format:
			char *format;
			c = get_line( c, &format );
			if( c ) {
				format = strstrNC( format, "Format:" );
				if( format ) {
DBG serprintf("FORMAT: %s\r\n", format );
					SSA_parse_format( format, priv );
				}
			} 
			break;
		}
	}
	if( !priv->start && !priv->end && !priv->text ) {
DBG serprintf("force defaults!\r\n" );
		// assume defaults:
		priv->start = 1;
		priv->end   = 2;
		priv->text  = 9;
	}
DBG serprintf("start %d  end %d  text %d\r\n", priv->start, priv->end, priv->text );
	return 0;
}

#define SS_TO_MS(x) ((x)*1000)
#define MM_TO_MS(x) ((SS_TO_MS(x))*60)
#define HH_TO_MS(x) ((MM_TO_MS(x))*60)

static int get_time(char *line)
{
	unsigned hh, mm, ss, cs;
	if (sscanf(line, "%u:%u:%u.%u", &hh, &mm, &ss, &cs) != 4 ||
	    mm >= 60 || ss >= 60 || cs >= 100) return -1;
	int64_t time = ((int64_t)hh * 3600 + mm * 60 + ss) * 1000 + cs * 10;
	return time <= INT_MAX ? (int)time : -1;
}

static void SSA_clean_text( char *text )
{
	char *in  = text;
	char *out = text;
	
	while( *in ) {
		// remove all style override codes
		if( *in == '{' && *(in + 1) == '\\' ) {
			in += 2;
			while( *in && *in != '}' )
				in++;
			in++;
			if( !*in )
				break;
		}
		// replace \N by \n (so only \n exported)
		if( *in == '\\' && *(in + 1) == 'N' ) {
			*(in + 1) = 'n';
		}
		*out++ = *in++;
	}
	*out++ = *in++;
}

static int SSA_parse_dialogue( char *line, int size, SSA_PRIV *priv, VIDEO_FRAME *frame )
{	
	char *c = line;
	int count = 0;
	
	// skip the dialogue tag:
	c = strstr( c, ":" );
	if( !c )
		return 1;
	c++;
	SKIP_SPACE( c );
	
	char *word;
	
	while( count < priv->text && (c = get_word( c, &word )) ) {
DBG2 serprintf("word: %d [%s]\r\n", count, word );		
		if( count == priv->start ) {
			frame->time = get_time( word );
			if (frame->time < 0) return 1;
DBGS serprintf("start: %s  %d  ", word, frame->time );
		} else if( count == priv->end ) {
			int end = get_time(word);
			if (frame->time < 0 || end <= frame->time) return 1;
			frame->duration = end - frame->time;
DBGS serprintf("end:   %s  %d  ", word, frame->duration  );
		} 
		count++;
	}
	if( count == priv->text ) {
		word = c;
DBGS serprintf("text:  %s\r\n", word );
		strnZcpy( frame->data[0], word, frame->size - 1 );
		//SSA_clean_text( frame->data[0] );
DBGS serprintf("clean: %s\r\n", frame->data[0] );
		return 0;
	}
	return 1;
}


static int _open( STREAM_DEC_SUB *dec, SUB_PROPERTIES *sub, void *ctx )
{
DBGS serprintf("sub_dec_open_SSA\r\n");
	SSA_PRIV *priv = dec->priv;
	*priv = (SSA_PRIV){.start = 1, .end = 2, .text = 9};
	if (sub->extraData2 && sub->extraDataSize2 > 0) {
		char *header = amalloc((size_t)sub->extraDataSize2 + 1);
		if (!header) return 1;
		memcpy(header, sub->extraData2, sub->extraDataSize2);
		header[sub->extraDataSize2] = 0;
		int failed = SSA_parse_header(header, priv);
		afree(header);
		if (failed) return 1;
	}  
	
	// copy the props locally
	dec->subtitle = &dec->_subtitle;
	*dec->subtitle = *sub;
	
	dec->ctx = ctx;
	dec->is_open = 1;
	
	return 0;
}

static int _close( STREAM_DEC_SUB *dec )
{
DBGS serprintf( "sub_dec_close_SSA\r\n");
	if( !dec->is_open ) {
serprintf("SSA: not open!\r\n");
		return 1;
	}
	
	dec->is_open = 0;
 	return 0;
}

static int _decode( STREAM_DEC_SUB *dec, UCHAR *data, int size, int time, VIDEO_FRAME **pframe )
{
DBG serprintf("SSA_decode: size %5d  time %d  [%s]\r\n", size, time, data );	
DBG2 Dump( data, size );
	VIDEO_FRAME *frame = *pframe;
	*pframe = NULL;
	if (!data || size <= 0 || !frame || !frame->data[0] || frame->size < 1) return 1;
	char *line = amalloc((size_t)size + 1);
	if (!line) return 1;
	memcpy(line, data, size);
	line[size] = 0;
	frame->time = -1;
	frame->duration = -1;
	int ret = SSA_parse_dialogue(line, size, dec->priv, frame);
	afree(line);
	if (ret || frame->time < 0 || frame->duration <= 0) return 1;
	*pframe = frame;
	return 0;
}

static int _flush( STREAM_DEC_SUB *dec )
{	
	return 0;
} 

static int _destroy( STREAM_DEC_SUB *dec )
{
	if( dec	) {
		if( dec->priv )
			afree( dec->priv );
		afree( dec );
	}
	return 0;
} 

static STREAM_DEC_SUB *_new_dec_ssa( void ) 
{ 
	STREAM_DEC_SUB *dec = (STREAM_DEC_SUB *)amalloc( sizeof( STREAM_DEC_SUB ) );
	
	if( !dec )
		return NULL;
	memset( dec, 0, sizeof( STREAM_DEC_SUB ) );

	static char name[] = "SSA";
	dec->name    = name;
	dec->destroy = _destroy;
	dec->open    = _open;
	dec->close   = _close;
	dec->decode  = _decode;
	dec->flush   = _flush;

	// allocate private data
	if( !(dec->priv = (SSA_PRIV*)amalloc( sizeof( SSA_PRIV ) ) ) ) {
		afree( dec );
		return NULL;
	}
	memset( dec->priv, 0, sizeof( SSA_PRIV ) );
	
	return dec;
}

STREAM_REGISTER_DEC_SUB( SUB_FORMAT_SSA, _new_dec_ssa, "SSA" );

#endif
#endif

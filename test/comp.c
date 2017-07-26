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

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>

#include "wav.h"
#include "stream_filter_audio.h"

#define serprintf printf
#define STANDALONE
#define SIM

#define DBG if(1)
#define DBG2 if(0)

#define amalloc( a )            malloc( (a) )
#define acalloc( a, b )         calloc( (a), (b) )
#define arealloc( a, b )        realloc( (a), (b) )
#define afree( a )              free( (a) )

#include "../Source/stream_filter_audio_dummy.c"
#include "../Source/pcm_autogain.c"
#include "../Source/stream_filter_audio_agc.c"

int main ( int argc, char * argv[] )
{
	if( argc < 3 ) {
		printf("comp <infile> <outfile>\n");
		return 1;
	}
	int in = open( argv[1], O_RDONLY );
	
	if( in == -1 ) {
		printf("cannot open %s: %s\n", argv[1], strerror(errno) );
		return -1;
	}

	int out = open( argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0600 );
	
	if( out == -1 ) {
		printf("cannot open %s: %s\n", argv[2], strerror(errno) );
		return -1;
	}
	
	WAV_HEADER w;
	unsigned char d[8];
	read( in, &w, sizeof( w ) );
	read( in, &d, 8 );

printf("read %d: freq: %d  at %d\n", w.samplesPerSec );

	write( out, &w, sizeof( w ) );
	write( out, &d, 8 );
	
	STREAM_FILTER_AUDIO *af = stream_filter_audio_agc_new();
	AUDIO_PROPERTIES audio = { 0 };
	
	audio.samplesPerSec = w.samplesPerSec;
	
printf("filter: %s\n", af->name );	
	
	af->open( af, &audio );	
	
	#define SAMPLES 256
	
	unsigned char buf[SAMPLES * 2 * 2];
	int buf_size = sizeof( buf );
	int bytes;
	do {
		bytes = read( in, buf, buf_size );
	
		AUDIO_FRAME f = { 0 };
		f.data = buf;
		f.size = bytes;

		af->filter( af, &f );	
		
		write( out, f.data, f.size );
	} while( bytes == buf_size );
	
	close( in );
	close( out );

	af->delete( af );	
	
	return 0;
}

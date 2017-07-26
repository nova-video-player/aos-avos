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

#ifndef _MP3_H
#define _MP3_H

#include "types.h"
#include "av.h"

#define NB_SYNC_TRIES		50

// This is the information structure of the MPEG-frame that
// is processed at the moment

typedef struct MP3_FRAME 
{
	int MPEG;
	int lsf;
	int layer;

   	int protection;
    	int bitrate_index;
    	int sr_index;
    	int padding;
	int private;
    	int mode;
	int ch;
    	int mode_ext;
    	int copyright;
    	int original;
    	int emphasis;
	int CRC;
	
	int data_start;

	int length;		// length of the source frame
	
	int side_info_end;
	int real_length;	// real length of the source frame
	int new_length;
	int new_rate;
	int main_data_begin;
	int part2_3_bytes;
	
	int vbr_rate;
	int vbr_seconds;	

	int bits;		// bits per sample for PCM
	
	int header_size;	// size of file header (non-music data)
} MP3_FRAME;

enum {
	MPEG_1 = 0,
	MPEG_2,
	MPEG_2_5,
};

enum {
	LAYER_1 = 0,
	LAYER_2,
	LAYER_3,
};

#define MPEG_AUDIO_MIN_LAYER LAYER_1

int  MP3_check_header( UCHAR head_0, UCHAR head_1, int *_MPEG, int *_lsf, int *_layer );
void MP3_get_frame_stats( MP3_FRAME *fr, UCHAR *data );
int  MP3_find_sync( AUDIO_PROPERTIES *audio, UCHAR *p, int size, MP3_FRAME *fr, int *ofs );
int  MP3_find_sync2( UCHAR *p, int size, int *ofs );

// MPEG frequencies, according to the frequency index

extern long mp3_freqs[3][3];

// MPEG bitrates, according to the bitrate index

extern long mp3_rates[3][3][15];

// VBR header flags
#define FRAMES_FLAG     0x0001
#define BYTES_FLAG      0x0002
#define TOC_FLAG        0x0004
#define VBR_SCALE_FLAG  0x0008

#endif

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

#ifndef STANDALONE

#include "global.h"
#include "mp3.h"
#include "debug.h"		

#define DBGP	if(Debug[DBG_PARSER])
#define DBGP2   if(Debug[DBG_PARSER] > 1)

#endif

// *******************************************************************************
// *******************************************************************************
//
// 	Generic MP3 stuff
//	
// *******************************************************************************
// *******************************************************************************

// MPEG sample frequencies 
long mp3_freqs[3][3] = {
	{ 44100, 48000, 32000, },	// MPEG 1 
	{ 22050, 24000, 16000, },	// MPEG 2
	{ 11025, 12000, 8000   },	// MPEG 2.5
};

// MPEG bitrates		
long mp3_rates[3][3][15] = 
{
      {	{0,32,48,56,64,80,96,112,128,160,192,224,256,320,384},	// MPEG 1   layer 1
	{0, 8,16,24,32,40,48,56,  64, 80, 96,112,128,144,160},	// MPEG 2   layer 1
	{0, 8,16,24,32,40,48,56,  64, 80, 96,112,128,144,160} },// MPEG 2.5 layer 1
   
      {	{0,32,48,56,64,80,96,112,128,160,192,224,256,320,384},	// MPEG 1   layer 2
	{0, 8,16,24,32,40,48,56,  64, 80, 96,112,128,144,160},	// MPEG 2   layer 2
	{0, 8,16,24,32,40,48,56,  64, 80, 96,112,128,144,160} },// MPEG 2.5 layer 2

      { {0,32,40,48,56,64,80, 96,112,128,160,192,224,256,320},	// MPEG 1   layer 3
	{0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160},	// MPEG	2   layer 3
	{0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160} } // MPEG 2.5 layer 3
};

// ************************************************************
//
//	MP3_check_header
//
// ************************************************************
int MP3_check_header( UCHAR head_0, UCHAR head_1, int *_MPEG, int *_lsf, int *_layer )
{
	int MPEG = -1, lsf = -1, layer = -1;
	int head = ((head_0 << 8) | head_1) & 0xFFFE;
	
	if  ( head == 0xFFFA ) {	// MPEG 1
		MPEG  = MPEG_1;
		lsf   = 0;
		layer = LAYER_3;
	} else if ( head == 0xFFF2 ) {	// MPEG 2
		MPEG  = MPEG_2;
		lsf   = 1;
		layer = LAYER_3;
	} else if ( head == 0xFFE2 ) {	// MPEG 2.5 ( low bitrate extensions )
		MPEG  = MPEG_2_5;
		lsf   = 1;
		layer = LAYER_3;
	} else if ( head == 0xFFFC ) {	// MPEG 1, layer 2
		MPEG  = MPEG_1;
		lsf   = 0;
		layer = LAYER_2;
	} else if ( head == 0xFFF4 ) {	// MPEG 2, layer 2
		MPEG  = MPEG_2;
		lsf   = 0;
		layer = LAYER_2;
	} else if ( head == 0xFFFE ) {	// MPEG 1, layer 1
		MPEG  = MPEG_1;
		lsf   = 0;
		layer = LAYER_1;
	} else if ( head == 0xFFF6 ) {	// MPEG 2, layer 2
		MPEG  = MPEG_2;
		lsf   = 0;
		layer = LAYER_1;
	}
DBGP2 serprintf("MP3_check_header: HEAD %04X -> MPEG_%d  LAYER_%d  lsf %d\r\n", head, MPEG + 1, layer + 1, lsf );

	if (_layer )
		*_layer = layer;
	if (_lsf )
		*_lsf = lsf;
	if( _MPEG )
		*_MPEG = MPEG;

	if( layer != -1 )
		return 0;
		
	return 1;
}

// ************************************************************
//
//	MP3_get_frame_stats
//
//	fill the MP3_FRAME struct "fr" from the 4 bytes MPEG header "data"
//
// ************************************************************
void MP3_get_frame_stats( MP3_FRAME *fr, UCHAR *data )
{ 
 
	fr->protection 	= data[1] & 0x01; 	
	
	fr->bitrate_index = (data[2] >> 4) & 0x0f;
  	fr->sr_index	= (data[2] >> 2) & 0x03;

    	fr->padding	= (data[2] >> 1) & 0x01;
	fr->private	= (data[2] & 0x01 );
   	
	fr->mode	= (data[3] >> 6) & 0x03;
  	fr->ch		= (fr->mode == 3) ? 1 : 2;
    	fr->mode_ext	= (data[3] >> 4) & 0x03;
    	fr->copyright	= (data[3] >> 3) & 0x01;
    	fr->original	= (data[3] >> 2) & 0x01;
    	fr->emphasis	= (data[3] & 0x03 );
	// determine offset of header
	if( fr->MPEG == 0 ) {        // mpeg1
    		if( fr->mode != 3 ) 
			fr->data_start = 32 + 4;
    		else    
			fr->data_start = 17 + 4;
	} else {      // mpeg2
    		if( fr->mode != 3 )
			fr->data_start = 17 + 4;
    		else
			fr->data_start = 9 + 4;
	}

	if( 	fr->layer == -1  	|| fr->MPEG > MPEG_2_5    || 
		fr->bitrate_index == 0 	|| fr->bitrate_index > 14 || 
		fr->sr_index > 2 	|| fr->lsf > 1 
	) {
		fr->length = 0;
	} else {
		fr->length = (144000 * mp3_rates[fr->layer][fr->MPEG][fr->bitrate_index] / 
		    	     ( mp3_freqs[fr->MPEG][fr->sr_index] << fr->lsf ));
	}
DBGP serprintf("MP3: %02X %02X %02X  MPEG %d  lsf %d  layer %d  br %2d  sr %d  len %4d  data %d\r\n", data[0], data[1], data[2], 
	fr->MPEG, fr->lsf, fr->layer, fr->bitrate_index, fr->sr_index, fr->length , fr->data_start );

}

static int verify_header( UCHAR *p, int ofs, MP3_FRAME *fr )
{
	int MPEG, lsf, layer;
	if( MP3_check_header( p[fr->length + ofs], p[fr->length + ofs + 1], &MPEG, &lsf, &layer ) )
		return 1;
	if( MPEG != fr->MPEG || layer != fr->layer || lsf != fr->lsf )
		return 1;
	return 0;
}

// ******************************************************************
//
//	MP3_find_sync
//
// ******************************************************************
int MP3_find_sync( AUDIO_PROPERTIES *audio, UCHAR *p, int size, MP3_FRAME *fr, int *ofs ) 
{
	int sc = 0;
	while (sc < size) {
		// Look for Sync Word
		MP3_check_header( p[0], p[1], &fr->MPEG, &fr->lsf, &fr->layer );
			
		if ( fr->layer >= MPEG_AUDIO_MIN_LAYER ) {
			// *************************************
			// Process Header
			// ************************************
			unsigned char head[4];
			int i;
			for (i = 0; i < 4; i++)
				head[i] = p[i];
 
 			MP3_get_frame_stats( fr, head );

			if( fr->sr_index > 2 )
				goto INVALID;
				
			if( !fr->length )
				goto INVALID;
			
			if( sc + fr->length > size - 1 ) {
DBGP2 serprintf("sc %d  size %d  length %d\n", sc, size, fr->length );
				goto INVALID;
			}	
			// verify, that we really found a valid header by checking the next sync word
			if ( !verify_header( p, 0, fr ) || !verify_header( p, 1, fr ) ) {
				// header information is the same as before, we found a valid header
				if( audio ) {
					audio->bytesPerSec   = mp3_rates[fr->layer][fr->MPEG][fr->bitrate_index] * 125;
					audio->samplesPerSec = mp3_freqs[fr->MPEG][fr->sr_index];
					audio->blockAlign    = 1;
					audio->channels      = (fr->mode == 3) ? 1 : 2;
					audio->bitsPerSample = 16;
					audio->format        = WAVE_FORMAT_MPEGLAYER3;
					audio->vbr	     = 0;
					audio->frames        = 0;
					audio->blockAlign    = 1;
					audio->scale 	     = audio->blockAlign;
					audio->rate          = audio->bytesPerSec;
					audio->valid         = 1;
DBGP show_audio_props( audio );
				}
				// header found, break
				if( ofs )
					*ofs = sc;
				return 0;
			}
		}
INVALID:
DBGP serprintf("not a valid header!\n");
		p++;
		sc++;
	}
	
	if( ofs )
		*ofs = sc;
	return 1;
}

int MP3_find_sync2( UCHAR *p, int size, int *ofs ) 
{
	MP3_FRAME fr;
	return MP3_find_sync( NULL, p, size, &fr, ofs );
}


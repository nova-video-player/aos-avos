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

#ifndef _WAV_H
#define _WAV_H

#include "types.h"
#include "av.h"
#include "id3tag.h"

#define FCC_RIFF 	0x46464952	/* "RIFF" */
#define FCC_WAVE	0x45564157	/* "WAVE" */
#define FCC_FMT		0x20746D66	/* "fmt " */
#define FCC_DATA	0x61746164	/* "data" */
#define FCC_FACT	0x74636166	/* "fact" */
#define FCC_ARCS	0x53435241	/* "ARCS" */
#define FCC_RCFM	0x4D464352	/* "RCFM" */
#define FCC_AREC	0x43455241	/* "AREC" */
#define FCC_AEDI	0x49444541	/* "AEDI" */
#define FCC_PAD		0x20444150	/* "PAD " from Steinberg Wavelab */
#define FCC_BEXT	0x74786562	/* "BEXT" broadcast extension chunk */

typedef struct _WAVEFORMATEX
{
	UINT16	wFormatTag; // using uint - less problems
	INT16	nChannels;
	
	INT32	nSamplesPerSec;
	INT32	nAvgBytesPerSec;
	
	INT16	nBlockAlign;
	INT16	wBitsPerSample;
	
	INT16	cbSize;
	INT16 	wID;
	
	INT32 	fdwFlags;
	
	INT16 	nBlockSize;
	INT16 	nFramesPerBlock;
	
	INT16 	nCodecDelay;
	INT16 	ndummy;

} WAVEFORMATEX;

typedef struct WAV_HEADER 
{
	UINT32		riff_ID;
	UINT32		fileSize;
	UINT32		riff_typeID;

	UINT32		fmt_ID;
	UINT32		fmt_size;

	// WAVEFORMATEX
	UINT16		format;
	UINT16		channels;
	UINT32		samplesPerSec;
	UINT32		avgBytesPerSec;
	UINT16		blockAlign;
	UINT16		bitsPerSample;
} WAV_HEADER;

typedef struct RIFF_HEADER 
{
	UINT32		fourcc;
	UINT32		size;
	UINT32		typeID;
} RIFF_HEADER;

typedef struct RIFF_CHUNK
{
	UINT32		fourcc;
	UINT32		size;
} RIFF_CHUNK;

typedef struct __PACKED__ WAV_FORMAT
{
	UINT16		format;
	UINT16		channels;
	UINT32		samplesPerSec;
	UINT32		avgBytesPerSec;
	UINT16		blockAlign;
	UINT16		bitsPerSample;
	UINT16		cbSize;
} WAV_FORMAT;

typedef struct __PACKED__ WAV_FORMAT_EXTENSIBLE
{
	UINT16 bitsPerSample;
	UINT32 channelMask;		
	UINT32 guid_format;
	UCHAR  guid_rest[12];
} WAV_FORMAT_EXTENSIBLE;

typedef struct WAV_FACT_HEADER
{
	UINT32		fact_ID;
	UINT32		fact_size;
	UINT32		sampleLength;
} WAV_FACT_HEADER;

typedef struct __PACKED__ WAV_IMA_ADPCM_HEADER {
	UINT16	samplesPerBlock;
} WAV_IMA_ADPCM_HEADER;

typedef struct WAV_DATA_HEADER 
{
	UINT32		data_ID;
	UINT32		data_size;
	UINT32		header_size;
} WAV_DATA_HEADER;

typedef struct {
	AUDIO_PROPERTIES audio;
	
	ID3_TAG		id3tag;	

	UINT32	data_start;
	UINT32	data_size;
	int	clippable;
} WAV_HeaderInfo;

int WAV_ParseHeader( UCHAR *buffer, UINT64 size, WAV_HeaderInfo *h, APIC *apic );

#endif /* _WAH_H */


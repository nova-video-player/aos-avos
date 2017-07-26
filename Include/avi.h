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

// *****************************************************
//
// 	AVI.H
//
// *****************************************************

#ifndef _AVI_H
#define _AVI_H

#include "stream.h"
#include "types.h"

#define mmioFOURCC( ch0, ch1, ch2, ch3 ) \
  ( (long)(unsigned char)(ch0) | ( (long)(unsigned char)(ch1) << 8 ) | \
  ( (long)(unsigned char)(ch2) << 16 ) | ( (long)(unsigned char)(ch3) << 24 ) )

#define aviTWOCC(ch0, ch1) ((USHORT)(UCHAR)(ch0) | ((USHORT)(UCHAR)(ch1) << 8)) 

// form types, list types, and chunk types 
#define formtypeAVI             mmioFOURCC('A', 'V', 'I', ' ')
#define formtypeAMV             mmioFOURCC('A', 'M', 'V', ' ')
#define formtypeAVIX            mmioFOURCC('A', 'V', 'I', 'X')

#define listtypeAVIHEADER       mmioFOURCC('h', 'd', 'r', 'l')
#define listtypeSTREAMHEADER    mmioFOURCC('s', 't', 'r', 'l')
#define listtypeAVIMOVIE	mmioFOURCC('m', 'o', 'v', 'i')
#define listtypeINFO		mmioFOURCC('I', 'N', 'F', 'O')
#define listtypeODML		mmioFOURCC('o', 'd', 'm', 'l')
#define ckidODMLHDR             mmioFOURCC('d', 'm', 'l', 'h')

#define ckidAVIMAINHDR          mmioFOURCC('a', 'v', 'i', 'h')
#define ckidAMVMAINHDR          mmioFOURCC('a', 'm', 'v', 'h')
#define ckidSTREAMHEADER        mmioFOURCC('s', 't', 'r', 'h')
#define ckidSTREAMFORMAT        mmioFOURCC('s', 't', 'r', 'f')
#define ckidSTREAMHANDLERDATA   mmioFOURCC('s', 't', 'r', 'd')
#define ckidSTREAMNAME		mmioFOURCC('s', 't', 'r', 'n')
#define ckidAVINEWINDEX		mmioFOURCC('i', 'd', 'x', '1')
#define ckidAVIODMLINDEX	mmioFOURCC('i', 'n', 'd', 'x')

#define FOURCC_RIFF		mmioFOURCC('R', 'I', 'F', 'F')
#define FOURCC_LIST		mmioFOURCC('L', 'I', 'S', 'T')

// Chunk id to use for video copy protection. 
#define ckidMACROPKEY		mmioFOURCC('P', 'K', 'E', 'Y')

// Chunk id to use for extra chunks for padding.
#define ckidAVIPADDING		mmioFOURCC('J', 'U', 'N', 'K')

/*
 ** Stream types for the <fccType> field of the stream header.
 */
#define streamtypeVIDEO		mmioFOURCC('v', 'i', 'd', 's')
#define streamtypeAUDIO		mmioFOURCC('a', 'u', 'd', 's')

/* Basic chunk types */
#define cktypeDIBbits		aviTWOCC( 'd', 'b' )
#define cktypeDIBcompressed	aviTWOCC( 'd', 'c' )
#define cktypeDIBKcompressed	aviTWOCC( 'k', 's' )
#define cktypeDIBKUcompressed	aviTWOCC( 'k', 'c' )
#define cktypePALchange		aviTWOCC( 'p', 'c' )
#define cktypeWAVEbytes		aviTWOCC( 'w', 'b' )

#define idxDIBCompressed	mmioFOURCC('0', '0', 'd', 'c')
// Added for Seal encrypted data
#define idxDIBKCompressed	mmioFOURCC('A', 'k', 'k', 's')
#define idxDIBKUCompressed	mmioFOURCC('A', 'k', 'k', 'c')
#define idxWAVEbytes		mmioFOURCC('0', '1', 'w', 'b')

#define txtDIBCompressed	"00dc"
#define txtDIBKCompressed	"Akks"
#define txtDIBKUCompressed	"Akkc"
#define txtWAVEbytes		"01wb"
#define txtJUNK			"JUNK"

#define MPEGLAYER3_ID_UNKNOWN            0
#define MPEGLAYER3_ID_MPEG               1
#define MPEGLAYER3_ID_CONSTANTFRAMESIZE  2

#define MPEGLAYER3_FLAG_PADDING_ISO      0x00000000
#define MPEGLAYER3_FLAG_PADDING_ON       0x00000001
#define MPEGLAYER3_FLAG_PADDING_OFF      0x00000002

typedef struct __PACKED__ {
	INT32	biSize;
	INT32	biWidth;
	INT32	biHeight;
	INT16	biPlanes;
	INT16	biBitCount;
	INT32	biCompression;
	INT32	biSizeImage;
	INT32	biXPelsPerMeter;
	INT32	biYPelsPerMeter;
	INT32	biClrUsed;
	INT32	biClrImportant;
	UCHAR	CodecSpecificData[1];
} BITMAPINFOHEADER;

typedef struct __PACKED__ {
	BITMAPINFOHEADER bmiHeader;
	INT32 bmiColors[1];
} BITMAPINFO;

/* flags for use in <dwFlags> in AVIFileHdr */
#define AVIF_HASINDEX		0x00000010	// Index at end of file?
#define AVIF_MUSTUSEINDEX	0x00000020
#define AVIF_ISINTERLEAVED	0x00000100
#define AVIF_TRUSTCKTYPE	0x00000800	// Use CKType to find key frames?
#define AVIF_WASCAPTUREFILE	0x00010000
#define AVIF_COPYRIGHTED	0x00020000

/* The AVI File Header LIST chunk should be padded to this size */
#define AVI_HEADERSIZE  	2048		// size of AVI header list

typedef struct __PACKED__ {
	INT32	dwMicroSecPerFrame;	// frame display rate (or 0L)
	INT32	dwMaxBytesPerSec;	// max. transfer rate
	INT32	dwPaddingGranularity;	// pad to multiples of this
					// size; normally 2K.
	INT32	dwFlags;		// the ever-present flags
	INT32	dwTotalFrames;		// # frames in file
	INT32	dwInitialFrames;
	INT32	dwStreams;
	INT32	dwSuggestedBufferSize;

	INT32	dwWidth;
	INT32	dwHeight;

	INT32	dwReserved[4];
} AVI_MainHeader;

// Stream header
typedef struct __PACKED__ {
	UINT32	fccType;
	UINT32	fccHandler;
	INT32	dwFlags;	/* Contains AVITF_* flags */
	INT16	wPriority;	/* dwPriority - splited for audio */
	INT16	wLanguage;
	INT32	dwInitialFrames;
	UINT32	dwScale;
	UINT32	dwRate;		/* dwRate / dwScale == samples/second */
	INT32	dwStart;
	INT32	dwLength;	/* In units above... */
	INT32	dwSuggestedBufferSize;
	INT32	dwQuality;
	INT32	dwSampleSize;

	INT16	left;
	INT16	top;
	INT16	right;
	INT16	bottom;
} AVI_StreamHeader;

/* Flags for index */
#define AVIIF_LIST	0x00000001L // chunk is a 'LIST'
#define AVIIF_KEYFRAME	0x00000010L // this frame is a key frame.

#define AVIIF_NOTIME	0x00000100L // this frame doesn't take any time
#define AVIIF_COMPUSE	0x0FFF0000L // these bits are for compressor use

typedef struct __PACKED__ {
	USHORT	ckid_l;
	USHORT	ckid_h;
	USHORT	flags_l;
	USHORT	flags_h;
	USHORT	offset_l;		// Position of chunk
	USHORT	offset_h;		// Position of chunk
	USHORT	length_l;		// Length of chunk
	USHORT	length_h;		// Length of chunk
} AVI_INDEX_ENTRY;

typedef struct __PACKED__ {
	UINT	ckid;
	UINT	flags;
	UINT	offset;		
	UINT	length;		
} AVI_INDEX_ENTRY2;

// Basic informations from the AVI parser, retrieved from the AVI header
typedef struct {
	int		valid;

	AUDIO_PROPERTIES *audio;
	VIDEO_PROPERTIES *video;
	SUB_PROPERTIES 	*subtitle;
	
	AV_PROPERTIES	av;

	UINT32		flags;	
	UINT32 		bitrate;			// Bitrate, Bytes per second

	int		total_frames;
	
	int		movie_start;
	int		movie_size;
	int		index_size;
	
	int		stream;
	UCHAR		*start;
} AVI_HeaderInfo;

typedef struct AVI_INDEX {
	UINT64 	pos;
	int 	frame;
	int 	audio[1];	// WARNING, the index entry can be larger if there is more than
				// one audio track, but our AVI editor assumes this index entry
				// as it works with our recordings that have one audio track only!!
} AVI_INDEX;

#define AVI_RIFF_MAX	16

typedef struct {
	UINT64 offset;          
	UINT32 size;            
	UINT32 duration;
} AVI_SUPER_INDEX;

#endif


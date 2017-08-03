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
#include "debug.h"
#include "av.h"
#include "util.h"
#endif
#include <ctype.h>

// *****************************************************************************
//
//	TYPES
//
// *****************************************************************************
typedef struct {
	int	type;
	char	*name;
} TYPE_NAME;

static const TYPE_NAME type_names[] = {
	{ TYPE_NONE,	"TYPE_NONE"	},
	{ TYPE_AUD,	"TYPE_AUD"	},
	{ TYPE_VID,	"TYPE_VID"	},
	{ TYPE_PIC,	"TYPE_PIC"	},
	{ TYPE_DIR,	"TYPE_DIR"	},
	{ TYPE_PDF,	"TYPE_PDF"	},
	{ TYPE_LST,	"TYPE_LST"	},
	{ TYPE_TXT,	"TYPE_TXT"	},
	{ TYPE_UNKNOWN,	"TYPE_UNKN"	},
	{ TYPE_MPN,	"TYPE_MPN"	},
	{ TYPE_PDF,	"TYPE_PDF"	},
	{ TYPE_PLS,	"TYPE_PLS"	},
	{ TYPE_HTML,	"TYPE_HTML"	},
	{ TYPE_APPL,	"TYPE_APPL"	},
	{ TYPE_SUBT,	"TYPE_SUBT"	},
	{ TYPE_LYR,	"TYPE_LYR"	},
	{ TYPE_SWF,	"TYPE_SWF"	},
	{ TYPE_DOCUMENT,"TYPE_DOCUMENT"	},
	{ TYPE_EMX,	"TYPE_EMX"	},
	{ TYPE_APK,	"TYPE_APK"	},
};

// *****************************************************************************
//
//	ETYPES
//
// *****************************************************************************
typedef struct {
	int	etype;
	char	*name;
} ETYPE_NAME;

static const ETYPE_NAME etype_names[] = {
	{ ETYPE_NONE,		"ETYPE_NONE"		},

	{ ETYPE_MP3,		"ETYPE_MP3"		},
	{ ETYPE_WMA,		"ETYPE_WMA"		},
	{ ETYPE_PCM,		"ETYPE_PCM"		},
	{ ETYPE_AAC,		"ETYPE_AAC"		},
	{ ETYPE_M3U,		"ETYPE_M3U"		},
	{ ETYPE_PLA,		"ETYPE_PLA"		},
	{ ETYPE_JPG,		"ETYPE_JPG"		},
	{ ETYPE_BMP,		"ETYPE_BMP"		},
	{ ETYPE_PNG,		"ETYPE_PNG"		},
	{ ETYPE_AVI,		"ETYPE_AVI"		},
	{ ETYPE_ASF,		"ETYPE_ASF"		},
	{ ETYPE_MPG,		"ETYPE_MPG"		},
	{ ETYPE_DPD,		"ETYPE_DPD"		},
	{ ETYPE_AVU,		"ETYPE_AVU"		},
	{ ETYPE_MP4,		"ETYPE_MP4"		},
	{ ETYPE_MPEG_PS,	"ETYPE_MPEG_PS"		},
	{ ETYPE_RCV,		"ETYPE_RCV"		},
	{ ETYPE_MPEG_TS,	"ETYPE_MPEG_TS"		},
	{ ETYPE_MPEG_RAW,	"ETYPE_MPEG_RAW"	},
	{ ETYPE_RCA,		"ETYPE_RCA"		},
	{ ETYPE_AC3,		"ETYPE_AC3"		},
	{ ETYPE_PLS,		"ETYPE_PLS"		},
	{ ETYPE_H264_RAW,	"ETYPE_H264_RAW"	},
	{ ETYPE_YUV,		"ETYPE_YUV"		},
	{ ETYPE_AMV,		"ETYPE_AMV"		},
	{ ETYPE_MPEG_TS_LIVE,	"ETYPE_MPEG_TS_LIVE"	},
	{ ETYPE_MMS,		"ETYPE_MMS"		},
	{ ETYPE_MPG4_RAW,	"ETYPE_MPG4_RAW"	},
	{ ETYPE_FLV,		"ETYPE_FLV"		},
	{ ETYPE_RM,		"ETYPE_RM"		},
	{ ETYPE_GIF,		"ETYPE_GIF"		},
	{ ETYPE_MMSH,		"ETYPE_MMSH"		},
	{ ETYPE_IMG_RAW,	"ETYPE_IMG_RAW"		},
	{ ETYPE_RCRV,		"ETYPE_RCRV"		},
	{ ETYPE_ICO,		"ETYPE_ICO"		},
	{ ETYPE_RTSP,		"ETYPE_RTSP"		},
	{ ETYPE_SPEK,		"ETYPE_SPEK"		},
	{ ETYPE_AMR,		"ETYPE_AMR"		},
	{ ETYPE_ASX,		"ETYPE_ASX"		},
	{ ETYPE_FLAC,		"ETYPE_FLAC"		},
	{ ETYPE_OGG,		"ETYPE_OGG"		},
	{ ETYPE_SDP,		"ETYPE_SDP"		},
	{ ETYPE_DOC,		"ETYPE_DOC"		},
	{ ETYPE_XLS,		"ETYPE_XLS"		},
	{ ETYPE_PPT,		"ETYPE_PPT"		},
	{ ETYPE_XPL,		"ETYPE_XPL"		},
	{ ETYPE_XML,		"ETYPE_XML"		},
	{ ETYPE_MKV,		"ETYPE_MKV"		},
	{ ETYPE_DTS,		"ETYPE_DTS"		},
	{ ETYPE_WAVPACK,	"ETYPE_WAVPACK"		},
	{ ETYPE_TTA,		"ETYPE_TTA"		},
	{ ETYPE_MKA,		"ETYPE_MKA"		},
	{ ETYPE_MJPG,		"ETYPE_MJPG"		},
	{ ETYPE_WTV,		"ETYPE_WTV"		},
	{ ETYPE_OGV,		"ETYPE_OGV"		},

	{ ETYPE_ARTIST,		"ETYPE_ARTIST"		},
	{ ETYPE_U_ARTIST,	"ETYPE_U_ARTIST"	},
	{ ETYPE_ALBUM,		"ETYPE_ALBUM"		},
	{ ETYPE_U_ALBUM,	"ETYPE_U_ALBUM"		},
	{ ETYPE_TITLE,		"ETYPE_TITLE"		},
	{ ETYPE_GENRE,		"ETYPE_GENRE"		},
	{ ETYPE_DATE,		"ETYPE_DATE"		},
	{ ETYPE_PLAYLIST,	"ETYPE_PLAYLIST"	},
	{ ETYPE_RADIO,		"ETYPE_RADIO"		},
};
 
// *****************************************************************************
//
//	VIDEO FORMATS
//
// *****************************************************************************
typedef struct {
	UINT32	fourCC;
	int	format;
	int	subfmt;
	char	*name;
} VIDEO_FORMAT;

static const VIDEO_FORMAT video_formats[] = 
{
	VIDEO_FOURCC_MPG2,	VIDEO_FORMAT_MPEG,	VIDEO_SUBFMT_MPEG2,	"MPEG-2",	
	VIDEO_FOURCC_DVR,	VIDEO_FORMAT_MPEG,	VIDEO_SUBFMT_MPEG2,	"MPEG-2",	
	VIDEO_FOURCC_MPG1,	VIDEO_FORMAT_MPEG,	VIDEO_SUBFMT_MPEG1,	"MPEG-1",	

	VIDEO_FOURCC_H264,	VIDEO_FORMAT_H264,	VIDEO_SUBFMT_NONE,	"H.264",		
	VIDEO_FOURCC_X264,	VIDEO_FORMAT_H264,	VIDEO_SUBFMT_NONE,	"H.264",		
	VIDEO_FOURCC_DAVC,	VIDEO_FORMAT_H264,	VIDEO_SUBFMT_NONE,	"H.264",		
	VIDEO_FOURCC_AVC1,	VIDEO_FORMAT_H264,	VIDEO_SUBFMT_NONE,	"H.264",		

	VIDEO_FOURCC_H265,	VIDEO_FORMAT_HEVC,	VIDEO_SUBFMT_NONE,	"HEVC/H.265",
	VIDEO_FOURCC_X265,	VIDEO_FORMAT_HEVC,	VIDEO_SUBFMT_NONE,	"HEVC/H.265",
	VIDEO_FOURCC_HEVC,	VIDEO_FORMAT_HEVC,	VIDEO_SUBFMT_NONE,	"HEVC/H.265",

	VIDEO_FOURCC_WMV1,	VIDEO_FORMAT_WMV1,	VIDEO_SUBFMT_NONE,	"WMV7",		
	VIDEO_FOURCC_WMV2,	VIDEO_FORMAT_WMV2,	VIDEO_SUBFMT_NONE,	"WMV8",		
	VIDEO_FOURCC_WMV3,	VIDEO_FORMAT_WMV3,	VIDEO_SUBFMT_NONE,	"WMV9",
	VIDEO_FOURCC_WM3B,	VIDEO_FORMAT_WMV3B,	VIDEO_SUBFMT_NONE,	"WMV9 Beta",		// we use this only internally!
	VIDEO_FOURCC_WVC1,	VIDEO_FORMAT_VC1,	VIDEO_SUBFMT_NONE,	"VC1",		
	VIDEO_FOURCC_WMVA,	VIDEO_FORMAT_VC1,	VIDEO_SUBFMT_NONE,	"VMVA",			// obsolete pre VC1...		
	VIDEO_FOURCC_WVP2,	VIDEO_FORMAT_VC1IMAGE,	VIDEO_SUBFMT_NONE,	"VC1 Image",		
	VIDEO_FOURCC_MSS2,	VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"WMV9 Screen",		
	VIDEO_FOURCC_DIV3,	VIDEO_FORMAT_MSMP43,	VIDEO_SUBFMT_NONE,	"MSMP43",	
	VIDEO_FOURCC_MP43,	VIDEO_FORMAT_MSMP43,	VIDEO_SUBFMT_NONE,	"MSMP43",	
	VIDEO_FOURCC_MP42,	VIDEO_FORMAT_MSMP42,	VIDEO_SUBFMT_NONE,	"MSMP42",	
	VIDEO_FOURCC_MP41,	VIDEO_FORMAT_MSMP41,	VIDEO_SUBFMT_NONE,	"MSMP41",	
	VIDEO_FOURCC_MPG4,	VIDEO_FORMAT_MSMP41,	VIDEO_SUBFMT_NONE,	"MSMP41",	
	VIDEO_FOURCC_MJPG,	VIDEO_FORMAT_MJPG,	VIDEO_SUBFMT_NONE,	"MJPG",		
	
	VIDEO_FOURCC_DIVX,	VIDEO_FORMAT_MPG4,	VIDEO_SUBFMT_NONE,	"MPEG-4",		
	VIDEO_FOURCC_4GMC,	VIDEO_FORMAT_MPG4GMC,	VIDEO_SUBFMT_NONE,	"MPEG-4(GMC)",		

	VIDEO_FOURCC_ARCHOS,	VIDEO_FORMAT_ARCHOS,	VIDEO_SUBFMT_NONE,	"ARCHOS",	
	VIDEO_FOURCC_YUV,	VIDEO_FORMAT_YUV,	VIDEO_SUBFMT_NONE,	"YUV",		
	VIDEO_FOURCC_SPARK,	VIDEO_FORMAT_SPARK,	VIDEO_SUBFMT_NONE,	"Spark",		

	VIDEO_FOURCC_RV10,	VIDEO_FORMAT_RV10,	VIDEO_SUBFMT_NONE,	"RealVideo 1.0",	//RV10 is RealVideo 1.0 (as in Real 5.0)		
	VIDEO_FOURCC_RV20,	VIDEO_FORMAT_RV20,	VIDEO_SUBFMT_NONE,	"RealVideo G2",		//RV20 is RealVideo G2 (6) and G2+SVT (7)
	VIDEO_FOURCC_RVTR_RV20,	VIDEO_FORMAT_RV20,	VIDEO_SUBFMT_NONE,	"RealVideo G2",		//RVTR is RV20 (says Real) 
	VIDEO_FOURCC_RV30,	VIDEO_FORMAT_RV30,	VIDEO_SUBFMT_NONE,	"RealVideo 8",		//RV30 is RealVideo 8
	VIDEO_FOURCC_RVTR_RV30,	VIDEO_FORMAT_RV30,	VIDEO_SUBFMT_NONE,	"RealVideo 8",		//RVT2 is RV30 (says Real)
	VIDEO_FOURCC_RV40,	VIDEO_FORMAT_RV40,	VIDEO_SUBFMT_NONE,	"RealVideo 9/10",	//RV40 is RealVideo 9 and 10, RealVideo 10 is same bitstream format as 9	
	VIDEO_FOURCC_RV41,	VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"RealVideo 10 Interlaced", // unsupported
	VIDEO_FOURCC_RV4X,	VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"RealVideo Experimental", // unsupported
	
	VIDEO_FOURCC_VP6F,	VIDEO_FORMAT_VP6,	VIDEO_SUBFMT_NONE,	"VP6",		
	VIDEO_FOURCC_VP70,	VIDEO_FORMAT_VP7,	VIDEO_SUBFMT_NONE,	"VP7",		
	VIDEO_FOURCC_VP80,	VIDEO_FORMAT_VP8,	VIDEO_SUBFMT_NONE,	"VP8",		
	VIDEO_FOURCC_VP90,	VIDEO_FORMAT_VP9,	VIDEO_SUBFMT_NONE,	"VP9",		

	VIDEO_FOURCC_THEO,	VIDEO_FORMAT_THEORA,	VIDEO_SUBFMT_NONE,	"Theora",		
	
	VIDEO_FOURCC_SCREEN,	VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"Screen video",		
	VIDEO_FOURCC_DIB,	VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"DIB",		
	VIDEO_FOURCC_IV32,	VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"IV32",		
	VIDEO_FOURCC_IV50,	VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"IV50",		
	VIDEO_FOURCC_SVQ1,	VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"SVQ1",		
	VIDEO_FOURCC_SVQ3,	VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"SVQ3",		
	VIDEO_FOURCC_CVID,	VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"Cinepak",	
	VIDEO_FOURCC_H263,	VIDEO_FORMAT_H263,	VIDEO_SUBFMT_NONE,	"H263",		
	VIDEO_FOURCC_H263PLUS,	VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"H263+",		
	VIDEO_FOURCC_MSVC,	VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"MSVC",		
	VIDEO_FOURCC_CRAM,	VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"MSVC",		
	VIDEO_FOURCC_WHAM,	VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"MSVC",		
	VIDEO_FOURCC_TSCC,	VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"TSCC",			// TechSmith Screen Capture Codec		
	0,			VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"s_none",	
	1,			VIDEO_FORMAT_UNKNOWN,	VIDEO_SUBFMT_NONE,	"s_none",
		
	VIDEO_FOURCC_LAVC,	VIDEO_FORMAT_LAVC,	VIDEO_SUBFMT_NONE,	"lavc",			// catch-all codec for stuff that lavc supports!
};

static const uint32_t mpeg4_formats[] = 
{
	mmioFOURCC('F', 'M', 'P', '4'),
	mmioFOURCC('D', 'I', 'V', 'X'),
	mmioFOURCC('D', 'X', '5', '0'),
	mmioFOURCC('X', 'V', 'I', 'D'),
	mmioFOURCC('M', 'P', '4', 'S'),
	mmioFOURCC('M', '4', 'S', '2'),
	mmioFOURCC( 4 ,  0 ,  0 ,  0 ),
	mmioFOURCC('D', 'I', 'V', '1'),
	mmioFOURCC('B', 'L', 'Z', '0'),
	mmioFOURCC('m', 'p', '4', 'v'),
	mmioFOURCC('U', 'M', 'P', '4'),
	mmioFOURCC('W', 'V', '1', 'F'),
	mmioFOURCC('S', 'E', 'D', 'G'),
	mmioFOURCC('R', 'M', 'P', '4'),
	mmioFOURCC('3', 'I', 'V', '2'),
	mmioFOURCC('W', 'A', 'W', 'V'),
	mmioFOURCC('F', 'F', 'D', 'S'),
	mmioFOURCC('F', 'V', 'F', 'W'),
	mmioFOURCC('D', 'C', 'O', 'D'),
	mmioFOURCC('M', 'V', 'X', 'M'),
	mmioFOURCC('P', 'M', '4', 'V'),
	mmioFOURCC('S', 'M', 'P', '4'),
	mmioFOURCC('D', 'X', 'G', 'M'),
	mmioFOURCC('V', 'I', 'D', 'M'),
	mmioFOURCC('M', '4', 'T', '3'),
	mmioFOURCC('G', 'E', 'O', 'X'),
	mmioFOURCC('H', 'D', 'X', '4'),
	mmioFOURCC('D', 'M', 'K', '2'),
	mmioFOURCC('D', 'I', 'G', 'I'),
	mmioFOURCC('I', 'N', 'M', 'C'),
	mmioFOURCC('E', 'P', 'H', 'V'),
	mmioFOURCC('E', 'M', '4', 'A'),
	mmioFOURCC('M', '4', 'C', 'C'),
	mmioFOURCC('S', 'N', '4', '0'),
	mmioFOURCC('V', 'S', 'P', 'X'),
	mmioFOURCC('U', 'L', 'D', 'X'),
	mmioFOURCC('G', 'E', 'O', 'V'),
	mmioFOURCC('S', 'I', 'P', 'P'),
};

// *****************************************************************************
//
//	AUDIO FORMATS
//
// *****************************************************************************
typedef struct {
	int	format;
	char	*name;
} AUDIO_FORMAT;

static const AUDIO_FORMAT audio_formats[] = {
	WAVE_FORMAT_PCM,   		"PCM",		
	WAVE_FORMAT_ALAW,   		"A-law",	
	WAVE_FORMAT_MULAW,   		"u-law",	
	WAVE_FORMAT_IMA,		"ADPCM",	
	WAVE_FORMAT_IMA_QT,		"IMA-QT",	
	WAVE_FORMAT_MPEGLAYER3,		"MP3",		
	WAVE_FORMAT_MPEG,		"MP2",		
	WAVE_FORMAT_MSAUDIO2,		"WMA",		
	WAVE_FORMAT_AAC,		"AAC",		
	WAVE_FORMAT_AAC_LATM,		"AAC-LATM",		
	WAVE_FORMAT_FAAD_AAC,		"AAC",		
	WAVE_FORMAT_AC3,		"AC3",		
	WAVE_FORMAT_DTS,		"Digital",
	WAVE_FORMAT_DTS_HD,		"Digital-HD",
	WAVE_FORMAT_ALAC,		"ALAC",		
	WAVE_FORMAT_COOK,		"RealAudio COOK",		
	WAVE_FORMAT_NELLY_MOSER,	"NellyMoser",
	WAVE_FORMAT_FLV_ADPCM,		"Flash ADPCM",
	WAVE_FORMAT_MS_ADPCM,		"MS-ADPCM",	
	WAVE_FORMAT_MSAUDIO1,		"WMA v1",	
	WAVE_FORMAT_MSAUDIO3,		"WMA-Pro",	
	WAVE_FORMAT_MSAUDIO_SPEECH, 	"WMA-Voice",
	WAVE_FORMAT_MSAUDIO_LOSSLESS,	"WMA-Lossless",	
	WAVE_FORMAT_VOICEAGE_AMR, 	"AMR",	
	WAVE_FORMAT_VOICEAGE_AMR_WB, 	"AMR-WB",	
	WAVE_FORMAT_FLAC, 		"FLAC",	
	WAVE_FORMAT_OGG1,		"Vorbis 1",
	WAVE_FORMAT_OGG2,		"Vorbis 2",
	WAVE_FORMAT_OGG3,		"Vorbis 3",
	WAVE_FORMAT_OGG1_PLUS,		"Vorbis 1+",
	WAVE_FORMAT_OGG2_PLUS,		"Vorbis 2+",
	WAVE_FORMAT_OGG3_PLUS,		"Vorbis 3+",
	WAVE_FORMAT_OPUS,		"Opus",
	WAVE_FORMAT_WAVPACK,		"Wavpack",
	WAVE_FORMAT_TTA,		"TTA True Audio",
	WAVE_FORMAT_ON2_AVC_AUDIO,	"ON2 AVC-Audio",
	WAVE_FORMAT_TRUEHD,		"TrueHD",
	WAVE_FORMAT_EAC3,		"EAC3",
	WAVE_FORMAT_PCM_BLURAY, 	"PCM(BR)",		
	
	WAVE_FORMAT_LAVC, 		"lavc",		
};

// *****************************************************************************
//
//	SUBTITLE FORMATS
//
// *****************************************************************************
typedef struct {
	int	format;
	char	*name;
} SUB_FORMAT;

static const SUB_FORMAT sub_formats[] = {
	SUB_FORMAT_DVD_GFX,	"DVD",		
	SUB_FORMAT_DVD,		"DVD",		
	SUB_FORMAT_DVBT,	"DVB",		
	SUB_FORMAT_TEXT,	"TEXT",		
	SUB_FORMAT_XSUB,	"XSUB",		
	SUB_FORMAT_SSA,   	"SSA",		
	SUB_FORMAT_ASS,   	"ASS",		
	SUB_FORMAT_EXT,   	"EXTERNAL",		
};

// *****************************************************************************
//
//	av_get_type_name
//
// *****************************************************************************
const char *av_get_type_name( int type )
{
	int i;
	for( i = 0; i < sizeof( type_names) / sizeof( TYPE_NAME ); i ++ ) {
		if( type == type_names[i].type ) {
			return type_names[i].name;
		}
	}

	// unknown
	return type_names[0].name;
}

// *****************************************************************************
//
//	av_get_type_from_name
//
// *****************************************************************************
int av_get_type_from_name( const char *name )
{
	int i;
	for( i = 0; i < sizeof( type_names) / sizeof( TYPE_NAME ); i ++ ) {
		if( !strcmp( name, type_names[i].name) ) {
			return type_names[i].type;
		}
	}

	// unknown
	return type_names[0].type;
}

// *****************************************************************************
//
//	av_get_etype_name
//
// *****************************************************************************
const char *av_get_etype_name( int etype )
{
	int i;
	for( i = 0; i < sizeof( etype_names) / sizeof( ETYPE_NAME ); i ++ ) {
		if( etype == etype_names[i].etype ) {
			return etype_names[i].name;
		}
	}

	// unknown
	return etype_names[0].name;
}

// *****************************************************************************
//
//	av_get_etype_from_name
//
// *****************************************************************************
int av_get_etype_from_name( const char *name )
{
	int i;
	for( i = 0; i < sizeof( etype_names) / sizeof( ETYPE_NAME ); i ++ ) {
		if( !strcmp( name, etype_names[i].name) ) {
			return etype_names[i].etype;
		}
	}

	// unknown
	return etype_names[0].etype;
}

static UINT32 uppercase_fourcc( UINT32 f_src )
{
	UINT32 	f_dst;
	char 	*dst = (char*)&f_dst;
	char 	*src = (char*)&f_src;
	int 	i;
	for( i = 0; i < 4; i++ )
		*dst++ = toupper( *src++ );
	return f_dst;
}

// *****************************************************************************
//
//	_is_mpeg4
//
// *****************************************************************************
static int is_mpeg4( UINT32 fourCC )
{
	int i;
	for( i = 0; i < sizeof( mpeg4_formats) / sizeof( uint32_t ); i ++ ) {
		if( uppercase_fourcc( fourCC ) == uppercase_fourcc( mpeg4_formats[i] ) ) {
			return 1;
		}
	}
	return 0;
}

// *****************************************************************************
//
//	video_get_format_from_fourcc
//
// *****************************************************************************
int video_get_format_from_fourcc( UINT32 fourCC, int *_subfmt ) 
{
	// always assume MPG4 if not told otherwise!
	int format = VIDEO_FORMAT_UNKNOWN;
	int subfmt = VIDEO_SUBFMT_NONE;

	int i;
	for( i = 0; i < sizeof( video_formats) / sizeof( VIDEO_FORMAT ); i ++ ) {
		if( uppercase_fourcc( fourCC ) == uppercase_fourcc( video_formats[i].fourCC ) ) {
			format = video_formats[i].format;
			subfmt = video_formats[i].subfmt;
			break;
		}
	}
	
	if( format == VIDEO_FORMAT_UNKNOWN && is_mpeg4( fourCC ) ) {
		format = VIDEO_FORMAT_MPG4;
		subfmt = VIDEO_SUBFMT_NONE;
	}

	if( _subfmt )
		*_subfmt = subfmt;
	return format;
}

// *****************************************************************************
//
//	video_get_fourcc_name
//
// *****************************************************************************
const char *video_get_fourcc_name( VIDEO_PROPERTIES *video )
{
	if( video->format == VIDEO_FORMAT_LAVC ) {
		return video->codec_name;
	}
	int i;
	for( i = 0; i < sizeof( video_formats) / sizeof( VIDEO_FORMAT ); i ++ ) {
		if( uppercase_fourcc( video->fourcc ) == video_formats[i].fourCC ) {
			return video_formats[i].name;
		}
	}

	// default
	return is_mpeg4( video->fourcc ) ? "MPEG-4" : "(unknown)";
}

// *****************************************************************************
//
//	video_get_format_name
//
// *****************************************************************************
const char *video_get_format_name( VIDEO_PROPERTIES *video )
{
	if( video->format == VIDEO_FORMAT_LAVC ) {
		return video->codec_name;
	}
	int i;
	for( i = 0; i < sizeof( video_formats) / sizeof( VIDEO_FORMAT ); i ++ ) {
		if( video->format != 0 && video->format == video_formats[i].format ) {
			return video_formats[i].name;
		}
	}

	// default
	return "(unknown)";
}

// *****************************************************************************
//
//	audio_get_format_name
//
// *****************************************************************************
const char *audio_get_format_name( AUDIO_PROPERTIES *audio )
{
	if( audio->format == WAVE_FORMAT_LAVC ) {
		return audio->codec_name;
	}
	int i;
	for( i = 0; i < sizeof( audio_formats) / sizeof( AUDIO_FORMAT ); i ++ ) {
		if( audio->format == audio_formats[i].format ) {
			return audio_formats[i].name;
		}
	}
	
	// default
	return "(unknown)";
}

// *****************************************************************************
//
//	audio_get_channel_layout_name
//
// *****************************************************************************
const char *audio_get_channel_layout_name( int channels )
{
	static const char *ch[] = {
		"",		// 0
		"Mono",		// 1
		"Stereo",	// 2
		"2.1",		// 3
		"4ch",		// 4
		"5ch",		// 5
		"5.1",		// 6
		"7ch",		// 7
		"7.1",		// 8
	};

	if( channels > 8 ) {
		return "";
	}
	return ch[channels];
}

// *****************************************************************************
//
//	sub_get_format_name
//
// *****************************************************************************
const char *sub_get_format_name( SUB_PROPERTIES *sub )
{
	int i;
	for( i = 0; i < sizeof( sub_formats) / sizeof( SUB_FORMAT ); i ++ ) {
		if( sub->format == sub_formats[i].format ) {
			return sub_formats[i].name;
		}
	}
	
	// default
	return "s_unknownformat";
}

static void show_extra( unsigned char *data, int size )
{
	int i;
	for ( i = 0; i < size; i++ ) {
serprintf("%02X", data[i] );
		if( i % 4 == 3 ) {
serprintf(" " );
		}
	}
serprintf("\r\n" );

}

// *****************************************************************************
//
//	show_video_props
//
// *****************************************************************************
void show_video_props( VIDEO_PROPERTIES *video )
{
	serprintf("video %d\r\n", video->valid);
	serprintf("  stream   %d\r\n", video->stream);
	serprintf("  fourcc   [%.4s] [%s]\r\n", (UCHAR*)&video->fourcc, video_get_fourcc_name(video) );
	serprintf("  codec    [%d]  format  %d/%d\r\n", video->codec_id, video->format, video->subfmt);
	serprintf("  profile  %d [%s]  level %d [%s]\r\n", video->profile, video->profile_name, video->level, video->level_name);
	if( video->format == VIDEO_FORMAT_H264 ) {
		serprintf("  entropy  [%s]\r\n", video->sps.entropy_coding_mode_flag ? "CABAC" : "CAVLC" );
	}
	if( video->sprite_usage ) {
		serprintf("  sprites  [%s]\n", (video->sprite_usage == 1) ? "STATIC" : "GMC" );
	}
	serprintf("  kbit/s   %d\r\n", video->bytesPerSec / 125);
	serprintf("  scale    %u\r\n", video->scale);
	serprintf("  rate     %u\r\n", video->rate);
	serprintf("  fps      %d\r\n", video->framesPerSec);
	serprintf("  frames   %d\r\n", video->frames);
	serprintf("  msec     %d\r\n", video->msPerFrame);
	serprintf("  WxH      %4d x %4d\r\n", video->width, video->height);
	if( video->padded_width || video->padded_height )
		serprintf("  pad WxH  %4d x %4d\r\n", video->padded_width, video->padded_height);
	serprintf("  aspect   %d/%d\r\n", video->aspect_n, video->aspect_d );
	if( video->aspect_d )
		serprintf("  disp_wid %d\r\n", (video->width * video->aspect_n) / video->aspect_d);
	if( video->rotation )
		serprintf("  rotation %d\r\n", video->rotation);
	serprintf("  duration %d\r\n", video->duration );
	serprintf("  reorder  %d  never_reorder %d\r\n", video->reorder_pts, video->never_reorder_pts );
	if( video->interlaced )
		serprintf("  interl.  %d\r\n", video->interlaced);
	if( video->stereo_mode )
		serprintf("  stereo   %d\r\n", video->stereo_mode);
	if( video->crypted )
		serprintf("  crypted, type %d\r\n", video->crypt_type);
	if( video->compressed )
		serprintf("  compressed, algo %d\r\n", video->comp_algo);
	
	if( video->extraDataSize ) {
		serprintf("  extraSz %d  data  ", video->extraDataSize );
		show_extra( video->extraData, video->extraDataSize ); 
	}
	
	serprintf("\r\n");
}

// *****************************************************************************
//
//	show_short_video_props
//
// *****************************************************************************
void show_short_video_props( VIDEO_PROPERTIES *video )
{
	serprintf("VID(%d)", video->stream);
	serprintf(" [%.4s][%s]", (UCHAR*)&video->fourcc, video_get_fourcc_name(video) );
	serprintf(" %d fps ", video->framesPerSec);
	serprintf(" %d kb/s ", video->bytesPerSec / 125);
	serprintf(" %dx%d\n", video->width, video->height);
}

// *****************************************************************************
//
//	show_audio_props
//
// *****************************************************************************
void show_audio_props( AUDIO_PROPERTIES *audio )
{
	serprintf("audio %d\r\n", audio->valid);
	serprintf("  stream  %d\r\n", audio->stream);
	serprintf("  name    [%s]\r\n", audio->name);
    if( audio->priority )
	serprintf("  priority %d\r\n", audio->priority);
	serprintf("  codec   [%d]  format  %04X/%d [%s]\r\n", audio->codec_id, audio->format, audio->subfmt, audio_get_format_name( audio ) );
	serprintf("  kbit/s  %d\r\n", audio->bytesPerSec / 125);
	serprintf("  scale   %d\r\n", audio->scale);
	serprintf("  rate :  %d\r\n", audio->rate);
	serprintf("  balign  %d\r\n", audio->blockAlign);			
	serprintf("  frames  %d\r\n", audio->frames);
    if( audio->sourceChannels )
	serprintf("  chann   %d -> %d\r\n", audio->sourceChannels, audio->channels);
    else 
	serprintf("  chann   %d\r\n", audio->channels);
    if( audio->sourceSamples )
	serprintf("  samp/s  %d -> %d\r\n", audio->sourceSamples, audio->samplesPerSec);
    else
	serprintf("  samp/s  %d\r\n", audio->samplesPerSec);
    if( audio->channelMask )
	serprintf("  ch_mask %04X\r\n", audio->channelMask);
	
	serprintf("  bits    %d -> %d  %s\r\n", audio->sourceBitsPerSample, audio->bitsPerSample, ( audio->bitsPerSample == 16 ) ? (audio->byteOrder ? "BE" : "LE" ) : "" );
	serprintf("  samp/B  %d\r\n", audio->samplesPerBlock);
	serprintf("  vbr     %d\r\n", audio->vbr);
	serprintf("  durat.  %d\r\n", audio->duration );
	serprintf("  crypted %s  type %d\r\n", audio->crypted ? "YES" : "no", audio->crypt_type);
	
	if( audio->extraDataSize ) {
		serprintf("  extraSz %d  data ", audio->extraDataSize );
		show_extra( audio->extraData, audio->extraDataSize ); 
	} 
	if( audio->extraDataSize2 && audio->extraData2 ) {
		serprintf("  extraS2 %d\r\n", audio->extraDataSize2 );
		Dump( audio->extraData2, MIN( 32, audio->extraDataSize2 ) );
serprintf("\r\n" );
	} 
	
	serprintf("\r\n");
}

// *****************************************************************************
//
//	show_short_audio_props
//
// *****************************************************************************
void show_short_audio_props( AUDIO_PROPERTIES *audio )
{
	serprintf("AUD(%d)", audio->stream);
	serprintf(" [%s] ", audio->name);
	serprintf("%04X/%d [%s]", audio->format, audio->subfmt, audio_get_format_name( audio ) );
	serprintf(" %d kb/s ", audio->bytesPerSec / 125);
	serprintf(" %d ch ", audio->channels);
	serprintf(" %d samp/s\r\n", audio->samplesPerSec);
}

// *****************************************************************************
//
//	show_subtitle_props
//
// *****************************************************************************
void show_subtitle_props( SUB_PROPERTIES *sub )
{
	serprintf("subtitle  %d\r\n", sub->valid);
	serprintf("  stream  %d\r\n", sub->stream);
	serprintf("  codec   [%d]  format  %d [%s]\r\n", sub->codec_id, sub->format, sub_get_format_name( sub ) );
	serprintf("  name    [%s]\r\n", sub->name);
	serprintf("  gfx     %d\r\n", sub->gfx);
	serprintf("  ext     %d\r\n", sub->ext);
	serprintf("  scale   %u\r\n", sub->scale);
	serprintf("  rate    %u\r\n", sub->rate);
	serprintf("  crypted %s  type %d\r\n", sub->crypted ? "YES" : "no", sub->crypt_type);
	
	if( sub->extraDataSize ) {
		serprintf("  extraSz  %d  data ", sub->extraDataSize );
		show_extra( sub->extraData, sub->extraDataSize ); 
	} 
	if( sub->extraDataSize2 && sub->extraData2 ) {
		serprintf("  extraS2  %d\r\n", sub->extraDataSize2 );
		Dump( sub->extraData2, MIN( 32, sub->extraDataSize2 ) );
serprintf("\r\n" );
	} 
	serprintf("\r\n");
}

// *****************************************************************************
//
//	show_short_subtitle_props
//
// *****************************************************************************
void show_short_subtitle_props( SUB_PROPERTIES *sub )
{
	serprintf("SUB(%d)", sub->stream);
	serprintf(" %d[%s] ", sub->format, sub_get_format_name( sub ) );
	serprintf("[%s]\n", sub->name);
}

// *****************************************************************************
//
//	show_av_props
//
// *****************************************************************************
void show_av_props( AV_PROPERTIES *av )
{
	int i;
	
	for( i = 0; i < av->vs_max; i++)
		show_video_props( av->video + i );
	for( i = 0; i < av->as_max; i++)
		show_audio_props( av->audio + i );
	for( i = 0; i < av->subs_max; i++)
		show_subtitle_props( av->sub + i );
}

// *****************************************************************************
//
//	show_short_av_props
//
// *****************************************************************************
void show_short_av_props( AV_PROPERTIES *av )
{
	int i;
	
	for( i = 0; i < av->vs_max; i++)
		show_short_video_props( av->video + i );
	for( i = 0; i < av->as_max; i++)
		show_short_audio_props( av->audio + i );
	for( i = 0; i < av->subs_max; i++)
		show_short_subtitle_props( av->sub + i );
}

int av__gcd(int a, int b) 
{ 
   return ( b != 0 ? av__gcd(b, a % b) : a ); 
}

void av__reduce( int *a, int *b )
{
	int c = av__gcd( *a, *b );
	if( c ) {
		*a /= c;
		*b /= c;
	}
}

#define VIDEO_AREA_HD 500000

int av_is_HD( int width, int height )
{
	return( width * height > VIDEO_AREA_HD );
}

// ************************************************************
//
//	video_frame_copy_props
//
// ***********************************************************
void video_frame_copy_props( VIDEO_FRAME *dst, VIDEO_FRAME *src )
{
	unsigned char 	*dst_data   = dst->data[0];
	int		dst_index   = dst->index;
	int		dst_size    = dst->size;
	void		(*dst_destroy)(VIDEO_FRAME *) = dst->destroy;
	memcpy( dst, src, sizeof(VIDEO_FRAME));
	dst->data[0]    = dst_data;
	dst->index      = dst_index;
	dst->size       = dst_size;
	dst->destroy    = dst_destroy;
}

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
#include "file_type.h"
#include "debug.h"
#include "av.h"
#include "app_av.h"
#include "util.h"

#include "stream_config.h"

#include <ctype.h>

#define DBG if(Debug[DBG_FILE])

static int allowed( int type, int etype )
{
	return 1;
}

typedef struct file_type_str {
	const char 	*ext;
	const char 	**mime_type;
	const char 	*name;
	int 		type;
	int 		etype;
	int 		(*is_allowed) (int, int);	// if defined and returns 0 the type is not recognized
	int		probe;
} FILE_TYPE;

/*
 * for ANDROID, keep in sync with:
 *
 * 	mydroid/frameworks/base/media/java/android/media/MediaFile.java
 *		public class MediaFile {
 *     			addFileType("XYZ", FILE_TYPE_XYZ, "audio/xyz");
 *
 *	mydroid/libcore/luni/src/main/java/libcore/net/MimeUtils.java
 *		public final class MimeUtils {
 *			static {
 *				add("audio/xyz, "xyz");
 */
 
static const char *mime_type_flv[] =
{
	"video/x-flv",
	"video/flv",
	NULL,
};

static const char *mime_type_mkv[] =
{
	"video/x-matroska",
	NULL,
};

static const char *mime_type_mka[] =
{
	"audio/x-matroska",
	NULL,
};

static const char *mime_type_wtv[] =
{
	"video/wtv", 
	"video/x-ms-wtv",
	NULL,
};

static const char *mime_type_webm[] =
{
	"video/webm",
	NULL,
};

static const char *mime_type_asf[] =
{
	"video/x-ms-asf",
	NULL,
};
static const char *mime_type_wmv[] =
{
	"video/x-ms-wmv",
	NULL,
};

static const char *mime_type_wvx[] =
{
	"video/x-ms-wvx",
	NULL,
};

static const char *mime_type_wmx[] =
{
	"video/x-ms-wmx",
	NULL,
};

static const char *mime_type_mp4[] =
{
	"video/mp4",
	"video/m4v",
	NULL
};

static const char *mime_type_m4a[] =
{
	"audio/mp4",
	NULL
};

static const char *mime_type_aac[] =
{
	"audio/aac",
	"audio/x-aac",
	NULL
};

static const char *mime_type_qt[] =
{
	"video/quicktime",
	"video/x-quicktime",
	NULL,
};

#ifdef CONFIG_3GP
static const char *mime_type_3gp[] =
{
	"audio/3gpp",
	"video/3gpp",
	"video/3gpp2",
	NULL,
};
#endif

#ifdef CONFIG_REALVIDEO
static const char *mime_type_rm[] =
{
	"video/vnd.rn-realvideo",
	NULL,
};
static const char *mime_type_smil[] =
{
	"application/smil",
	NULL,
};
#endif

static const char *mime_type_mp3[] =
{
	"audio/mpeg",
	"audio/mpegurl",
	NULL,
};

static const char *mime_type_mpeg[] =
{
	"video/mpeg",
	"video/x-mpeg",
	"video/mpg",
	"video/x-mpg",
	"video/dvd",
	NULL,
};

static const char *mime_type_ts[] =
{
	"video/mp2t",
	NULL
};

static const char *mime_type_ps[] =
{
	"video/mp2p",
	NULL
};

#ifdef CONFIG_AUDIO
static const char *mime_type_m3u[] =
{
	"audio/x-mpegurl",
	NULL,
};
#endif

static const char *mime_type_wav[] =
{
	"audio/x-wav",
	NULL,
};

#ifdef CONFIG_OGG
static const char *mime_type_ogg[] =
{
	"application/ogg",
	NULL,
};
#endif

#ifdef CONFIG_WAVPACK
static const char *mime_type_wv[] =
{
	"audio/x-wavpack",
	"audio/wavpack",
	"application/x-wavpack",
	NULL,
};
#endif

#ifdef CONFIG_TTA
static const char *mime_type_tta[] =
{
	"audio/x-tta",
	"audio/tta",
	NULL,
};
#endif

#ifdef CONFIG_AMR
static const char *mime_type_amr[] =
{
	"audio/amr",
	NULL,
};
#endif

static const char *mime_type_flac[] =
{
	"audio/flac",
	NULL,
};

#ifdef CONFIG_WMA
static const char *mime_type_wma[] =
{
	"audio/x-ms-wma",
	NULL,
};
#endif

static const char *mime_type_avi[] =
{
	"video/x-msvideo",
	NULL,
};

static const FILE_TYPE file_type[] =
{
//	ext		mime_type	name		type		etype		is_allowed
//   	---------------------------------------------------------------------------------------------------------------------
#ifdef CONFIG_AUDIO
	{ "M3U",	mime_type_m3u,	"M3U",		TYPE_LST, 	ETYPE_M3U,	allowed },
  #ifdef CONFIG_MP3
	{ "MP3",	mime_type_mp3,	"MP3",		TYPE_AUD, 	ETYPE_MP3,	allowed },
	{ "MP2",	mime_type_mp3,	"MP2",		TYPE_AUD, 	ETYPE_MP3,	allowed },
  #endif
  #ifdef CONFIG_PCM
	{ "WAV",	mime_type_wav,	"WAV",		TYPE_AUD, 	ETYPE_PCM,	allowed },
  #endif
  #ifdef CONFIG_WMA
	{ "WMA",	mime_type_wma,	"WMA",		TYPE_AUD, 	ETYPE_WMA,	allowed },
  #endif
  #ifdef CONFIG_AAC
	{ "M4A",	mime_type_m4a,	"M4A",		TYPE_AUD, 	ETYPE_AAC,	allowed },
	{ "M4B",	NULL,		"M4A",		TYPE_AUD, 	ETYPE_AAC,	allowed },
	{ "AAC",	mime_type_aac,	"AAC",		TYPE_AUD, 	ETYPE_AAC,	allowed },
	{ "ADTS",	NULL,		"ADTS",		TYPE_AUD, 	ETYPE_AAC,	allowed },
  #endif
	{ "AC3",	NULL,		"AC3",		TYPE_VID, 	ETYPE_AC3,	allowed },
  #ifdef CONFIG_AMR
	{ "AMR",	mime_type_amr,	"AMR",		TYPE_AUD, 	ETYPE_AMR,	allowed },
  #endif	
  #ifdef CONFIG_FLAC
	{ "FLAC",	mime_type_flac,	"FLAC",		TYPE_AUD, 	ETYPE_FLAC,	allowed },
  #endif	
  #ifdef CONFIG_OGG
	{ "OGG",	mime_type_ogg,	"OGG",		TYPE_AUD, 	ETYPE_OGG,	allowed },
	{ "OGA",	mime_type_ogg,	"OGG",		TYPE_AUD, 	ETYPE_OGG,	allowed },
  #endif	
  #ifdef CONFIG_OGV
	{ "OGV",	mime_type_ogg,	"OGV",		TYPE_VID, 	ETYPE_OGV,	allowed },
	{ "OGM",	mime_type_ogg,	"OGV",		TYPE_VID, 	ETYPE_OGV,	allowed },
  #endif	
#else // CONFIG_AUDIO
	{ "...",	NULL,		"...",		TYPE_NONE, 	ETYPE_NONE,	allowed },
#endif	

#ifdef CONFIG_VIDEO
	{ "AVI",	mime_type_avi,	"AVI",		TYPE_VID, 	ETYPE_AVI,	allowed },
	{ "GVI",	NULL,		"AVI",		TYPE_VID, 	ETYPE_AVI,	allowed },
	{ "DIVX",	mime_type_avi,	"AVI",		TYPE_VID, 	ETYPE_AVI,	allowed },
#endif
#ifdef CONFIG_ASF
	{ "ASF",	mime_type_asf,	"WMV",		TYPE_VID, 	ETYPE_ASF,	allowed },
	{ "WMV",	mime_type_wmv,	"WMV",		TYPE_VID, 	ETYPE_ASF,	allowed },
#ifdef CONFIG_DVR_MS
	{ "DVR-MS",	mime_type_wmv,	"WMV",		TYPE_VID, 	ETYPE_ASF,	allowed },
#endif
	{ "ASX",	mime_type_asf,	"WMV",		TYPE_LST, 	ETYPE_ASX,	allowed },
	{ "WVX",	mime_type_wvx,	"WMV",		TYPE_LST, 	ETYPE_ASX,	allowed },
	{ "WMX",	mime_type_wmx,	"WMV",		TYPE_LST, 	ETYPE_ASX,	allowed },
#endif
#ifdef CONFIG_MP4
	{ "MP4",	mime_type_mp4,	"MP4",		TYPE_VID, 	ETYPE_MP4,	allowed },
	{ "M4V",	mime_type_mp4,	"MP4",		TYPE_VID, 	ETYPE_MP4,	allowed },
	{ "MOV",	mime_type_qt,	"MP4",		TYPE_VID, 	ETYPE_MP4,	allowed }, // or should it be MTP_OFC_MP4_Container ?
	{ "QT",		mime_type_qt,	"MP4",		TYPE_VID, 	ETYPE_MP4,	allowed },
#ifdef CONFIG_3GP
	{ "3GP",	mime_type_3gp,	"3GP",		TYPE_VID, 	ETYPE_MP4,	allowed },
	{ "3GPP",	mime_type_3gp,	"3GPP",		TYPE_VID, 	ETYPE_MP4,	allowed },
#endif
#endif
#ifdef CONFIG_MPEG_PS
	{ "MPG",	mime_type_mpeg,	"MPG",		TYPE_VID, 	ETYPE_MPEG_PS,	allowed },
	{ "MPE",	mime_type_mpeg,	"MPG",		TYPE_VID, 	ETYPE_MPEG_PS,	allowed },
	{ "MPEG",	mime_type_mpeg,	"MPG",		TYPE_VID, 	ETYPE_MPEG_PS,	allowed },
	{ "PS",		mime_type_ps,	"MPG",		TYPE_VID, 	ETYPE_MPEG_PS,	allowed },
	{ "VOB",	mime_type_mpeg,	"MPG",		TYPE_VID, 	ETYPE_MPEG_PS,	allowed },
	{ "VRO",	NULL,		"MPG",		TYPE_VID, 	ETYPE_MPEG_PS,	allowed },
#endif
#ifdef CONFIG_MPEG_TS
	{ "TS",		mime_type_ts,	"MPG",		TYPE_VID, 	ETYPE_MPEG_TS,	allowed },
	{ "TRP",	NULL,		"MPG",		TYPE_VID, 	ETYPE_MPEG_TS,	allowed },
	{ "MTS",	NULL,		"MPG",		TYPE_VID, 	ETYPE_MPEG_TS,	allowed },
	{ "M2TS",	NULL,		"MPG",		TYPE_VID, 	ETYPE_MPEG_TS,	allowed },
	{ "DVR0",	NULL,		"MPG",		TYPE_VID, 	ETYPE_MPEG_TS,	allowed },
#endif
#ifdef CONFIG_MPEG2
	{ "M2V",	NULL,		"MPG",		TYPE_VID, 	ETYPE_MPEG_RAW,	allowed },
#endif	
#ifdef CONFIG_H264
	{ "264",	NULL,		"H264",		TYPE_VID, 	ETYPE_H264_RAW,	allowed },
	{ "H264",	NULL,		"H264",		TYPE_VID, 	ETYPE_H264_RAW,	allowed },
#endif
#ifdef CONFIG_MPG4
	{ "MPG4",	NULL,		"MPEG4",	TYPE_VID, 	ETYPE_MPG4_RAW, allowed },
	{ "MPEG4",	NULL,		"MPEG4",	TYPE_VID, 	ETYPE_MPG4_RAW, allowed },
#endif
#ifdef CONFIG_AMV
	{ "AMV",	NULL,		"AMV",		TYPE_VID,	ETYPE_AMV,	allowed },
#endif
#ifdef CONFIG_FLV
	{ "FLV",	mime_type_flv,	"FLV",		TYPE_VID, 	ETYPE_FLV,	allowed },
#endif
#ifdef CONFIG_MKV
	{ "MKV",	mime_type_mkv,	"MKV",		TYPE_VID, 	ETYPE_MKV,	allowed },
	{ "MKA",	mime_type_mka,	"MKA",		TYPE_VID, 	ETYPE_MKV,	allowed },
	{ "WEBM",	mime_type_webm,	"WEBM",		TYPE_VID, 	ETYPE_MKV,	allowed },
#endif
#ifdef CONFIG_MKA // one day we will play MKA for audio
	{ "MKA",	mime_type_mka,	"MKA",		TYPE_AUD, 	ETYPE_MKV,	allowed },
#endif
#ifdef CONFIG_WTV
	{ "WTV",	mime_type_wtv ,	"WTV",		TYPE_VID, 	ETYPE_WTV,	allowed },
#endif
#ifdef CONFIG_DTS
	{ "DTS",	NULL,		"DTS",		TYPE_VID, 	ETYPE_DTS,	allowed },
#endif
#ifdef CONFIG_WAVPACK
	{ "WV",		mime_type_wv,	"WV",		TYPE_AUD, 	ETYPE_WAVPACK,	allowed },
#endif
#ifdef CONFIG_TTA
	{ "TTA",	mime_type_tta,	"TTA",		TYPE_AUD, 	ETYPE_TTA,	allowed },
#endif
#ifdef CONFIG_REALVIDEO
	{ "RM",		mime_type_rm,	"RM",		TYPE_VID, 	ETYPE_RM,	allowed },
	{ "RA",		mime_type_rm,	"RM",		TYPE_VID, 	ETYPE_RM,	allowed },
	{ "RAM",	mime_type_rm,	"RM",		TYPE_VID, 	ETYPE_RM,	allowed },
	{ "RMVB",	mime_type_rm,	"RM",		TYPE_VID, 	ETYPE_RM,	allowed },
	{ "SMIL",	mime_type_smil,	"SMIL",		TYPE_LST, 	ETYPE_MP4,	allowed },
#endif
#ifdef CONFIG_SUBTITLES
	{ "SSA",	NULL,		"SUB",		TYPE_SUBT,	ETYPE_NONE,	allowed },
	{ "ASS",	NULL,		"SUB",		TYPE_SUBT,	ETYPE_NONE,	allowed },
	{ "SRT",	NULL,		"SUB",		TYPE_SUBT,	ETYPE_NONE,	allowed },
	{ "SUB",	NULL,		"SUB",		TYPE_SUBT,	ETYPE_NONE,	allowed },
	{ "SMI",	NULL,		"SUB",		TYPE_SUBT,	ETYPE_NONE,	allowed },
#endif
};

#ifdef CONFIG_GUI
#include "lcd_bitmaps.h"

struct file_icon_str {
	int 	type;
	int 	etype;
	int	small_icon;		// icon to display in the media browsers, the file browser or the info screen
	int	infopanel_icon;		// icon to display in the browsers info panel 
};

static struct file_icon_str file_icon[] =
{
	// special folders
	{ TYPE_DIR, 		ETYPE_GENRE,			SYM_FILETYPE_GENRE,     	SYM_INFOPANEL_FILETYPE_GENRE },
	{ TYPE_DIR, 		ETYPE_DATE,			SYM_FILETYPE_YEAR,		SYM_INFOPANEL_FILETYPE_YEAR },
	{ TYPE_DIR, 		ETYPE_ARTIST,			SYM_FILETYPE_ARTIST,		SYM_INFOPANEL_FILETYPE_ARTIST },
	{ TYPE_DIR, 		ETYPE_ALBUM,			SYM_FILETYPE_ALBUM,		SYM_INFOPANEL_FILETYPE_ALBUM },
	{ TYPE_DIR, 		ETYPE_TITLE,			SYM_FILETYPE_TITLE,		SYM_INFOPANEL_FILETYPE_TITLE },
	{ TYPE_DIR, 		ETYPE_PLAYLIST,			SYM_FILETYPE_PLAYLIST,		SYM_INFOPANEL_FILETYPE_PLAYLIST },
	{ TYPE_DIR, 		ETYPE_PODCAST,			SYM_FILETYPE_RADIO,		SYM_INFOPANEL_FILETYPE_RADIO },
	{ TYPE_DIR, 		ETYPE_MEDIA,			SYM_FILETYPE_FOLDER,		SYM_INFOPANEL_FILETYPE_FOLDER },
	{ TYPE_DIR, 		ETYPE_FAVORITES,		SYM_FILETYPE_RATINGS,     	SYM_INFOPANEL_FILETYPE_RATINGS },
	{ TYPE_DIR, 		ETYPE_U_ARTIST,			SYM_FILETYPE_UNKNOWN_ARTIST,	SYM_INFOPANEL_FILETYPE_UNKNOWN_ARTIST },
	{ TYPE_DIR, 		ETYPE_U_ALBUM,			SYM_FILETYPE_UNKNOWN_ALBUM, 	SYM_INFOPANEL_FILETYPE_UNKNOWN_ALBUM },
	{ TYPE_DIR,		ETYPE_PHOTO,			SYM_FILETYPE_PIC,		SYM_INFOPANEL_FILETYPE_PIC },
	{ TYPE_DIR,		ETYPE_RATING,			SYM_FILETYPE_RATINGS,		SYM_INFOPANEL_FILETYPE_RATINGS },
	// generic folder
	{ TYPE_DIR, 		ETYPE_ANY,			SYM_FILETYPE_FOLDER,		SYM_INFOPANEL_FILETYPE_FOLDER },
	{ TYPE_AUD, 		ETYPE_ANY,			SYM_FILETYPE_NOTE,		SYM_INFOPANEL_FILETYPE_NOTE },
	{ TYPE_LST, 		ETYPE_ANY,			SYM_FILETYPE_PLAYLIST,		SYM_INFOPANEL_FILETYPE_PLAYLIST },
	{ TYPE_PIC,		ETYPE_ANY,			SYM_FILETYPE_PIC,		SYM_INFOPANEL_FILETYPE_PIC },
	{ TYPE_VID,		ETYPE_ANY,			SYM_FILETYPE_VIDEO,		SYM_INFOPANEL_FILETYPE_VIDEO },
	{ TYPE_UPDATE,		ETYPE_ANY, 			SYM_FILETYPE_UPDATE,		0 },
#ifdef CONFIG_SUBTITLES
	{ TYPE_SUBT,		ETYPE_ANY, 			SYM_FILETYPE_SUBTITLES,		0 },
#endif
#ifdef CONFIG_LYRICS
	{ TYPE_LYR,		ETYPE_ANY, 			SYM_FILETYPE_LYRICS,		0 },
#endif

	{ TYPE_UNKNOWN,		ETYPE_ANY,			SYM_FILETYPE_UNKNOWN,		0 },
	
	{ TYPE_ANY,		ETYPE_ANY,			SYM_FILETYPE_UNKNOWN,		0 },
};


#endif

static FILETYPE_REG *_filetype_reg = NULL;

// ************************************************************
//
//	filetype_register
//
// ************************************************************
int filetype_register( FILETYPE_REG *reg )
{
	if( !_filetype_reg ) {
		_filetype_reg = reg;
	} else {
		FILETYPE_REG *head = _filetype_reg;
		while( head->next ) {
			head = head->next;
		} 
		head->next = reg;
	}
	reg->next = NULL;
	return 0;
}

// *****************************************************************************
//
//	filetype_get_reg
//
// *****************************************************************************
FILETYPE_REG* filetype_get_reg( int type )
{
DBG serprintf("filetype_get_reg( type %d )\r\n", type ); 
	FILETYPE_REG *p = _filetype_reg;
	while( p ) {
		if( type == p->type ) {
			return p;
		}
		p = p->next;
	}
	return NULL;
}

#ifdef DEBUG_MSG
// ************************************************************
//
//	_file_dump_reg
//
// ************************************************************
static void _file_dump_reg( void )
{
	FILETYPE_REG *p = _filetype_reg;
serprintf("\r\nAction:\r\n" );	
	while( p ) {
serprintf("\t%4d  %s\r\n", p->type, p->name );	
		p = p->next;
	}
serprintf("\r\n" ); 
}

DECLARE_DEBUG_COMMAND_VOID( "freg", _file_dump_reg );
#endif

// ************************************************
//
//	detect_file_type
//
// Find out the file type based on mimetype and file ending
//
// ************************************************
int detect_file_type(const char *name, const char *mime_type, int *type, int *etype)
{
	const char *type_plain = "text/plain";
	const char *type_binary = "application/octet-stream";

	// many badly configured webserver set the mimetype to text/plain
	// or application/octet-stream if they dont know a file. for that case
	// we prefer to check for the file ending first.
	// MW: also if the mime-type is NULL. note that the if terminates early
	// if mime_type == NULL.
	if ( !mime_type || !strncmp(type_plain, mime_type, strlen(type_plain)) ||
			!strncmp(type_binary, mime_type, strlen(type_binary)) ) {
DBG serprintf("broken mimetype '%s'\n", mime_type);
		if ( get_file_type(name, type, etype) ) {
			return get_file_type_from_mime_type(mime_type, type, etype);
		}
		return 0;
	}
	// this mimetype looks good so we give it preference over the file ending
	else {
DBG serprintf("good mimetype '%s'\n", mime_type);
		if ( get_file_type_from_mime_type(mime_type, type, etype) ) {
			return get_file_type(name, type, etype);
		}
		return 0;
	}
}

// ************************************************
//
//	get_file_type_from_ext
//
// ************************************************
int get_file_type_from_ext( const char *ext, int *_type, int *_etype, const char **_mime, int *_probe ) 
{
	if( !ext || *ext == '\0' )
		return 1;
		
	int i;
	for( i = 0; i < sizeof( file_type ) / sizeof( FILE_TYPE ); i++ ) {
		if( !strcmpNC( ext, file_type[i].ext ) ) {
			// is this type allowed?
			if( file_type[i].is_allowed && !file_type[i].is_allowed( file_type[i].type, file_type[i].etype ) ) {
				continue;
			}
			if( _type )
				*_type = file_type[i].type;
			if( _etype )
				*_etype = file_type[i].etype;
			if( _mime ) {
				if( file_type[i].mime_type && file_type[i].mime_type[0] )
					*_mime = file_type[i].mime_type[0];
				else
					*_mime = "";
			}
			if( _probe )
				*_probe = file_type[i].probe;
DBG serprintf("%s: detected file as %s\n", __FUNCTION__, file_type[i].name);
			return 0;
		}
	}
	return 1;
}

// ************************************************
//
//	_get_file_type
//
// ************************************************
static int _get_file_type( STREAM_URL *src, int *_type, int *_etype, const char **_mime, int probe ) 
{
	if (src == NULL)
		return 1;

	// the path could be an URL, and URLs are allowed to have params at the end,
	// separated by a "?", so parse the string and drop everything after the last ?
	char name[MAX_PATH_LEN + 1];
	strnZcpy( name, src->url, MAX_PATH_LEN );
	char *c = name + strlen( name ) - 1;
	while( c > name ) {
		if( *c == '?' ) {
DBG serprintf("get_file_type: drop %s\r\n", c );
			*c = '\0';
			break;
		}
		c--;
	}
	
	int must_probe = 0;
	if( !get_file_type_from_ext( get_extension( name ), _type, _etype, _mime, &must_probe ) ) {
		if( !probe || !must_probe ) {
			return 0;
		}
	}

	if( !strncmpNC( name, "rtsp://", strlen( "rtsp://" ) ) ) {
		if( _type )
			*_type = TYPE_VID;
		if( _etype )
			*_etype = ETYPE_RTSP;
		if( _mime )
			*_mime = "";
		return 0;
	}
	
	if( _type )
		*_type = TYPE_UNKNOWN;
	if( _etype )
		*_etype = ETYPE_NONE;
	if( _mime )
		*_mime = "";

DBG serprintf("%s: could not find a file type for '%s'\n", __FUNCTION__, name);
	return 1;
}

// ************************************************
//
//	get_file_type
//
// ************************************************
int get_file_type( const char *_name, int *_type, int *_etype ) 
{
	STREAM_URL src;
	stream_url_cpy_url( &src, _name );
	return _get_file_type( &src, _type, _etype, NULL, 1 );
}

// ************************************************
//
//	get_file_type_no_probe
//
// ************************************************
int get_file_type_no_probe( const char *_name, int *_type, int *_etype ) 
{
	STREAM_URL src;
	stream_url_cpy_url( &src, _name );
	return _get_file_type( &src, _type, _etype, NULL, 0 );
}

// ************************************************
//
//	get_file_type_and_mime
//
// ************************************************
int get_file_type_and_mime( const char *_name, int *_type, int *_etype, const char **_mime ) 
{
	STREAM_URL src;
	stream_url_cpy_url( &src, _name );
	return _get_file_type( &src, _type, _etype, _mime, 1 );
}


// ************************************************
//
//	get_url_type
//
// ************************************************
int get_url_type( STREAM_URL *src,  int *_type, int *_etype )
{
	return _get_file_type( src, _type, _etype, NULL, 1 );
}

// ************************************************
//
//	get_url_type_and_mime
//
// ************************************************
int get_url_type_and_mime( STREAM_URL *src,  int *_type, int *_etype, const char **_mime )
{
	return _get_file_type( src, _type, _etype, _mime, 1 );
}

// ************************************************
//
//	get_type_name
//
// ************************************************
const char *get_type_name( int type, int etype )
{
	int i;

	for( i = 0; i < sizeof( file_type ) / sizeof( FILE_TYPE ); i++ ) {
		if( type == file_type[i].type && etype == file_type[i].etype )
			return file_type[i].name;
	}
	
	return "";
}

// ************************************************
//
//	get_file_type_from_mime_type
//
// ************************************************
int get_file_type_from_mime_type(const char *mime_type, int *type, int *etype)
{
	if (mime_type == NULL)
		return 1;
	
	int i;
	for( i = 0; i < sizeof( file_type ) / sizeof( FILE_TYPE ); i++ ) {
		int j = 0;
		while ( file_type[i].mime_type && file_type[i].mime_type[j] ) {
			if ( !strcmp(mime_type, file_type[i].mime_type[j]) ) {
				if ( type ) {
					*type = file_type[i].type;
				}
				if ( etype ) {
					*etype = file_type[i].etype;
				}
DBG serprintf("%s: detected file as type/etype %d/%d  ext %s  name %s\n", __FUNCTION__, file_type[i].type, file_type[i].etype, file_type[i].ext, file_type[i].name);
				return 0;
			}
			++j;
		}
	}
	
	if( type )
		*type = TYPE_UNKNOWN;
	if( etype )
		*etype = ETYPE_NONE;

DBG serprintf("%s: could not detected file type by its mime type '%s'\n", __FUNCTION__, mime_type);
	return 1;
}

// ************************************************
//
//	get_mime_type_from_file_type
//
// ************************************************
int get_mime_type_from_file_type(int type, int etype, const char **mime)
{
	int i;
	for( i = 0; i < sizeof( file_type ) / sizeof( FILE_TYPE ); i++ ) {
		if( file_type[i].type == type && file_type[i].etype == etype ) {
			if ( file_type[i].mime_type && file_type[i].mime_type[0] ) {
				*mime = file_type[i].mime_type[0];
			}
			return 0;
		}
	}
	
	if( mime )
		*mime = "";
	return 1;
}

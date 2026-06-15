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
#include "stream.h"
#include "stream_parser.h"
#include "debug.h"
#include "astdlib.h"
#include "util.h"
#include "cbe.h"
#include "linked_list.h"
#include "astdlib.h"
#include "mpeg2.h"
#include "h264.h"
#include "hevc.h"
#include "file_info_priv.h"
#include "iso639.h"
#include "android_codec.h"
#include "util.h"

#ifdef CONFIG_STREAM
#ifdef CONFIG_FFMPEG_PARSER

#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/intreadwrite.h>

#include "dovi_nal.h"

#include <string.h>
#include <math.h>

#define DBGS 	if(Debug[DBG_STREAM])
#define DBGP 	if(Debug[DBG_PARSER])
#define DBGP2 	if(Debug[DBG_PARSER] > 1)
#define DBGP3 	if(Debug[DBG_PARSER] > 2)
#define DBGS2   if(Debug[DBG_STREAM] == 2)
#define DBGC1   if((Debug[DBG_CHU]&1) == 1)
#define DBGC2   if((Debug[DBG_CHU]&2) == 2)
#define DBGC4   if((Debug[DBG_CHU]&4) == 4)
#define DBGC8   if((Debug[DBG_CHU]&8) == 8)
#define DBGC32  if((Debug[DBG_CHU]&32) == 32)

#define DBG if(0)
#define DBG2 if(0)

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>

static int max_delay       = 100000;
static int log_debug       = 0;
static int use_pts         = 1;
static int force_reorder   = -1;
static int force_vpid      = 0;
static int force_apid      = 0;

DECLARE_DEBUG_PARAM ("ffmd",  max_delay );
DECLARE_DEBUG_PARAM ("fflog", log_debug );
DECLARE_DEBUG_TOGGLE("ffpts", use_pts );
DECLARE_DEBUG_TOGGLE("ffreo", force_reorder );
DECLARE_DEBUG_PARAM("ffvp",  force_vpid );
DECLARE_DEBUG_PARAM("ffap",  force_apid );

typedef struct PacketNode {
	LinkedListNode s;
	AVPacket packet;
} PacketNode;

typedef struct AVQueue {
	LinkedList	list;
	pthread_mutex_t mutex;
	int		mem_used;
	int 		packets;
} AVQueue;

// max number of Dolby Vision enhancement-layer packets kept while waiting
// for the matching base-layer packet
#define DV_EL_PENDING_MAX 32

typedef struct FF_PRIV 
{
	AVFormatContext *fmt;
        AVDictionary    *fmt_opts;
	
	AVQueue		aq;
	AVQueue		vq;
	AVQueue		sq;
	
	STREAM		*s;
	UINT64		size;
	int		duration;
	int		start_time;
	
	AV_PROPERTIES 	av;
	AUDIO_PROPERTIES *audio;
	VIDEO_PROPERTIES *video;
	SUB_PROPERTIES 	*subtitle;
	
	ID3_TAG		tag;

	int 		flags;
	int		buffer_size;
	int 		sleeping;
	
	int 		time_base_den;
	int 		time_base_num;

	int 		packet_count;
	
	int 		need_key;
	int 		last_audio_time;
	
	int		apid;
	int		vpid;

	STREAM_CHUNK	sc;

	// Dolby Vision profile 7 dual-track: enhancement-layer merge state
	int		dv_el_stream;		// FFmpeg stream index of the EL track (-1: none)
	int		dv_el_merge;		// 1: merge EL packets into BL access units
	AVRational	dv_el_time_base;	// time base of the EL stream
	int		dv_el_pending_count;
	AVPacket	dv_el_pending[DV_EL_PENDING_MAX];

	// Dolby Vision tone-map mode: EL exposed as a separate packet queue
	int		dv_el_expose;		// 1: route EL packets to elq for the EL decoder
	int		elq_inited;
	AVQueue		elq;			// enhancement-layer packet queue
	int		dv_split_active;	// interleaved P7 tone-map NAL split active
	int		dv_nal_length_size;	// hvcC NAL length size (0 = Annex-B)
	unsigned char	*dv_el_hvcc;	// hvcE EL config (AV_PKT_DATA_HEVC_CONF) copy
	int		dv_el_hvcc_size;
	
} FF_PRIV;


static int ff_force_seek = 1;

DECLARE_DEBUG_PARAM( "fffs", ff_force_seek );

static int _close( STREAM *s );
static int _flush_packets( AVQueue *q, const char *tag );
static void _dv_el_flush( FF_PRIV *priv );
static void _dv_elq_flush( FF_PRIV *priv );
static int _dv_tonemap_setup_interleaved( FF_PRIV *priv );

/* libavos.c */
extern int libavos_get_dolby_vision_mode(void);

#define ff_p	((FF_PRIV*)s->parser_priv)

static struct id_fmt_str {
	int 	id;
	int 	format;
	UINT32 	fourcc;
} id_fmt[] = 
{
	// Audio
	{ AV_CODEC_ID_MP2,	WAVE_FORMAT_MPEG,		0	},
	{ AV_CODEC_ID_MP3,	WAVE_FORMAT_MPEGLAYER3,		0	},
	{ AV_CODEC_ID_WMAV2,	WAVE_FORMAT_MSAUDIO2,		0	},
	{ AV_CODEC_ID_AAC,	WAVE_FORMAT_AAC,		0	},
	{ AV_CODEC_ID_AAC_LATM,	WAVE_FORMAT_AAC_LATM,		0	},
	{ AV_CODEC_ID_AC3,	WAVE_FORMAT_AC3,		0	},
	{ AV_CODEC_ID_COOK,	WAVE_FORMAT_COOK,		0	},
	{ AV_CODEC_ID_AMR_NB,	WAVE_FORMAT_VOICEAGE_AMR,	0	},
	{ AV_CODEC_ID_AMR_WB,	WAVE_FORMAT_VOICEAGE_AMR_WB,	0	},
	{ AV_CODEC_ID_FLAC,	WAVE_FORMAT_FLAC,		0	},
	{ AV_CODEC_ID_VORBIS,	WAVE_FORMAT_OGG1,		0	},
	{ AV_CODEC_ID_OPUS,	WAVE_FORMAT_OPUS,		0	},
	{ AV_CODEC_ID_DTS,	WAVE_FORMAT_DTS,		0	},
	{ AV_CODEC_ID_COOK,	WAVE_FORMAT_COOK,		0	},
	{ AV_CODEC_ID_TRUEHD,	WAVE_FORMAT_TRUEHD,		0	},
	{ AV_CODEC_ID_EAC3,	WAVE_FORMAT_EAC3,		0	},
	{ AV_CODEC_ID_PCM_BLURAY, WAVE_FORMAT_PCM_BLURAY,	0	},
		
	// Video
	{ AV_CODEC_ID_MPEG4,	VIDEO_FORMAT_MPG4,		VIDEO_FOURCC_DX50	},	
	{ AV_CODEC_ID_MPEG2VIDEO,VIDEO_FORMAT_MPEG,		VIDEO_FOURCC_MPG2	},
	{ AV_CODEC_ID_H264,	VIDEO_FORMAT_H264,		VIDEO_FOURCC_H264	},
#ifdef CONFIG_FF_HEVC
	{ AV_CODEC_ID_HEVC,	VIDEO_FORMAT_HEVC,		VIDEO_FOURCC_HEVC	},
#endif
	{ AV_CODEC_ID_WMV3,	VIDEO_FORMAT_WMV3,		VIDEO_FOURCC_WMV3	},
	{ AV_CODEC_ID_VC1,	VIDEO_FORMAT_VC1,		VIDEO_FOURCC_WVC1	},
	{ AV_CODEC_ID_MSMPEG4V3,VIDEO_FORMAT_MSMP43,		VIDEO_FOURCC_MP43	},
	{ AV_CODEC_ID_MSMPEG4V2,VIDEO_FORMAT_MSMP42,		VIDEO_FOURCC_MP42	},
	{ AV_CODEC_ID_MSMPEG4V1,VIDEO_FORMAT_MSMP41,		VIDEO_FOURCC_MP41	},
	{ AV_CODEC_ID_MJPEG,	VIDEO_FORMAT_MJPG,		VIDEO_FOURCC_MJPG	},
	{ AV_CODEC_ID_FLV1,	VIDEO_FORMAT_SPARK,		VIDEO_FOURCC_SPARK	},
	{ AV_CODEC_ID_VP6F,	VIDEO_FORMAT_VP6,		VIDEO_FOURCC_VP6F	},
	{ AV_CODEC_ID_H263,	VIDEO_FORMAT_H263,		VIDEO_FOURCC_H263	},
	{ AV_CODEC_ID_RV10,	VIDEO_FORMAT_RV10,		VIDEO_FOURCC_RV10	},
	{ AV_CODEC_ID_RV20,	VIDEO_FORMAT_RV20,		VIDEO_FOURCC_RV20	},
	{ AV_CODEC_ID_RV30,	VIDEO_FORMAT_RV30,		VIDEO_FOURCC_RV30	},
	{ AV_CODEC_ID_RV40,	VIDEO_FORMAT_RV40,		VIDEO_FOURCC_RV40	},
	{ AV_CODEC_ID_THEORA,	VIDEO_FORMAT_THEORA,		VIDEO_FOURCC_THEO	},
	{ AV_CODEC_ID_VP8,	VIDEO_FORMAT_VP8,		VIDEO_FOURCC_VP80	},
	{ AV_CODEC_ID_VP9,	VIDEO_FORMAT_VP9,		VIDEO_FOURCC_VP90	},
	{ AV_CODEC_ID_AV1,	VIDEO_FORMAT_AV1,		VIDEO_FOURCC_AV01	},

	// Subtitle
	{ AV_CODEC_ID_DVD_SUBTITLE,SUB_FORMAT_DVD_GFX,	0 },
//	{ AV_CODEC_ID_DVB_SUBTITLE,SUB_FORMAT_DVBT,	0 },
	{ AV_CODEC_ID_TEXT,	SUB_FORMAT_TEXT,	0 },
	{ AV_CODEC_ID_BIN_DATA,	SUB_FORMAT_TEXT,	0 },
	{ AV_CODEC_ID_SUBRIP,   SUB_FORMAT_TEXT,        0 },	
	{ AV_CODEC_ID_XSUB,	SUB_FORMAT_XSUB,	0 },
	{ AV_CODEC_ID_SSA,	SUB_FORMAT_SSA,		0 },
	{ AV_CODEC_ID_ASS,	SUB_FORMAT_SSA,		0 },
	{ AV_CODEC_ID_MOV_TEXT,	SUB_FORMAT_MOV_TEXT,	0 },
	{ AV_CODEC_ID_HDMV_PGS_SUBTITLE, SUB_FORMAT_PGS,        0 },
	{ AV_CODEC_ID_WEBVTT,	SUB_FORMAT_WEBVTT,	0 },
};


static const char *disposition_name( int disposition, int is_audio )
{
	if (disposition & AV_DISPOSITION_HEARING_IMPAIRED)
		return "(hearing impaired)";
	if (disposition & AV_DISPOSITION_VISUAL_IMPAIRED)
		return is_audio ? "(audio description)" : "(visual impaired)";
	if (disposition & AV_DISPOSITION_FORCED)
		return "(forced)";
	if (disposition & AV_DISPOSITION_ORIGINAL)
		return "(original)";
	if (disposition & AV_DISPOSITION_DUB)
		return is_audio ? "(dubbed)" : "(translated)";
	if (disposition & AV_DISPOSITION_CAPTIONS)
		return "(captions)";
	if (disposition & AV_DISPOSITION_DESCRIPTIONS)
		return "(descriptions)";
	if( disposition & AV_DISPOSITION_DEFAULT )
		return "(default)";
	if (disposition & AV_DISPOSITION_COMMENT)
		return "(commentary)";
	if (disposition & AV_DISPOSITION_LYRICS)
		return "(lyrics)";
	if (disposition & AV_DISPOSITION_KARAOKE)
		return "(karaoke)";
	if( disposition & AV_DISPOSITION_ATTACHED_PIC)
		return "(attached pic)";
	if (disposition & AV_DISPOSITION_CLEAN_EFFECTS)
		return "(clean effects)";
		
	return "(none)";
}
 
void av_log_cb(void* ptr, int level, const char* fmt, va_list vl)
{
	if( log_debug && level > AV_LOG_DEBUG ) {
		return;
	} else if ( level > AV_LOG_ERROR ) {
		return;
	}
	vserprintf( fmt, vl );
}

// ************************************************************
//
//	get_ff_format
//
// ************************************************************
static int get_ff_format( int id, UINT32 *fourcc )
{
	int i;
    serprintf("get_ff_format %d\n", id);
	for( i = 0; i < sizeof( id_fmt ) / sizeof( struct id_fmt_str); i++ ) {
		if( id_fmt[i].id == id ) {
			if( fourcc ) {
				*fourcc = id_fmt[i].fourcc;
			}
			return id_fmt[i].format;
		}
	} 
	return 0;
}

// ************************************************************
//
//	_parse_dovi_conf_record
//	parse the raw dvcC/dvvC DOVIDecoderConfigurationRecord bit layout,
//	mirroring FFmpeg's ff_isom_parse_dvcc_dvvc (libavformat/dovi_isom.c).
//	used as a fallback for old MKV files that store the record in
//	CodecPrivate instead of a track BlockAdditionMapping.
// ************************************************************
static int _parse_dovi_conf_record( const uint8_t *data, int size, AVDOVIDecoderConfigurationRecord *out )
{
	uint32_t buf;

	if( !data || size < 4 || !out )
		return 0;

	memset( out, 0, sizeof( *out ) );
	out->dv_version_major = data[0];
	out->dv_version_minor = data[1];
	buf = ( data[2] << 8 ) | data[3];
	out->dv_profile       = ( buf >> 9 ) & 0x7f;
	out->dv_level         = ( buf >> 3 ) & 0x3f;
	out->rpu_present_flag = ( buf >> 2 ) & 0x01;
	out->el_present_flag  = ( buf >> 1 ) & 0x01;
	out->bl_present_flag  = buf & 0x01;
	if( size >= 5 )
		out->dv_bl_signal_compatibility_id = ( data[4] >> 4 ) & 0x0f;

	// sanity: reject anything that cannot be a dvcC/dvvC record
	if( out->dv_version_major != 1 || out->dv_profile == 0 || out->dv_profile > 20 )
		return 0;
	// a valid record always signals the RPU and at least one layer; this also
	// rejects HEVC hvcC extradata whose bytes happen to decode as plausible DV fields
	if( !out->rpu_present_flag || ( !out->el_present_flag && !out->bl_present_flag ) )
		return 0;
	return 1;
}

// ************************************************************
//
//	_pair_dovi_tracks
//	Dolby Vision profile 7 dual-track files store the base layer (BL)
//	and the enhancement layer (EL) in two separate HEVC tracks. Android
//	MediaCodec DV decoders expect a single combined bitstream, so pick
//	the BL as the video track to play, hide the EL track, and remember
//	it so that _parse_once can merge EL packets into BL access units.
//	Logic adapted from mpv's demux_mkv.c pair_dovi_tracks().
// ************************************************************
static void _pair_dovi_tracks( FF_PRIV *priv )
{
	AV_PROPERTIES *av = &priv->av;
	VIDEO_PROPERTIES *el;
	AVStream *bl_st;
	int el_idx = -1, bl_idx = -1;
	int el_stream;
	int gcd;
	int i;

	if( av->vs_max < 2 )
		return;

	for( i = 0; i < av->vs_max; i++ ) {
		VIDEO_PROPERTIES *v = &av->video[i];
		if( !v->valid )
			continue;
		if( v->format != VIDEO_FORMAT_DOLBY_VISION && v->format != VIDEO_FORMAT_HEVC )
			continue;
		if( v->dv_profile_source == 7 && v->dv_el_present ) {
			// bl_present_flag is not checked: files in the wild set it
			// to 1 even for the EL track
			if( el_idx != -1 )
				return; // ambiguous
			el_idx = i;
		} else {
			if( bl_idx != -1 )
				return; // ambiguous
			bl_idx = i;
		}
	}

	if( el_idx == -1 || bl_idx == -1 )
		return;

	el = &av->video[el_idx];
	el_stream = el->stream;

serprintf("DV: profile 7 dual-track: BL video[%d] (stream %d) + EL video[%d] (stream %d)\n",
		bl_idx, av->video[bl_idx].stream, el_idx, el_stream );

	// the player always decodes video track 0: make it the BL
	if( bl_idx != 0 ) {
		VIDEO_PROPERTIES tmp = av->video[0];
		av->video[0] = av->video[bl_idx];
		av->video[bl_idx] = tmp;
		if( el_idx == 0 )
			el_idx = bl_idx;
	}

	// hide the EL track from the track list
	for( i = el_idx; i < av->vs_max - 1; i++ )
		av->video[i] = av->video[i + 1];
	av->vs_max--;
	memset( &av->video[av->vs_max], 0, sizeof( VIDEO_PROPERTIES ) );
	av->video[av->vs_max].aspect_n = 1;
	av->video[av->vs_max].aspect_d = 1;

	priv->dv_el_stream = el_stream;
	priv->dv_el_time_base = priv->fmt->streams[el_stream]->time_base;

	// a real EL track exists in the container: flag it on the BL properties
	// so codec-level FEL gates (HW path) route dual-track P7 to the
	// software path for full reshaping/NLQ composition, exactly like a
	// single-track interleaved file whose dvcC sets el_present_flag.
	// The BL track's own dvcC often reads el_present_flag=0 in this layout.
	av->video[0].dv_el_present = 1;
	av->video[0].dv_el_dual_track = 1;

	// video timestamps must be computed with the BL stream time base
	bl_st = priv->fmt->streams[av->video[0].stream];
	gcd = av_gcd( bl_st->time_base.num, bl_st->time_base.den );
	if( gcd ) {
		priv->time_base_num = bl_st->time_base.num / gcd;
		priv->time_base_den = bl_st->time_base.den / gcd;
	}

	// EL decoder config for tone-map mode: the EL track's CodecPrivate (hvcC)
	// points into the demuxer context, same ownership model as extraData
	{
		AVCodecParameters *el_par = priv->fmt->streams[el_stream]->codecpar;
		if( el_par->extradata && el_par->extradata_size > 0 ) {
			av->video[0].dv_el_extraData     = el_par->extradata;
			av->video[0].dv_el_extraDataSize = el_par->extradata_size;
		}
	}

	// Passthrough mode: merge EL packets into BL access units for the device
	// DV decoder. Tone-map mode: expose EL packets on a separate queue; the
	// video codec runs a second decoder instance and libplacebo composites the
	// enhancement layer (mpv f_enhancement_pair style).
	if( av->video[0].format == VIDEO_FORMAT_DOLBY_VISION &&
	    libavos_get_dolby_vision_mode() != 0 ) {
		priv->dv_el_merge  = 0;
		priv->dv_el_expose = 1;
	} else {
		// only merge when the BL is actually sent to a Dolby Vision decoder;
		// otherwise the EL packets are simply dropped
		priv->dv_el_merge = ( av->video[0].format == VIDEO_FORMAT_DOLBY_VISION );
	}
}

// ************************************************************
//
//	_parse_format
//
// ************************************************************
// REMARK: cannot use scaling by audio speed there because task is performed once
static int _parse_format( int etype, FF_PRIV *priv ) 
{
	AVFormatContext *fmt = priv->fmt;

DBGP serprintf("format   [%s]\r\n", fmt->iformat->name );

	if( fmt->pb ) {
		priv->size = avio_size(fmt->pb);
DBGP serprintf("size     %lld\r\n", priv->size );
	}
	if( fmt->duration != AV_NOPTS_VALUE && etype != ETYPE_MPEG_TS ) {
		priv->duration = 1000 * (INT64)fmt->duration / AV_TIME_BASE; // rst domain
		DBGP serprintf( "duration %d\r\n", priv->duration );
	} else {
		if( priv->s )
			priv->s->no_duration = 1;
DBGP serprintf("duration ---\r\n" );
	}

	if (fmt->start_time != AV_NOPTS_VALUE) {
DBGP serprintf("FFMPEG start    %lld\r\n",  fmt->start_time);
		priv->start_time = 1000 * (INT64)fmt->start_time / AV_TIME_BASE; // rst domain
DBGP serprintf( "start    %d\r\n", priv->start_time );
	}
DBGP serprintf("bitrate  %d\r\n", fmt->bit_rate);

	int i;
	for(i = 0; i < fmt->nb_streams; i++) {
		AVStream *st          = fmt->streams[i];
		AVCodecParameters *codecpar = st->codecpar;
		int discard = 1;

		// For thumbnails: skip non-video streams early to save CPU
		if ((priv->flags & STREAM_PARSER_THUMB) &&
		    st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
			continue;
		}

DBGP serprintf("Stream #%d: ", i);
		if(st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO){
DBGP serprintf("VIDEO\r\n");
		} else if( st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ){
DBGP serprintf("AUDIO\r\n");
		} else if( st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE ){
DBGP serprintf("SUBTITLE\r\n");
		} else if( st->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT ){
DBGP serprintf("ATTACHEMENT\r\n");
		} else if( st->codecpar->codec_type == AVMEDIA_TYPE_DATA ){
DBGP serprintf("DATA\r\n");
		} else {
DBGP serprintf("<unknown> type %d\r\n", st->codecpar->codec_type);
		}
		int flags = fmt->iformat->flags;
		if (flags & AVFMT_SHOW_IDS) {
DBGP serprintf("\tPID        0x%x\r\n", st->id);
		}
		AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
		if (lang) {
DBGP serprintf("\tlanguage   %s -> %s\r\n", lang->value, map_ISO639_code( lang->value ) );
		}
		AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
		if (title) {
DBGP serprintf("\ttitle   %s\r\n", title->value);
		}
		int gcd = av_gcd(st->time_base.num, st->time_base.den);
DBGP serprintf("\tnum/dem    %d/%d\r\n", st->time_base.num/gcd, st->time_base.den/gcd);
DBGP serprintf("\tcodec_id   %X\r\n", codecpar->codec_id);
		const AVCodecDescriptor *desc = avcodec_descriptor_get(codecpar->codec_id);
DBGP serprintf("\tcodec_name %s\r\n", desc ? desc->name : "");
DBGP serprintf("\tcodec_long_name %s\r\n", desc ? desc->long_name : "");
		if( codecpar->extradata_size ) {
DBGP serprintf("\textra      "); 
DBGP DumpLine( codecpar->extradata, MIN(128,codecpar->extradata_size), MIN(128,codecpar->extradata_size) );
		}
DBGP serprintf("\tbitrate    %d\r\n", codecpar->bit_rate);
DBGP serprintf("\tdisposition %d / %s\r\n", st->disposition, disposition_name(st->disposition, st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO));
		
		if(st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO){
			//
			// video
			//
			if( st->disposition & AV_DISPOSITION_ATTACHED_PIC ) {
				goto DISCARD_STREAM;
			}
			if(st->avg_frame_rate.den && st->avg_frame_rate.num) {
DBGP serprintf("\tfps        %5.2f fps(r)\r\n", av_q2d(st->avg_frame_rate));
			}
DBGP serprintf("\tPAR        %d/%d\r\n", codecpar->sample_aspect_ratio.num, codecpar->sample_aspect_ratio.den ); 
			if ( priv->av.vs_max < VIDEO_TRACK_MAX ) {
				VIDEO_PROPERTIES *video = priv->av.video + priv->av.vs_max;
				
				video->stream = i;
                if (st->avg_frame_rate.den && st->r_frame_rate.den && av_q2d(st->avg_frame_rate) == av_q2d(st->r_frame_rate)) {
                    video->frame_rate_den = st->r_frame_rate.den;
                    video->frame_rate_num = st->r_frame_rate.num;
                }

				if(st->avg_frame_rate.den && st->avg_frame_rate.num) {
					video->rate  = st->avg_frame_rate.num;
					video->scale = st->avg_frame_rate.den;
DBGP serprintf( "vrate=%d; vscale=%d\n", video->rate, video->scale );
				} else {
					//video->scale  = st->time_base.num;
					//video->rate   = st->time_base.den;
serprintf( "untouched (!?) vrate=%d; vscale=%d\n", video->rate, video->scale );
				}

				priv->time_base_num = st->time_base.num/gcd;
				priv->time_base_den = st->time_base.den/gcd;
				video->frames = 0;
				video->valid  = 1;
				
				//if( priv->av.vs_max == 0 && video->rate )
				//	priv->duration = (UINT32)( 1000ull * (UINT64)video->frames * (UINT64) video->scale / (UINT64) video->rate);

				video->codec_id	   = codecpar->codec_id;
				strnZcpy( video->codec_name, desc ? desc->name : "", AV_NAME_LEN );
				
				video->fourcc      = codecpar->codec_tag;
				video->format      = get_ff_format( codecpar->codec_id, &video->fourcc  );
				if( video->format == 0 && video->codec_id ) {
					video->format = VIDEO_FORMAT_LAVC;
					video->fourcc = VIDEO_FOURCC_LAVC;
				}
				
				if( codecpar->extradata_size ) {
					// libavformat used to skip the first 4 bytes in av1 private data but not anymore
					// for AV1 both sfdec android hw codecs and dav1d do not want this thus skip it
					int offset = ( video->format == VIDEO_FORMAT_AV1 ) ? 4 : 0;
					if( codecpar->extradata_size <= sizeof( video->extraData ) ) {
						// Add debug output to investigate the extradata
						DBGP {
							serprintf( "AV1 extraData[%d]=[", codecpar->extradata_size );
							if( codecpar->extradata_size >= 8 ) {
								serprintf( "4 first bytes: " );
								for( int i = 0; i < 4; i++ ) {
									serprintf( "%02X,", codecpar->extradata[i] );
								}
								serprintf( "]\n" );
							}
						}
						video->extraDataSize = codecpar->extradata_size - offset ;
						memcpy( video->extraData, codecpar->extradata + offset , video->extraDataSize );
						if( video->format == VIDEO_FORMAT_H264 && video->extraData[0] == 0x00 ) {
serprintf("FF: parse H264 SPS\n");
							// for non-AVCC H264, parse the SPS/PPS here
							H264_get_video_props( video, video->extraData, video->extraDataSize, &video->sps );
						}
					} else {
						video->extraDataSize  = 0;
						video->extraDataSize2 = codecpar->extradata_size - offset;
						video->extraData2     = codecpar->extradata + offset;
					}
				} 
				
				video->width       = codecpar->width;
				video->height      = codecpar->height;
				video->aspect_n    = codecpar->sample_aspect_ratio.num;
				video->aspect_d	   = codecpar->sample_aspect_ratio.den;
				video->bytesPerSec = codecpar->bit_rate / 8;

				video->color_primaries = codecpar->color_primaries;
				video->color_trc       = codecpar->color_trc;
				video->color_space     = codecpar->color_space;
				video->color_range     = codecpar->color_range;

				switch( video->format ) {
				case VIDEO_FORMAT_MPEG:
					video->reorder_pts   = 0;
					video->extraDataSize = 0;
					break;
				default:
					video->reorder_pts = 1;
					break;
				}
				if( !strcmp("avi", fmt->iformat->name ) ) {
					video->reorder_pts = 0;
				}
				if( force_reorder != -1 ) {
					video->reorder_pts = force_reorder;
				}
				
				priv->av.vs_max ++;
				discard = 0;

                        int side_data_size = 0;
                        uint8_t* side_data = NULL;
                        AVDOVIDecoderConfigurationRecord dovi_rec;
                        AVDOVIDecoderConfigurationRecord *dovi_record = NULL;

                        // FFmpeg 8+ replacement for deprecated av_stream_get_side_data
                        for (int j = 0; j < codecpar->nb_coded_side_data; j++) {
                            if (codecpar->coded_side_data[j].type == AV_PKT_DATA_DOVI_CONF) {
                                side_data = codecpar->coded_side_data[j].data;
                                side_data_size = codecpar->coded_side_data[j].size;
                                break;
                            }
                        }
                        if (side_data && side_data_size > 0) {
                            dovi_record = (AVDOVIDecoderConfigurationRecord*)side_data;
                        } else if (video->format == VIDEO_FORMAT_HEVC &&
                                   video->extraDataSize >= 5 &&
                                   video->extraData[0] == 1 && video->extraData[1] == 0 &&
                                   _parse_dovi_conf_record(video->extraData, video->extraDataSize, &dovi_rec)) {
                            // old MKV muxings store the dvcC/dvvC record in CodecPrivate,
                            // which FFmpeg's matroska demuxer does not parse: fall back to
                            // parsing it here. extraData is kept as-is and still sent as CSD.
                            dovi_record = &dovi_rec;
                            serprintf("DV: found dvcC/dvvC record in extradata (legacy CodecPrivate)\n");
                        }
#ifdef CONFIG_ANDROID
                        // Tone-map mode renders DV through libplacebo and does not
                        // need a device DV decoder, so detect DV from the container
                        // metadata regardless of "video/dolby-vision" HW support.
                        if(dovi_record && (libavos_get_dolby_vision_mode() != 0 || acodecs_is_type_supported("video/dolby-vision", 0))) {
#else
                        if(dovi_record) {
#endif
                            if (video->format == VIDEO_FORMAT_HEVC) {
                                switch(dovi_record->dv_profile) {
                                    // Mapping source: Kodi's DVDVideoCodecAndroidMediaCodec.cpp
                                    case 4:
                                        video->dv_profile = 16; //DolbyVisionProfileDvheDtr
                                        break;
                                    case 5:
                                        video->dv_profile = 32; //DolbyVisionProfileDvheStn 
                                        break;
                                    case 7:
                                        video->dv_profile = 256; //DolbyVisionProfileDvheSt
                                        break;
                                    case 8:
                                        video->dv_profile = 256; //DolbyVisionProfileDvheSt, should be Dtb but Kodi says to use St
                                        break;
                                    case 9:
                                        video->dv_profile = 512; //DolbyVisionProfileDvavSe
                                        break;
                                    default:
                                        serprintf("Unsupported Dolby HEVC profile %d", dovi_record->dv_profile);
                                        break;
                                }
                            } else if (video->format == VIDEO_FORMAT_AV1) {
                                if (dovi_record->dv_profile == 10) {
                                    video->dv_profile = 0x400;//DolbyVisionProfileDvav110 
                                } else {
                                    serprintf("Unsupported Dolby AV1 profile %d", dovi_record->dv_profile);
                                }
                            } else {
                                serprintf("Dolby Vision in an unknown codec %d", video->format);
                            }

                            video->dv_profile_source = dovi_record->dv_profile;
                            video->dv_level      = dovi_record->dv_level;
                            video->dv_el_present = dovi_record->el_present_flag;
                            video->dv_bl_present = dovi_record->bl_present_flag;
                            video->dv_compat_id  = dovi_record->dv_bl_signal_compatibility_id;

                            video->fourcc = VIDEO_FOURCC_DOLBY_VISION;
                            video->format = VIDEO_FORMAT_DOLBY_VISION;

                            serprintf("HELLO, This is a dolby vision content! profile %d level %d el %d bl %d compat %d\r\n",
                                      video->dv_profile_source, video->dv_level, video->dv_el_present,
                                      video->dv_bl_present, video->dv_compat_id);
                        }
			}
		} else if( st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ){
			//
			// audio
			//
DBGP serprintf("\tsampleRate %d\r\n", codecpar->sample_rate);
DBGP serprintf("\tblockAlign %d\r\n", codecpar->block_align);
DBGP serprintf("\tchannels   %d\r\n", codecpar->ch_layout.nb_channels);

			if(st->avg_frame_rate.den && st->avg_frame_rate.num) {
DBGP serprintf("\tfps        %5.2f fps(r)\r\n", av_q2d(st->avg_frame_rate));
			}

			if ( priv->av.as_max < AUDIO_TRACK_MAX ) {	
				AUDIO_PROPERTIES *audio = priv->av.audio + priv->av.as_max;

				audio->codec_id	     = codecpar->codec_id;
				strnZcpy( audio->codec_name, desc ? desc->name : "", AV_NAME_LEN );
				audio->format        = get_ff_format( codecpar->codec_id, NULL );
				if( audio->format == WAVE_FORMAT_EAC3 &&
				    codecpar->profile == AV_PROFILE_EAC3_DDP_ATMOS ) {
					audio->format = WAVE_FORMAT_E_AC3_JOC;
					DBG serprintf("stream_parser_ffmpeg: detected EAC3 Atmos (profile=%d)\n",
						codecpar->profile);
				}

				if( audio->format == 0 && audio->codec_id ) {
					audio->format = WAVE_FORMAT_LAVC;
				}

				audio->stream        = i;
				audio->scale         = st->time_base.num/gcd;
				audio->rate          = st->time_base.den/gcd;
DBGP serprintf( "arate=%d; ascale=%d\n", audio->rate, audio->scale );
				audio->frames        = 0;
				audio->channels      = codecpar->ch_layout.nb_channels;
				audio->samplesPerSec = codecpar->sample_rate;
				audio->bitsPerSample = 0;
				audio->blockAlign    = codecpar->block_align;
				audio->bytesPerSec   = codecpar->bit_rate / 8;
				audio->valid         = 1;
				//stream_set_audio_name( audio, priv->av.as_max + 1 );

				if (title) {
					int n = snprintf(audio->name, AV_NAME_LEN, "%s", title->value);
					if (n >= AV_NAME_LEN) audio->name[AV_NAME_LEN - 1] = '\0';
				}

				if (lang) {
					strnZcpy( audio->lang, lang->value, AV_NAME_LEN );
				}

				audio->disposition = st->disposition;

				if (st->disposition && st->disposition != AV_DISPOSITION_DEFAULT) {
					if (st->disposition & (AV_DISPOSITION_HEARING_IMPAIRED | AV_DISPOSITION_VISUAL_IMPAIRED)) {
						audio->priority = 2;
					}
				}
				
				if ( audio->format == WAVE_FORMAT_IMA ) {
					audio->samplesPerBlock = 0;
				}

				if( codecpar->extradata_size ) {
					if( codecpar->extradata_size <= sizeof( audio->extraData ) ) {
						audio->extraDataSize = codecpar->extradata_size;
						memcpy( audio->extraData, codecpar->extradata, audio->extraDataSize  );	
					} else {
						audio->extraDataSize  = 0;
						audio->extraDataSize2 = codecpar->extradata_size;
						audio->extraData2     = codecpar->extradata;
					}
				} 
				
				// hack for stupid canon cameras!
				if ( audio->samplesPerSec == 11024 )
					audio->samplesPerSec ++;
				
				//_check_VBR( audio );
				
				priv->av.as_max ++;
				discard = 0;
			} 
		} else if( st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE || st->codecpar->codec_type == AVMEDIA_TYPE_DATA ){
			//
			// subtitle
			//
			int fmt = get_ff_format( codecpar->codec_id, NULL );
			if( fmt && priv->av.subs_max < SUB_TRACK_MAX ) {
				SUB_PROPERTIES *sub = priv->av.sub + priv->av.subs_max;
	
				sub->valid          = 1;
				sub->codec_id	    = codecpar->codec_id;
				strnZcpy( sub->codec_name, desc ? desc->name : "", AV_NAME_LEN );
				serprintf("sub->codec_name %s\n", sub->codec_name);
				sub->format         = fmt;
				sub->gfx            = (sub->format == SUB_FORMAT_DVD_GFX || sub->format == SUB_FORMAT_PGS) ? 1 : 0;
				sub->stream         = i;
				sub->scale          = st->time_base.num;
				sub->rate           = st->time_base.den;

DBGP serprintf("srate=%d; sscale=%d\n", sub->rate, sub->scale);
				sub->extraData2     = codecpar->extradata;
				sub->extraDataSize2 = codecpar->extradata_size;

				if (title) {
					int n = snprintf(sub->name, AV_NAME_LEN, "%s", title->value);
					if (n >= AV_NAME_LEN) sub->name[AV_NAME_LEN - 1] = '\0';
				}

				if (lang) {
					strnZcpy( sub->lang, lang->value, AV_NAME_LEN );
				}

				sub->disposition = st->disposition;

				if (st->disposition && st->disposition != AV_DISPOSITION_DEFAULT) {
					if (st->disposition & (AV_DISPOSITION_HEARING_IMPAIRED | AV_DISPOSITION_VISUAL_IMPAIRED)) {
						sub->priority = 2;
					}
				}

				priv->av.subs_max ++;
				discard = 0;
			}
		}
DISCARD_STREAM:
		if( discard ) {
DBGP serprintf("\tDISCARD!\n" );
			st->discard = AVDISCARD_ALL;
		}		
DBGP serprintf("\r\n");
	}

	// Dolby Vision profile 7 dual-track: pair BL + EL tracks so that the
	// EL is hidden and its packets get merged into the BL access units
	_pair_dovi_tracks( priv );

	// Dolby Vision profile 7 single-track interleaved in tone-map mode:
	// split the combined stream into BL(+RPU) and EL via dovi_split BSFs
	_dv_tonemap_setup_interleaved( priv );

	if( fmt->nb_chapters ) {
DBGP serprintf("chapters:\r\n");	
		for( i =0; i < fmt->nb_chapters; i++ ) {
			// chapters stays in rst domain
			AVChapter *ch = fmt->chapters[i];
			UINT64 start = 1000 * ch->start * ch->time_base.num / ch->time_base.den; 
			UINT64 end   = 1000 * ch->end   * ch->time_base.num / ch->time_base.den; 
			AVDictionaryEntry *t = av_dict_get( ch->metadata, "title", NULL, 0 );
			DBGP serprintf( "[%2d] id %08X  start/end %8lld/%8lld  [%s]\r\n", i, ch->id, start, end,
							t ? t->value : "(no title)" );
			if( priv->s ) {
				stream_add_chapter( priv->s, start, end, t ? t->value : "s_unknown" );
			}
		}
DBGP serprintf("\r\n");
	}

	return 0;
}

static int ffmpeg_interrupt_cb(void *ctx)
{
	STREAM *s = (STREAM*)ctx;
	return s && stream_abort( s ) ? 1 : 0;
}

static void parse_PID_from_query( STREAM *s )
{
	int pid;
	char *vid = strstr( s->src_query, "vid=" );
	if( vid && sscanf( vid, "vid=%d", &pid ) == 1 ) {
		ff_p->vpid = pid;
DBGP serprintf("video PID    %4d\n", ff_p->vpid);
	}
	
	char *aud = strstr( s->src_query, "aud=" );
	if( aud && sscanf( aud, "aud=%d", &pid ) == 1 ) {
		ff_p->apid = pid;
DBGP serprintf("audio PID    %4d\n", ff_p->apid);
	}
}

// ************************************************************
//
//	_open
//
// ************************************************************
static int _open( STREAM *s, int buffer_size, int flags )
{
DBGS serprintf("FFMPEG: open: %s, buffer_size: %d\r\n", s->src.url, buffer_size);

	// allocate private data
	if( !(s->parser_priv = (FF_PRIV*)amalloc( sizeof( FF_PRIV ) ) ) ) {
		goto ErrorExit;
	}
	
	memset( ff_p, 0, sizeof( FF_PRIV ) );
	ff_p->dv_el_stream = -1;
	av_init_props( ff_p );
	ff_p->s = s;
	
	ff_p->flags = flags;
	
	stream_parser_clear_chunks( s );

		ff_p->buffer_size = buffer_size;

	av_log_set_callback(av_log_cb);
	if( log_debug ) {
		av_log_set_level( AV_LOG_DEBUG );
 	}
	
	if (avformat_network_init() != 0) {
serprintf("FFMPEG: cannot init network");
		goto ErrorExit2;
    	}
	
	ff_p->fmt = avformat_alloc_context();

	// set max_delay here, we need that for proper RTSP, all other demuxers ignore it ...
	ff_p->fmt->max_delay = max_delay;
DBGP serprintf("max_delay: %d\n", ff_p->fmt->max_delay);
	
	ff_p->fmt->interrupt_callback.callback = ffmpeg_interrupt_cb;
	ff_p->fmt->interrupt_callback.opaque   = s;

	if( strstr( s->src_query, "?mpegts&" ) ) {
		parse_PID_from_query( s );
	}
	
	if( force_vpid ) {
		ff_p->vpid = force_vpid;
	}
	if( force_apid ) {
		ff_p->apid = force_apid;
	}
		
	if( ff_p->vpid || ff_p->apid ) {
		char buf[32];
		av_dict_set(&ff_p->fmt_opts, "no_pat", "1", 0);
        	
		if( ff_p->vpid ) {
			snprintf(buf, sizeof(buf), "%d", ff_p->vpid);
			av_dict_set(&ff_p->fmt_opts, "vpid", buf, 0);
		}
		if( ff_p->apid ) {
			snprintf(buf, sizeof(buf), "%d", ff_p->apid);
			av_dict_set(&ff_p->fmt_opts, "apid", buf, 0);
			if(!ff_p->vpid) {
				// audio only, lower score for MP3
				char buf[10] = "50";
				av_dict_set(&ff_p->fmt_opts, "probe_extra", buf, 0);
			}
		}
		ff_p->fmt->flags |= AVFMT_FLAG_NOFILLIN;
	}

	// For thumbnails: use minimal probing to speed up processing
	if (ff_p->flags & STREAM_PARSER_THUMB) {
		av_dict_set(&ff_p->fmt_opts, "probesize", "500000", 0);      // 500KB instead of 10MB
		av_dict_set(&ff_p->fmt_opts, "analyzeduration", "1000000", 0);  // 1 second max
	} else {
		av_dict_set(&ff_p->fmt_opts, "probesize", "10000000", 0);
	}

	// Set user agent for HTTP streams to improve compatibility with CDN/debrid services
	av_dict_set(&ff_p->fmt_opts, "user_agent", "Mozilla/5.0 (Linux; Android) Nova/1.0", 0);
DBGP serprintf("FFMPEG: opening url [%s]\r\n", s->src.url);

	if( avformat_open_input(&ff_p->fmt, s->src.url, NULL, &ff_p->fmt_opts ) != 0) {
serprintf("FFMPEG: cannot open file [%s]\r\n", s->src.url);
		goto ErrorExit4;
	}

DBGP serprintf("info\r\n");

	// Retrieve stream information
	if (ff_p->flags & STREAM_PARSER_THUMB) {
		// For thumbnails: only analyze video stream, skip audio/subs for speed
		int nb_streams = ff_p->fmt->nb_streams;
		AVDictionary **opts = (AVDictionary **)acalloc(nb_streams, sizeof(AVDictionary *));
		if (opts) {
			for (int i = 0; i < nb_streams; i++) {
				if (ff_p->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
					// Analyze video stream with minimal time
					av_dict_set(&opts[i], "analyzeduration", "1000000", 0);  // 1 second max
				} else {
					// Skip audio/subtitle analysis completely
					av_dict_set(&opts[i], "analyzeduration", "0", 0);
					ff_p->fmt->streams[i]->discard = AVDISCARD_ALL;
				}
			}
		}
		if (avformat_find_stream_info(ff_p->fmt, opts) < 0) {
			printf("FFMPEG: cannot find stream info\r\n");
		}
		// Clean up
		if (opts) {
			for (int i = 0; i < nb_streams; i++) {
				av_dict_free(&opts[i]);
			}
			afree(opts);
		}
	} else {
		// Normal mode: analyze all streams
		if (avformat_find_stream_info(ff_p->fmt, NULL) < 0) {
			printf("FFMPEG: cannot find stream info\r\n");
		}
	}

	_parse_format( s->etype, ff_p );

	memcpy( &s->av, &ff_p->av, sizeof( AV_PROPERTIES ) );

	s->duration = ff_p->duration;
	s->size     = ff_p->size;
	
	LinkedList_init( &ff_p->aq.list );
	LinkedList_init( &ff_p->vq.list );
	LinkedList_init( &ff_p->sq.list );
	LinkedList_init( &ff_p->elq.list );
	ff_p->elq_inited = 1;

	pthread_mutex_init( &ff_p->aq.mutex, NULL );
	pthread_mutex_init( &ff_p->vq.mutex, NULL );
	pthread_mutex_init( &ff_p->sq.mutex, NULL );
	pthread_mutex_init( &ff_p->elq.mutex, NULL );

	// make lavf parser use this sync mode! 0 is for STREAM_SYNC_CDATA (PTS) and 1 for STREAM_SYNC_SAMPLES
	//s->sync_mode = STREAM_SYNC_SAMPLES;
	//s->sync_mode = STREAM_SYNC_CDATA; // current default one
	s->sync_mode = stream_parser_get_sync_mode();
	
	// Force sample-based sync for FLAC audio tracks to avoid sync issues
	if (s->audio->valid && s->audio->format == WAVE_FORMAT_FLAC) {
		s->sync_mode = STREAM_SYNC_SAMPLES;
	}

	s->parser_open = 1;

	if( s->video->valid ) {
		ff_p->need_key = 1;
	}
	return 0;

ErrorExit4:
ErrorExit3:
	av_dict_free(&ff_p->fmt_opts);
	avformat_network_deinit();

ErrorExit2:
ErrorExit:
	afree( ff_p );
	s->parser_priv = NULL;
	
	return 1;
}

// ************************************************************
//
//	_close
//
// ************************************************************
static int _close( STREAM *s )
{
DBGS serprintf("FFMPEG: close\r\n");
	if( !s->parser_open ) {
serprintf("FFMPEG: not open!\r\n" );
		return 1;
	} 
	s->parser_open = 0;
	if( ff_p ) {
		if( ff_p->fmt ) {
			// Close the video file
			avformat_close_input(&ff_p->fmt);
		}


		_flush_packets( &ff_p->vq, "VID" );
		_flush_packets( &ff_p->aq, "AUD" );
		_flush_packets( &ff_p->sq, "SUB" );
		_dv_el_flush( ff_p );
		_dv_elq_flush( ff_p );
		av_freep( &ff_p->dv_el_hvcc );

		av_dict_free(&ff_p->fmt_opts);

		afree( ff_p );
		s->parser_priv = NULL;
	}
	avformat_network_deinit();
	return 0;
}

// ************************************************************
//
//	_dispose_packet
//
// ************************************************************
static void _dispose_packet( AVPacket *packet )
{
	av_packet_unref( packet );
}

// ************************************************************
//
//	_add_packet
//
// ************************************************************
static int _add_packet( AVQueue *q, AVPacket *packet )
{
	pthread_mutex_lock( &q->mutex );

	PacketNode *node = acalloc( 1, sizeof( PacketNode ) );
	LinkedListNode_init( (LinkedListNode*)node);

	av_packet_ref(&node->packet, packet);

	LinkedList_append( &q->list, (LinkedListNode*) node);
	
	q->mem_used += sizeof( PacketNode ) + node->packet.size;
	q->packets  ++;
	pthread_mutex_unlock( &q->mutex );
	return 0;
}

// ************************************************************
//
//	_get_packet
//
// ************************************************************
static AVPacket *_get_packet( AVQueue *q, AVPacket *packet )
{
	pthread_mutex_lock( &q->mutex );
	PacketNode *node = (PacketNode*)q->list.first;
	if( !node ) {
		pthread_mutex_unlock( &q->mutex );
		return NULL;
	}	

	LinkedList_remove( &q->list, (LinkedListNode*)node );

	*packet = node->packet;
	afree( node );
	
	q->mem_used -= sizeof( PacketNode ) + packet->size;
	q->packets  --;
	
	pthread_mutex_unlock( &q->mutex );
	return packet;
} 

// ************************************************************
//
//	_peek_packet
//
// ************************************************************
static AVPacket *_peek_packet( AVQueue *q, AVPacket *packet, int at )
{
	pthread_mutex_lock( &q->mutex );
	PacketNode *node = (PacketNode*)LinkedList_entryAt( &q->list, at );
	if( !node ) {
		pthread_mutex_unlock( &q->mutex );
		return NULL;
	}	

	*packet = node->packet;
	
	pthread_mutex_unlock( &q->mutex );
	return packet;
} 

// ************************************************************
//
//	_flush_packets
//
// ************************************************************
static int _flush_packets( AVQueue *q, const char *tag )
{
DBGP serprintf("flush_packets[%s] [%4d|%8d]->", tag, q->packets, q->mem_used );
	while( 1 ) {
		AVPacket _packet;
		AVPacket *packet = _get_packet( q, &_packet );
		if( !packet ) {
			break;
		}

		_dispose_packet( &_packet );
	}
DBGP serprintf("[%4d|%8d]\r\n", q->packets, q->mem_used );
	return 0;
}

extern int stream_drive_wake_sleep;

// audio_speed > 1 means that parsers are outputting audio/video quicker with smaller time units yielding smaller timestamps
// this means that real stream time = timestamps * audio_speed or rst = ts * as
// conversely timestamp = real stream time / audio_speed or ts = rst / as
// ts = rst/as: i.e. ts<rst when as>1

#define GET_AUDIO_TS( ts ) ( ts == AV_NOPTS_VALUE ? STREAM_NO_PTS_VALUE : (INT64)ts * 1000 * (INT64)s->audio->scale / s->audio->rate )
#define GET_VIDEO_TS( ts ) ( ts == AV_NOPTS_VALUE ? -1 : (INT64)ts * 1000 * (INT64)ff_p->time_base_num / ff_p->time_base_den )
#define GET_SUB_TS( ts )   ( ts == AV_NOPTS_VALUE ? -1 : (INT64)ts * 1000 * (INT64)s->subtitle->scale / s->subtitle->rate )

// ************************************************************
//
//	_get_video_time
//	returns the video timestamp in milliseconds in ts domain
//
// ************************************************************
static int _get_video_time( STREAM *s, AVPacket *packet )
{
	int t = ( use_pts && packet->pts != AV_NOPTS_VALUE ) ? GET_VIDEO_TS( packet->pts ) : GET_VIDEO_TS( packet->dts );
	if (t == -1) return -1;
	t -= ff_p->start_time;
	return RST_TO_TS_TIME(t, int);
}

// Convert an enhancement-layer packet's timestamps into the same avos time
// domain as the BL cdata->time (_get_video_time: ms offset by start_time), so
// decoded EL frame pts matches BL vframe->pts for pairing in codec_ffmpeg_video.c
static void _dv_el_convert_time( FF_PRIV *priv, AVPacket *pkt, int tb_num, int tb_den )
{
	int64_t t;
	if( !tb_den )
		return;
	if( pkt->pts != AV_NOPTS_VALUE ) {
		t = (int64_t)pkt->pts * 1000 * tb_num / tb_den - priv->start_time;
		pkt->pts = RST_TO_TS_TIME( t, int );
	}
	if( pkt->dts != AV_NOPTS_VALUE ) {
		t = (int64_t)pkt->dts * 1000 * tb_num / tb_den - priv->start_time;
		pkt->dts = RST_TO_TS_TIME( t, int );
	}
}

// ************************************************************
//
//	_get_audio_time
//	returns the audio timestamp in milliseconds in ts domain
//
// ************************************************************
static int _get_audio_time( STREAM *s, AVPacket *packet )
{
	int t = GET_AUDIO_TS( packet->pts );
	if (t == STREAM_NO_PTS_VALUE) return STREAM_NO_PTS_VALUE;
	t -= ff_p->start_time;
	return RST_TO_TS_TIME(t, int);
}

// ************************************************************
//
//     _get_subtitle_time
//	returns the subtitle timestamp in milliseconds in ts domain
//
// ************************************************************
// _get_subtitle_time returns ts = rst / as
static int _get_subtitle_time( STREAM *s, AVPacket *packet )
{
	int t = GET_SUB_TS( packet->pts );
	if (t == -1) return -1;
	t -= ff_p->start_time;
	return RST_TO_TS_TIME(t, int);
}

// ************************************************************
//
//	Dolby Vision profile 7 pending enhancement-layer packets
//
// ************************************************************
static void _dv_el_flush( FF_PRIV *priv )
{
	int i;
	for( i = 0; i < priv->dv_el_pending_count; i++ )
		av_packet_unref( &priv->dv_el_pending[i] );
	priv->dv_el_pending_count = 0;
}

static void _dv_el_push( FF_PRIV *priv, AVPacket *packet )
{
	if( priv->dv_el_pending_count >= DV_EL_PENDING_MAX ) {
		// window overflow: drop the oldest EL packet
		av_packet_unref( &priv->dv_el_pending[0] );
		memmove( &priv->dv_el_pending[0], &priv->dv_el_pending[1],
		         ( DV_EL_PENDING_MAX - 1 ) * sizeof( AVPacket ) );
		priv->dv_el_pending_count--;
		memset( &priv->dv_el_pending[priv->dv_el_pending_count], 0, sizeof( AVPacket ) );
	}
	if( av_packet_ref( &priv->dv_el_pending[priv->dv_el_pending_count], packet ) < 0 )
		return; // OOM: leave the blank slot uncounted, drop this EL packet
	priv->dv_el_pending_count++;
}

// ************************************************************
//
//	Dolby Vision tone-map mode: EL packet queue + dovi_split BSFs
//
// ************************************************************
static void _dv_elq_flush( FF_PRIV *priv )
{
	AVPacket pkt;
	if( !priv->elq_inited )
		return;
	while( _get_packet( &priv->elq, &pkt ) )
		av_packet_unref( &pkt );
}

// Standalone port of the core NAL filtering from FFmpeg's
// libavcodec/bsf/dovi_split.c (that BSF does not exist in Nova's FFmpeg 8.0.1).
// Splits an interleaved profile 7 access unit into BL(+RPU) and EL streams:
//   - NAL type 63 (HEVC_NAL_UNSPEC63): enhancement layer, outer two-byte NAL
//     header stripped on output (the EL is a self-contained HEVC stream)
//   - NAL type 62 (HEVC_NAL_UNSPEC62): RPU metadata, kept with the BL
//   - everything else: base layer
// NAL walking itself lives in the shared dovi_nal helpers.

// build one output stream (el_mode 0: BL+RPU, 1: EL) from an interleaved AU.
// Returns 0 on success (*out_buf NULL when nothing was kept).
static int _dv_split_build( const uint8_t *data, int size, int lsize, int el_mode,
                            uint8_t **out_buf, int *out_size )
{
	int prefix = lsize ? lsize : 4;
	size_t total = 0;
	int kept = 0, pass, pos, nal_size, i;
	const uint8_t *nal;
	uint8_t *buf = NULL, *dst;

	for( pass = 0; pass < 2; pass++ ) {
		pos = 0;
		dst = buf;
		while( ( nal = dovi_next_nal( data, size, lsize, &pos, &nal_size ) ) != NULL ) {
			int type = ( nal[0] >> 1 ) & 0x3F;
			const uint8_t *payload;
			int psize;
			if( type == DOVI_NAL_TYPE_EL ) {
				if( !el_mode || nal_size <= 2 )
					continue;
				payload = nal + 2;	// strip outer EL NAL header
				psize = nal_size - 2;
			} else if( type == DOVI_NAL_TYPE_RPU ) {
				if( el_mode )
					continue;
				payload = nal;
				psize = nal_size;
			} else {
				if( el_mode )
					continue;
				payload = nal;
				psize = nal_size;
			}
			if( pass == 0 ) {
				total += prefix + psize;
				kept++;
			} else {
				if( lsize ) {
					for( i = lsize - 1; i >= 0; i-- )
						*dst++ = ( psize >> ( 8 * i ) ) & 0xFF;
				} else {
					*dst++ = 0; *dst++ = 0; *dst++ = 0; *dst++ = 1;
				}
				memcpy( dst, payload, psize );
				dst += psize;
			}
		}
		if( pass == 0 ) {
			if( !kept ) {
				*out_buf = NULL;
				*out_size = 0;
				return 0;
			}
			buf = (uint8_t *) av_malloc( total + AV_INPUT_BUFFER_PADDING_SIZE );
			if( !buf )
				return 1;
		}
	}
	memset( dst, 0, AV_INPUT_BUFFER_PADDING_SIZE );
	*out_buf = buf;
	*out_size = (int) total;
	return 0;
}

// wrap a freshly built buffer into an AVPacket inheriting in's timestamps
static int _dv_make_packet( AVPacket *in, AVPacket *out, uint8_t *buf, int size )
{
	AVBufferRef *bref = av_buffer_create( buf, size + AV_INPUT_BUFFER_PADDING_SIZE, NULL, NULL, 0 );
	if( !bref ) {
		av_free( buf );
		return 1;
	}
	if( av_packet_copy_props( out, in ) < 0 ) {
		av_buffer_unref( &bref );
		return 1;
	}
	out->buf  = bref;
	out->data = buf;
	out->size = size;
	return 0;
}

// split one interleaved P7 access unit into BL(+RPU) and EL packets
static int _dv_split_packet( FF_PRIV *priv, AVPacket *in, AVPacket *bl_out, AVPacket *el_out )
{
	uint8_t *bl_buf = NULL, *el_buf = NULL;
	int bl_size = 0, el_size = 0;

	if( _dv_split_build( in->data, in->size, priv->dv_nal_length_size, 0, &bl_buf, &bl_size ) )
		return 1;
	if( _dv_split_build( in->data, in->size, priv->dv_nal_length_size, 1, &el_buf, &el_size ) ) {
		av_free( bl_buf );
		return 1;
	}
	if( bl_buf && _dv_make_packet( in, bl_out, bl_buf, bl_size ) )
		bl_buf = NULL;	// consumed/failed inside
	if( el_buf && _dv_make_packet( in, el_out, el_buf, el_size ) )
		el_buf = NULL;
	return 0;
}

// Single-track interleaved profile 7 in tone-map mode: activate the NAL split so
// the codec receives BL(+RPU) packets on the normal video path and EL packets
// via the EL queue, exactly like the dual-track layout.
static int _dv_tonemap_setup_interleaved( FF_PRIV *priv )
{
	VIDEO_PROPERTIES *v = &priv->av.video[0];
	AVStream *st;

	if( priv->dv_el_stream >= 0 )
		return 0;			// dual-track handled elsewhere
	if( v->format != VIDEO_FORMAT_DOLBY_VISION )
		return 0;
	if( v->dv_profile_source != 7 || !v->dv_el_present )
		return 0;
	if( libavos_get_dolby_vision_mode() == 0 )
		return 0;			// passthrough keeps the combined stream

	st = priv->fmt->streams[v->stream];
	priv->dv_nal_length_size = dovi_hvcc_nal_length_size( st->codecpar->extradata,
	                                                     st->codecpar->extradata_size );
	priv->dv_split_active = 1;
	priv->dv_el_expose = 1;

	// EL config: prefer the hvcE BlockAdditionMapping config that FFmpeg >= 9
	// exposes as AV_PKT_DATA_HEVC_CONF coded side data (mkvmerge dual-layer
	// single-track layout); fall back to the BL hvcC for interleaved files
	// where the EL shares the track framing.
	for( int i = 0; i < st->codecpar->nb_coded_side_data; i++ ) {
		AVPacketSideData *sd = &st->codecpar->coded_side_data[i];
		if( sd->type == AV_PKT_DATA_HEVC_CONF && sd->size >= 23 ) {
			priv->dv_el_hvcc = av_malloc( sd->size );
			if( priv->dv_el_hvcc ) {
				memcpy( priv->dv_el_hvcc, sd->data, sd->size );
				priv->dv_el_hvcc_size = sd->size;
			}
			break;
		}
	}
	if( priv->dv_el_hvcc ) {
		v->dv_el_extraData     = priv->dv_el_hvcc;
		v->dv_el_extraDataSize = priv->dv_el_hvcc_size;
		serprintf("FFM: interleaved DV P7 tone-map: hvcE EL config (%d bytes)\n",
		          priv->dv_el_hvcc_size );
	} else {
		v->dv_el_extraData     = v->extraData;
		v->dv_el_extraDataSize = v->extraDataSize;
	}

	serprintf("FFM: interleaved DV P7 tone-map: dovi_split active (nal_length_size %d)\n",
	          priv->dv_nal_length_size );
	return 0;
}

// Codec pull API: next enhancement-layer packet (tone-map mode)
static int _get_dovi_el_packet( STREAM *s, void *pkt )
{
	AVPacket *out = (AVPacket *)pkt;
	if( !ff_p->elq_inited || !out )
		return 1;
	if( !_get_packet( &ff_p->elq, out ) )
		return 1;
	return 0;
}

// find and remove the pending EL packet with the same timestamp as the BL packet
static int _dv_el_take_match( FF_PRIV *priv, AVPacket *bl, AVPacket *el_out )
{
	AVRational bl_tb;
	int i;

	if( bl->pts == AV_NOPTS_VALUE || !priv->time_base_den )
		return 0;

	bl_tb.num = priv->time_base_num;
	bl_tb.den = priv->time_base_den;

	for( i = 0; i < priv->dv_el_pending_count; i++ ) {
		AVPacket *el = &priv->dv_el_pending[i];
		if( el->pts != AV_NOPTS_VALUE &&
		    av_compare_ts( bl->pts, bl_tb, el->pts, priv->dv_el_time_base ) == 0 ) {
			*el_out = *el;
			for( ; i < priv->dv_el_pending_count - 1; i++ )
				priv->dv_el_pending[i] = priv->dv_el_pending[i + 1];
			priv->dv_el_pending_count--;
			memset( &priv->dv_el_pending[priv->dv_el_pending_count], 0, sizeof( AVPacket ) );
			return 1;
		}
	}
	return 0;
}

// ************************************************************
//
//	_parse_once
//
// ************************************************************
static int _parse_once( STREAM *s, int *timestamp)
{
	AVFormatContext *fmt = ff_p->fmt;
	
	if( ff_p->sleeping ) {
		// we are sleeping, decide whether to wake up
		if( s->time_parsed < stream_drive_wake_sleep ) {
			// time to wake up
DBGP serprintf("FFMPEG: wake\r\n");
			ff_p->sleeping = 0;
		} else {
			return 0;
		}
	}

	if( ff_p->aq.mem_used + ff_p->vq.mem_used + ff_p->sq.mem_used > ff_p->buffer_size ) {
		// The audio queue must never be starved by video backpressure:
		// below-realtime software video decode (e.g. Dolby Vision FEL
		// BL+EL composition) would otherwise let the pool stay full, block
		// all reads including audio, and stall A/V sync on every frame.
		// Audio packets are tiny compared to video, so keep reading while
		// the audio queue is below a small cap.
		if( ff_p->aq.packets > 32 ) {
			if( s->time_parsed > stream_drive_wake_sleep && !(ff_p->flags & STREAM_PARSER_FILE_NONLOCAL) ) {
				// time to sleep
DBGP serprintf("FFMPEG: sleep\r\n");
				ff_p->sleeping = 1;
			}
			return 0;
		}
	}
	
	// Read the next packet, skipping all packets that aren't for this stream
	AVPacket packet = { 0 };
	// Read new packet
	if (av_read_frame( fmt, &packet) < 0) {
		if( !s->video_parse_end ) {
DBGP serprintf("FFMPEG: end\r\n");
			s->video_parse_end = 1;
			s->audio_parse_end = 1;
		}
		return 1;
	}

	int stream = packet.stream_index;
DBGP3 serprintf("%8d/%8d/%8d  %4d/%4d/%4d  ", 
			ff_p->aq.mem_used, ff_p->vq.mem_used, ff_p->sq.mem_used, 
			ff_p->aq.packets,  ff_p->vq.packets,  ff_p->sq.packets );
DBGP2 serprintf("pkt [%4d] st %d  size %10d  pos %8lld  %08X  ", 
			ff_p->packet_count++, stream, packet.size, packet.pos, packet.data );
	
	if( s->audio->valid && stream == s->audio->stream ) {
		DBG serprintf("FFMPEG:AUDIO pkt st=%d pts=%lld dts=%lld pos=%lld size=%d seek=%d\n",
			stream,
			(long long)GET_AUDIO_TS( packet.pts ),
			(long long)GET_AUDIO_TS( packet.dts ),
			(long long)packet.pos,
			packet.size,
			s->seek);
DBGP2 serprintf("     AUDIO dts/pts %8lld/%8lld     %02X %02X %02X %02X\r\n", GET_AUDIO_TS( packet.dts ), GET_AUDIO_TS( packet.pts ), packet.data[0], packet.data[1],packet.data[2],packet.data[3] );
DBGC1 serprintf("     AUDIO dts/pts %8lld/%8lld     %02X %02X %02X %02X  %d\r\n", GET_AUDIO_TS( packet.dts ), GET_AUDIO_TS( packet.pts ), packet.data[0], packet.data[1],packet.data[2],packet.data[3], packet.size );
		// add audio packet
		_add_packet( &ff_p->aq, &packet );
		if( timestamp )
			*timestamp = GET_AUDIO_TS( packet.pts );
	} else if( ff_p->dv_el_merge && stream == ff_p->dv_el_stream ) {
DBGP2 serprintf("VIDEO EL   dts/pts %8lld/%8lld  size %d\r\n",
					(long long)GET_VIDEO_TS( packet.dts ), (long long)GET_VIDEO_TS( packet.pts ), packet.size );
		// Dolby Vision profile 7 enhancement layer: hold until paired with
		// the matching base-layer packet
		_dv_el_push( ff_p, &packet );
		if( timestamp )
			*timestamp = -1;
	} else if( ff_p->dv_el_expose && stream == ff_p->dv_el_stream ) {
DBGP2 serprintf("VIDEO EL(q) dts/pts %8lld/%8lld  size %d\r\n",
					(long long)GET_VIDEO_TS( packet.dts ), (long long)GET_VIDEO_TS( packet.pts ), packet.size );
		// Dolby Vision tone-map mode, dual-track: EL packets go to the EL
		// queue; the video codec decodes them separately and libplacebo
		// composites the enhancement layer (mpv f_enhancement_pair style).
		// Convert to the BL time domain first so pts pairing works.
		_dv_el_convert_time( ff_p, &packet, ff_p->dv_el_time_base.num, ff_p->dv_el_time_base.den );
		_add_packet( &ff_p->elq, &packet );
		if( timestamp )
			*timestamp = -1;
	} else if( s->video->valid && stream == s->video->stream ) {
DBGP2 serprintf("VIDEO      dts/pts %8lld/%8lld  %s  %02X %02X %02X %02X\r\n", GET_VIDEO_TS( packet.dts ), GET_VIDEO_TS( packet.pts ), (packet.flags & AV_PKT_FLAG_KEY) ? "I" : " ",
										packet.data[0], packet.data[1],packet.data[2],packet.data[3]  );
DBGC4 serprintf("VIDEO      dts/pts %8lld/%8lld  %s  %02X %02X %02X %02X\r\n", GET_VIDEO_TS( packet.dts ), GET_VIDEO_TS( packet.pts ), (packet.flags & AV_PKT_FLAG_KEY) ? "I" : " ",
										packet.data[0], packet.data[1],packet.data[2],packet.data[3]  );
		if( ff_p->dv_split_active ) {
			// Dolby Vision tone-map mode, single-track interleaved P7: split
			// the combined access unit into BL(+RPU) -> vq and EL -> elq
			// (mpv runs the same dovi_split at demux level). mkvmerge
			// dual-layer files instead carry the EL as an hvcE block
			// addition (AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL, id 'hvcE').
			uint8_t *hvce_el = NULL;
			int hvce_el_size = 0;
			for( int i = 0; i < packet.side_data_elems; i++ ) {
				if( packet.side_data[i].type == AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL &&
				    packet.side_data[i].size > 8 &&
				    AV_RB64( packet.side_data[i].data ) == 0x68766345ULL ) {
					hvce_el = packet.side_data[i].data + 8;
					hvce_el_size = packet.side_data[i].size - 8;
					break;
				}
			}
			AVPacket *bl_out = av_packet_alloc();
			AVPacket *el_out = av_packet_alloc();
			int split_ok = 0;
			if( bl_out && el_out &&
			    _dv_split_packet( ff_p, &packet, bl_out, el_out ) == 0 )
				split_ok = 1;
			if( split_ok ) {
				if( bl_out->data ) {
					// drop the hvcE block addition copy from the BL packet
					av_packet_side_data_remove( bl_out->side_data,
					                            &bl_out->side_data_elems,
					                            AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL );
					_add_packet( &ff_p->vq, bl_out );
				}
				if( !el_out->data && hvce_el_size > 0 ) {
					// EL arrives as a block addition, not in-band NALs
					if( av_new_packet( el_out, hvce_el_size ) == 0 ) {
						memcpy( el_out->data, hvce_el, hvce_el_size );
						el_out->pts = packet.pts;
						el_out->dts = packet.dts;
					}
				}
				if( el_out->data ) {
					// EL inherits the combined packet's pts (BL track timebase);
					// convert to the BL time domain so pts pairing works
					_dv_el_convert_time( ff_p, el_out, ff_p->time_base_num, ff_p->time_base_den );
					_add_packet( &ff_p->elq, el_out );
				}
			} else {
				serprintf("FFM: dovi_split failed on AU, decoding BL only\n");
				_add_packet( &ff_p->vq, &packet );
			}
			if( bl_out ) av_packet_free( &bl_out );
			if( el_out ) av_packet_free( &el_out );
		} else {
			if( ff_p->dv_el_merge && ff_p->dv_el_pending_count ) {
				// Dolby Vision profile 7 dual-track: append the matching EL
				// payload to this BL access unit so the DV decoder+composer
				// receives both layers (like a single-track interleaved stream)
				AVPacket el;
				if( _dv_el_take_match( ff_p, &packet, &el ) ) {
					int bl_size = packet.size;
					if( av_grow_packet( &packet, el.size ) >= 0 ) {
						memcpy( packet.data + bl_size, el.data, el.size );
DBGP2 serprintf("DV: merged EL into BL pts %lld (+%d bytes)\r\n", (long long)packet.pts, el.size );
					}
					av_packet_unref( &el );
				}
			}
			// add video packet
			_add_packet( &ff_p->vq, &packet );
		}
		if( timestamp )
			*timestamp = use_pts ? GET_VIDEO_TS( packet.pts ) : GET_VIDEO_TS( packet.dts );
	} else if( s->subtitle->valid && stream == s->subtitle->stream ) {
DBGP2 serprintf("SUBTITLE   dts/pts %8lld/%8lld  ", GET_SUB_TS( packet.dts ), GET_SUB_TS( packet.pts ) );
DBGP2 DumpLine( packet.data, 16, 16 );		
		// add subtitle packet
		_add_packet( &ff_p->sq, &packet );
		if( timestamp )
			*timestamp = GET_SUB_TS( packet.pts );
	} else {
DBGP2 serprintf("\r\n");
		if( timestamp )
			*timestamp = -1;
	}

	// discard packet
	av_packet_unref(&packet);

	return 0;
}

// ************************************************************
//
//	_parse
//
// ************************************************************
static int _parse( STREAM *s)
{
	// load chunk aggressively, try more often ...
	int i;
	for( i = 0; i < 5; i ++ ) {
		if( _parse_once( s, NULL ) ) {
			return 1;
		}
	}
	return 0;
}

// ************************************************************
//
//	_pauseable
//
// ************************************************************
static int _pauseable( STREAM *s )
{
	return 1;
}

// ************************************************************
//
//	_seekable
//
// ************************************************************
static int _seekable( STREAM *s )
{
	if( s->etype == ETYPE_RTSP ) {
		return 0;
	}
	
	if( s->size == (UINT64)0xFFFFFFFFFFFFFFFull ) {
		return 0;
	}

	return 1;
}

// ************************************************************
//
//	_seek
//
// ************************************************************
static int _seek( STREAM *s, int time, int pos, int dir, int flags, int force_reload, STREAM_CHUNK *sc )
{
	// time argument is rst
DBGP serprintf("FFMPEG: seek: time %8d  pos %5d  dir %d\r\n", time, pos, dir); 
	AVFormatContext *fmt = ff_p->fmt;
	int start = atime();
	
	ff_p->last_audio_time = 0;
	
	int av_flags = dir & STREAM_SEEK_BACKWARD ? AVSEEK_FLAG_BACKWARD : 0;
	
	INT64 new_pos;
	if( time == -1 ) {
		// seek to pos
		new_pos = s->size * pos / STREAM_POS_MAX;
		av_flags |= AVSEEK_FLAG_BYTE;
		
		if( new_pos > s->size ) {
			// pos is beyond end of file - what do we do now?
			DBGP serprintf("at end %lld %llu\r\n", new_pos, s->size);
			// eof reached, stop playback
			s->video_parse_end = 1;
			s->audio_parse_end = 1;
			if ( s->size > 1024 * 1024ul ) {
				new_pos = s->size - 1024 * 1024ul; // 1MB before end
			} else {
				new_pos = 0;
			}
		}
DBGP serprintf("FFMPEG: new pos: %lld\r\n", new_pos );
	} else {
		// TODO: start_time is ts: bug mixing time domains
		new_pos = (INT64)( time + ff_p->start_time ) * AV_TIME_BASE / 1000;
		DBGP serprintf( "FFMPEG: new time: %lld\r\n", new_pos );
	}

	__attribute__((unused))	
	int stream = s->video->valid ? s->video->stream : s->audio->stream;
#if 0
	int ret = av_seek_frame( fmt, stream, new_pos, 1 );
#else
	int64_t seek_min    = dir == STREAM_SEEK_FORWARD ? new_pos : INT64_MIN;
	int64_t seek_max    = dir == STREAM_SEEK_BACKWARD ? new_pos : INT64_MAX;

	int ret = avformat_seek_file( fmt, -1, seek_min, new_pos, seek_max, av_flags);
#endif

	if( ret < 0 ) {
serprintf("FFMPEG: seek error\r\n"); 
		return 1;
	}
	
	s->audio_parse_end = 0;
	s->video_parse_end = 0;

	_flush_packets( &ff_p->vq, "VID" );
	_flush_packets( &ff_p->aq, "AUD" );
	_flush_packets( &ff_p->sq, "SUB" );
	_dv_el_flush( ff_p );
	_dv_elq_flush( ff_p );

	ff_p->sleeping = 0;
	
	// retry until we get a video frame
	int ignore_first = 0;
	if( s->video->format == VIDEO_FORMAT_MPEG ) {
		// for some f*cking reason, lavf is unable to give 
		// us a good key frame after seek in TS, so we scan until
		// the next one...doh
		ignore_first = 1;
	}

	int retry = 500;
	while( retry -- ) {
		_parse_once( s, NULL );
		
		AVPacket _packet;
		AVPacket *packet = _peek_packet( &ff_p->vq, &_packet, 0 );
		if( packet ) {
			int ts = _get_video_time( s, packet ); // returns ts
			DBG2 serprintf( "stream_parser_ffmpeg:_seek time %d, pos %d, rt=%d -> ts=%d\n", time, pos, (int)( audio_interface_get_audio_speed() * ts ), ts );

			if( packet->flags & AV_PKT_FLAG_KEY ) {
				if( ignore_first ) {
DBGP serprintf("ignore! %d\n", ts);
					ignore_first--;
				} else if( ts != -1 ) {
					sc->time = ts; // stream chunk is ts
					break;
				}
			} else {
DBGP serprintf("nokey!  %d\n", ts);
			}
			packet = _get_packet( &ff_p->vq, &_packet );
			_dispose_packet( packet );			
		}
	}
DBGP serprintf("FFMPEG: seek to time %8d  pos %5d  dir %d -> %d/%lld  (took %d)\r\n", time, pos, dir, sc->time, sc->pos, atime() - start ); 
	if( s->audio->valid ) {
		while( 1 ) {
			AVPacket _packet;
			AVPacket *packet = _peek_packet( &ff_p->aq, &_packet, 0 );

			if( !packet )
				break;

			int ts = _get_audio_time( s, packet );
			if( ts >= sc->time ) { // comparison made in ts domain
				break;
			}
DBGP serprintf("audio!  %d\n", ts);
			packet = _get_packet( &ff_p->aq, &_packet );
			_dispose_packet( packet );			
		}
	}
	
	return 0;
}

static int _seek_time( STREAM *s, int time, int dir, int flags, int force_reload, STREAM_CHUNK *sc )
{
	// time is rst
	return _seek( s, time, -1, dir, flags, force_reload, sc );
}

static int _seek_pos( STREAM *s, int time, int dir, int flags, int force_reload, STREAM_CHUNK *sc )
{
	return _seek( s, -1, time, dir, flags, force_reload, sc );
}

// ************************************************************
//
//	_get_audio_cdata
//
// ************************************************************
static int _get_audio_cdata( STREAM *s, CLEVER_BUFFER *audio_buffer, STREAM_CDATA *cdata )
{
	if( cdata->valid != 0 ) {
		return 0;
	}
	
	// drop audio until we have a video key frame
	if( s->video->valid && ff_p->need_key ) {
		return 1;
	}
	
	AVPacket _packet;
	AVPacket *packet = _get_packet( &ff_p->aq, &_packet );
	if( !packet ) {
		return 1;
	}

	if( audio_buffer->size < packet->size ) {
		if ( realloc_clever_buffer( audio_buffer, packet->size ) ) {
			_dispose_packet( packet );			
			return 1;
		}
	}

	// copy relevant chunk info:
	memset( cdata, 0, sizeof( STREAM_CDATA ) );

	cdata->type 	  = 0;
	cdata->key   	  = 1;
	cdata->size       = packet->size;
	cdata->time       = _get_audio_time( s, packet );
	cdata->frame      = 0;
	cdata->pos        = packet->pos;
	
	if( cdata->time != STREAM_NO_PTS_VALUE ) {
		if( ff_p->last_audio_time && abs(cdata->time - ff_p->last_audio_time) > 1000 ) {
			DBG serprintf("FF: audio_skip! %d\n", cdata->time - ff_p->last_audio_time );
			cdata->audio_skip = 1;
		}
		ff_p->last_audio_time = cdata->time;
	}
	
DBGC2  serprintf(" A   siz %6d  pos %8lld   tim %8d  pkt %6d  %8d\r\n", packet->size, packet->pos, cdata->time, ff_p->aq.packets, ff_p->aq.mem_used );
	memcpy( audio_buffer->data, packet->data, packet->size );
	
	cdata->valid = CHUNK_VALID;

	_dispose_packet( packet );
	return 0;
}

// ************************************************************
//
//	_peek_n_audio_chunk
//
// ************************************************************
static STREAM_CHUNK *_peek_n_audio_chunk(STREAM *s, int n, UCHAR **data )
{
	AVPacket _packet;
	AVPacket *packet = _peek_packet( &ff_p->aq, &_packet, 0 );
	if( !packet ) {
		return NULL;
	}
	STREAM_CHUNK *sc = &ff_p->sc;

	sc->stream = s->audio->stream;
	sc->size   = packet->size;
	if( data ) { 
		*data = packet->data;
	}
	return sc;
}

// ************************************************************
//
//	_get_video_cdata
//
// ************************************************************
static int _get_video_cdata( STREAM *s, CBE *cbe, STREAM_CDATA *cdata )
{
	if( cdata->valid != 0 ) {
		return 0;
	}
	
	AVPacket _packet;
	AVPacket *packet = _get_packet( &ff_p->vq, &_packet );
	if( !packet ) {
		return 1;
	}
	memset( cdata, 0, sizeof( STREAM_CDATA ) );
	
	if( ff_p->need_key ) {
		if( !(packet->flags & AV_PKT_FLAG_KEY) ) {
			goto  ErrorExit;
		}
		ff_p->need_key--;
		if ( ff_p->need_key ) {
			goto  ErrorExit;
		}
	}
	// copy relevant chunk info:
	cdata->type 	  = 0;
	cdata->key   	  = (packet->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
	cdata->time       = _get_video_time( s, packet );
	cdata->frame      = 0;
	cdata->pos        = packet->pos;
DBGC8  serprintf("V    siz %6d  pos %8lld %d tim %8d  pkt %6d  %8d\r\n", packet->size, packet->pos, cdata->key, cdata->time, ff_p->vq.packets, ff_p->vq.mem_used );
	if( cdata->key ) {
		// check video props change in case of key frame
		VIDEO_PROPERTIES new = { 0 };
		if( !MPEG_get_video_props( ff_p->video->format, &new, packet->data, 1, packet->size ) ) {
			int changed;
			MPEG_check_video_changed( ff_p->video, &new, &changed );  
			if( changed ) {
				cdata->changed = &ff_p->av;
			}
		}
	}

	if( !s->video->no_extra && s->video->format != VIDEO_FORMAT_WMV3 ) {
		stream_parser_send_video_extra( s->video, cbe, &cdata->size );
	}
	
#ifdef CONFIG_H264
	if( s->video->avcc ) {
		H264_parse_NAL( (UCHAR*)packet->data, packet->size, cbe, &cdata->size, s->video->nal_unit_size );
	} else 
#endif
#ifdef CONFIG_HEVC
	if( s->video->hvcc ) {
		HEVC_parse_NAL( (UCHAR*)packet->data, packet->size, cbe, &cdata->size, s->video->nal_unit_size );
	} else 
#endif
	{
		cbe_write( cbe, (UCHAR*)packet->data, packet->size);
		cdata->size  += packet->size;
	}	
	
	cdata->valid  = CHUNK_VALID;
ErrorExit:	
	_dispose_packet( packet );

	return 0;
}

static int msk_fixup_ssa( char *dst, int max, const char *src, int src_size, int time, int duration )
{
	const char *layer = NULL;
	const char *ptr = src; 
	const char *end = src + src_size;
	
	// skip the count
	for ( ; *ptr != ',' && ptr < end - 1; ptr++ );
	
	// we are at the layer tag
	if ( *ptr == ',' )
		layer = ++ptr;
	
	// find next comma
	for ( ; *ptr != ',' && ptr < end - 1; ptr++ );
	
	// we are at the rest to copy verbatim
	if ( layer && *ptr == ',' ) {
		int sc =  time / 10;
		int ec = (time + duration) / 10;
		
		int sh  = sc / 360000;
		    sc -= 360000 * sh;
		int sm  = sc / 6000;
		    sc -= 6000 * sm;
		int ss  = sc / 100;
		    sc -= 100 * ss;
		
		int eh  = ec / 360000;
		    ec -= 360000 * eh;
		int em  = ec / 6000;
		    ec -= 6000 * em;
		int es  = ec / 100;
		    ec -= 100 * es;
		char *layere = (char*)ptr;
		
		*layere = '\0';
		snprintf( dst, max, "Dialogue: %s,%d:%02d:%02d.%02d,%d:%02d:%02d.%02d,", layer, sh, sm, ss, sc, eh, em, es, ec );
		*layere = ',';
		
		max -= strlen(dst) + 3;
		char *d = dst + strlen(dst);
		ptr ++;
		while( max-- > 0 && *ptr && ptr != end )
			*d++ = *ptr++;
		*d++ = '\r';
		*d++ = '\n';
		*d++ = '\0';
	} else {
		strcpy( dst, "" );
	}
	return strlen( dst );
}

static int msk_fixup_srt( char *dst, int max, const char *src, int src_size, int time, int duration )
{
	char *d = dst;
	max --;
	snprintf( d, max, "%d:%d,", time, time + duration );
	max -= strlen(dst);
	d   += strlen(dst);
	int copy = MIN( max, src_size );
	snprintf( d, copy + 1, "%s", src );
	return strlen( dst );
}

// ************************************************************
//
//	_get_subtitle_cdata
//
// ************************************************************
static int _get_subtitle_cdata( STREAM *s, CLEVER_BUFFER *sub_buffer, STREAM_CDATA *cdata )
{
	if( cdata->valid != 0 ) {
		return 0;
	}

	AVPacket _packet;
	AVPacket *packet = _get_packet( &ff_p->sq, &_packet );
	if( !packet ) {
		return 1;
	}

	if( sub_buffer->size < packet->size + 128 ) {
serprintf("realloc %d -> %d \r\n", sub_buffer->size, packet->size );
		if ( realloc_clever_buffer( sub_buffer, packet->size + 128 ) ) {
			_dispose_packet( packet );			
			return 1;
		}
	}

	// copy relevant chunk info:
	memset( cdata, 0, sizeof( STREAM_CDATA ) );

	cdata->type 	  = 0;
	cdata->key   	  = 1;
	cdata->size       = packet->size;
	cdata->time       = _get_subtitle_time( s, packet ); // ts domain
	cdata->frame      = 0;
	cdata->pos        = packet->pos;
DBGC32 serprintf("  S  siz %6d  pos %8lld   tim %8d  pkt %6d  %8d\r\n", packet->size, packet->pos, cdata->time, ff_p->sq.packets, ff_p->sq.mem_used );

	
	int duration_rst = GET_SUB_TS( packet->duration );
	int duration_ts = RST_TO_TS_DELTA(duration_rst, int);
	if( s->subtitle->format == SUB_FORMAT_SSA ) {
		cdata->size = msk_fixup_ssa( sub_buffer->data, sub_buffer->size, packet->data, packet->size, cdata->time, duration_ts );
	} else if( s->subtitle->format == SUB_FORMAT_TEXT ) {
		cdata->size = msk_fixup_srt( sub_buffer->data, sub_buffer->size, packet->data, packet->size, cdata->time, duration_ts );
	} else {
		memcpy( sub_buffer->data, packet->data, packet->size );
	}
	cdata->valid = CHUNK_VALID;

	_dispose_packet( packet );			
	return 0;
}

// ************************************************************
//
//	_seek_by_index
//
// ************************************************************
static int _seek_by_index( STREAM *s, int idx_size, void *idx_data, int force_reload, STREAM_CHUNK *sc )
{
	return 1;
}

// ************************************************************
//
//	_get_index
//
// ************************************************************
static int _get_index( STREAM *s, int *time, void **data, int *size )
{
	if( data )
		*data = NULL;
	if( size )
		*size = 0;
	
	return 1;
}

// ************************************************************
//
//	_set_audio_stream
//
// ************************************************************
static int _set_audio_stream( STREAM *s, int audio_stream )
{
	return stream_parser_set_audio_stream( s, audio_stream);
}

// ************************************************************
//
//	_calc_rate
//
// ************************************************************
static int _calc_rate( STREAM *s ) {
	if( s->audio->valid ) {
		pthread_mutex_lock( &ff_p->aq.mutex );
		PacketNode *first = (PacketNode*)ff_p->aq.list.first;
		PacketNode *last  = (PacketNode*)ff_p->aq.list.last;
		if( first && last ) {
			int first_time   = GET_AUDIO_TS( first->packet.dts ); // rst domain
			int last_time    = GET_AUDIO_TS( last->packet.dts ); // rst domain
			UINT64 first_pos = first->packet.pos;
			UINT64 last_pos  = last->packet.pos;

			s->atime_parsed = last_time - first_time; // rst domain
			if( s->atime_parsed ) {
				s->acurrent_rate = (UINT64)( last_pos - first_pos ) * (UINT64)1000 / s->atime_parsed;
			} else {
				s->acurrent_rate = 0;
			}
//serprintf("A: 1st %8d  last %8d  diff %8d  rate %8d\r\n", first_time, last_time, s->atime_parsed, s->acurrent_rate );
		}
		
		pthread_mutex_unlock( &ff_p->aq.mutex );
	}
	if( s->video->valid ) {
		pthread_mutex_lock( &ff_p->vq.mutex );
		PacketNode *first = (PacketNode*)ff_p->vq.list.first;
		PacketNode *last  = (PacketNode*)ff_p->vq.list.last;
		if( first && last ) {
			int first_time   = GET_VIDEO_TS( first->packet.dts ); // rst domain
			int last_time    = GET_VIDEO_TS( last->packet.dts ); // rst domain
			UINT64 first_pos = first->packet.pos;
			UINT64 last_pos  = last->packet.pos;

			s->vtime_parsed = last_time - first_time; // rst domain
			if( s->atime_parsed ) {
				// Compensate for compressed timeline in bitrate calculation
				// Note: intentionally uses s->atime_parsed for consistency with audio timeline
				s->vcurrent_rate = (UINT64)(last_pos - first_pos) * (UINT64)1000 / s->atime_parsed;
			} else {
				s->vcurrent_rate = 0;
			}
//serprintf("V: 1st %8d  last %8d  diff %8d  rate %8d\r\n", first_time, last_time, s->vtime_parsed, s->vcurrent_rate );
		}
		
		pthread_mutex_unlock( &ff_p->vq.mutex );
	}

	
	if ( s->audio->valid && s->video->valid ) {
		s->time_parsed  = MIN( s->vtime_parsed,  s->atime_parsed  ); 
		s->current_rate = MAX( s->vcurrent_rate, s->acurrent_rate ); 
	} else if ( s->audio->valid ) {
		s->time_parsed  = s->atime_parsed;
		s->current_rate = s->acurrent_rate;
	} else { 
		s->time_parsed  = s->vtime_parsed;
		s->current_rate = s->vcurrent_rate;
	}
//DBGS2 serprintf("time %5d  rate %8d \r\n", s->time_parsed, s->current_rate );

	return 0;
}

// ************************************************************
//
//	_get_stats
//
// ************************************************************
static STREAM_PARSER_STATS *_get_stats( STREAM *s, STREAM_PARSER_STATS *stats )
{
	memset( stats, 0, sizeof( *stats ) );
	
	stats->buffer_size   = ff_p->buffer_size;
	stats->buffer_used   = ff_p->aq.mem_used + ff_p->vq.mem_used;
	
	stats->audio_chunks  = ff_p->aq.packets;
	stats->video_chunks  = ff_p->vq.packets;

	stats->atime_parsed  = s->atime_parsed;
	stats->vtime_parsed  = s->vtime_parsed;
	
	stats->acurrent_rate = s->acurrent_rate;
	stats->vcurrent_rate = s->vcurrent_rate;
	
	return stats;
}

static STREAM_PARSER stream_parser_FFMPEG = {
	"FFMPEG",
	_open,
	_close,
	stream_parser_pause,
	_parse,
	NULL,		//_parse_chunk,
	_set_audio_stream,
	_calc_rate,
	_get_audio_cdata,
	_get_video_cdata,
	_get_subtitle_cdata,
	_get_dovi_el_packet,
	_peek_n_audio_chunk,
	_seek_time,	//_seek_time
	_seek_pos,	//_seek_pos
	_seek_by_index,
	_seekable,	// seekable
	_pauseable,	// pauseable
	_get_index,
	NULL,		// start_next
	_get_stats,
};

#ifndef CONFIG_LIVE555_RTSP
STREAM_REGISTER_PARSER( ETYPE_RTSP, stream_parser_FFMPEG );
static STREAM_IO *_dummy_new( STREAM_URL *src ) 
{
	return NULL;
}
static char proto[] = "rtsp://";
STREAM_REGISTER_IO( proto, _dummy_new, STREAM_IO_NONLOCAL, ETYPE_RTSP );
#endif

// *****************************************************************************
//
//	get_info_FFMPEG
//
// *****************************************************************************
static int _get_info_FFMPEG( const char *full_path, FILE_INFO *info, APIC *apic, FILE_INFO_ABORT abort )
{
DBGP serprintf("ReadFFMPEGInfo: ");

	FF_PRIV *priv = NULL;
	// allocate private data
	if( !(priv = (FF_PRIV*)amalloc( sizeof( FF_PRIV ) ) ) ) {
		return 1;
	}
	
	memset( priv, 0, sizeof( FF_PRIV ) );
	av_init_props( priv );

	int err = 0;
	AVDictionary **opts = NULL;
	int nb_streams = 0;

	// Open video file
	priv->fmt = avformat_alloc_context();

	// For metadata-only retrieval: use minimal probing to speed up file scanning
	AVDictionary *fmt_opts = NULL;
	av_dict_set(&fmt_opts, "probesize", "500000", 0);      // 500KB instead of 5MB default
	av_dict_set(&fmt_opts, "analyzeduration", "1000000", 0);  // 1 second max
	// Set user agent for HTTP streams to improve compatibility with CDN/debrid services
	av_dict_set(&fmt_opts, "user_agent", "Mozilla/5.0 (Linux; Android) Nova/1.0", 0);

	serprintf("FFMPEG: metadata opening url [%s]\r\n", full_path);
	if( avformat_open_input(&priv->fmt, full_path, NULL, &fmt_opts ) != 0) {
serprintf("FFMPEG: cannot open file [%s]\r\n", full_path);
		av_dict_free(&fmt_opts);
		err = 1;
		goto ErrorExit;
	}
	av_dict_free(&fmt_opts);

DBGP serprintf("info\r\n");
	// Retrieve stream information with stream-specific optimization
	// For file scanning: only analyze video stream thoroughly, minimize audio analysis
	nb_streams = priv->fmt->nb_streams;
	opts = (AVDictionary **)acalloc(nb_streams, sizeof(AVDictionary *));
	if (opts) {
		for (int i = 0; i < nb_streams; i++) {
			if (priv->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
				// Analyze video stream with minimal time
				av_dict_set(&opts[i], "analyzeduration", "1000000", 0);  // 1 second max
			} else {
				// Minimal analysis for audio/subtitles - just get basic info
				av_dict_set(&opts[i], "analyzeduration", "500000", 0);  // 0.5 second
			}
		}
	}
	if (avformat_find_stream_info(priv->fmt, opts) < 0) {
		printf("FFMPEG: cannot find stream info\r\n");
	}
	// Clean up
	if (opts) {
		for (int i = 0; i < nb_streams; i++) {
			av_dict_free(&opts[i]);
		}
		afree(opts);
	}

	_parse_format( info->etype, priv );

	memcpy( &info->av, &priv->av, sizeof( AV_PROPERTIES ) );
	memcpy( &info->id3_tag, &priv->tag, sizeof( ID3_TAG ) );

	info->size     = priv->size;
	info->duration = priv->duration;

	// Format bitstream informations for display
DBGP serprintf("video %d/%d  audio %d/%d\r\n", info->av.vs_max, info->video->valid, info->av.as_max, info->audio->valid);

ErrorExit:
	if( priv && priv->fmt ) {
		// Close the video file
		avformat_close_input(&priv->fmt);
	}
	afree( priv );

	return err;
}

#ifdef CONFIG_MPEG_TS
#ifdef CONFIG_MPEG_TS_FF
STREAM_REGISTER_PARSER( ETYPE_MPEG_TS, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_VID, ETYPE_MPEG_TS, _get_info_FFMPEG );
#endif
#endif

#ifdef CONFIG_WTV
STREAM_REGISTER_PARSER( ETYPE_WTV, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_VID, ETYPE_WTV, _get_info_FFMPEG );
#endif

#ifdef CONFIG_OGV
STREAM_REGISTER_PARSER( ETYPE_OGV, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_VID, ETYPE_OGV, _get_info_FFMPEG );
#endif

#ifdef CONFIG_FLV
STREAM_REGISTER_PARSER( ETYPE_FLV, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_VID, ETYPE_FLV, _get_info_FFMPEG );
#endif

STREAM_REGISTER_PARSER( ETYPE_AMV, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_VID, ETYPE_AMV, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_AC3, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_VID, ETYPE_AC3, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_H264_RAW, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_VID, ETYPE_H264_RAW, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_MPG4_RAW, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_VID, ETYPE_MPG4_RAW, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_MPEG_PS, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_VID, ETYPE_MPEG_PS, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_MPEG_RAW, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_VID, ETYPE_MPEG_RAW, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_DTS, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_VID, ETYPE_DTS, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_MP3, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_AUD, ETYPE_MP3, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_AAC, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_AUD, ETYPE_AAC, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_FLAC, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_AUD, ETYPE_FLAC, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_WAVPACK, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_AUD, ETYPE_WAVPACK, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_TTA, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_AUD, ETYPE_TTA, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_OGG, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_AUD, ETYPE_OGG, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_ASF, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_VID, ETYPE_ASF, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_AVI, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_VID, ETYPE_AVI, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_MP4, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_VID, ETYPE_MP4, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_MKV, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_VID, ETYPE_MKV, _get_info_FFMPEG );

STREAM_REGISTER_PARSER( ETYPE_RM, stream_parser_FFMPEG );
FILE_INFO_REGISTER_PATH( TYPE_VID, ETYPE_RM, _get_info_FFMPEG );

#ifdef DEBUG_MSG
static STREAM_REG_PARSER reg_avi = {
	ETYPE_AVI,
	&stream_parser_FFMPEG,
};

static STREAM_REG_PARSER reg_asf = {
	ETYPE_ASF,
	&stream_parser_FFMPEG,
};

static STREAM_REG_PARSER reg_mkv = {
	ETYPE_MKV,
	&stream_parser_FFMPEG,
};

static STREAM_REG_PARSER reg_ts = {
	ETYPE_MPEG_TS,
	&stream_parser_FFMPEG,
};

static STREAM_REG_PARSER reg_ps = {
	ETYPE_MPEG_PS,
	&stream_parser_FFMPEG,
};

static STREAM_REG_PARSER reg_mp4 = {
	ETYPE_MP4,
	&stream_parser_FFMPEG,
};

static FILE_INFO_REG fi_mkv = {
	TYPE_VID,
	ETYPE_MKV,
	_get_info_FFMPEG,
	"_get_info_FFMPEG",
	NULL,
	NULL,
};

static FILE_INFO_REG fi_ogg = {
	TYPE_VID,
	ETYPE_OGG,
	_get_info_FFMPEG,
	"_get_info_FFMPEG",
	NULL,
	NULL,
};

static void _reg_ff( void ) 
{
serprintf("register lavf for AVI\r\n");
	stream_unregister_parser( ETYPE_AVI );
	stream_register_parser( &reg_avi );

serprintf("register lavf for ASF\r\n");
	stream_unregister_parser( ETYPE_ASF );
	stream_register_parser( &reg_asf );

serprintf("register lavf for MKV\r\n");
	stream_unregister_parser( ETYPE_MKV );
	stream_register_parser( &reg_mkv );

serprintf("register lavf for MKV info\r\n");
	file_info_unregister( TYPE_VID, ETYPE_MKV );
	file_info_register( &fi_mkv );

serprintf("register lavf for TS\r\n");
	stream_unregister_parser( ETYPE_MPEG_TS );
	stream_register_parser( &reg_ts );
	
serprintf("register lavf for PS\r\n");
	stream_unregister_parser( ETYPE_MPEG_PS );
	stream_register_parser( &reg_ps );

serprintf("register lavf for MP4\r\n");
	stream_unregister_parser( ETYPE_MP4 );
	stream_register_parser( &reg_mp4 );

serprintf("register lavf for OGG info\r\n");
	file_info_unregister( TYPE_VID, ETYPE_OGG );
	file_info_register( &fi_ogg );

}

DECLARE_DEBUG_COMMAND_VOID( "regff", 	_reg_ff );
#endif

#endif	// CONFIG_FFMPEG_PARSER
#endif

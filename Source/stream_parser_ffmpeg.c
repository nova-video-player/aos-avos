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
#include "stream_heap.h"

#ifdef CONFIG_STREAM
#ifdef CONFIG_FFMPEG_PARSER

#include <libavutil/mathematics.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>

#include <string.h>

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

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>

static int use_stream_heap = 1;
static int max_delay       = 100000;
static int log_debug       = 0;
static int use_pts         = 1;
static int force_reorder   = -1;
static int force_vpid      = 0;
static int force_apid      = 0;

DECLARE_DEBUG_TOGGLE("ffsh",  use_stream_heap );
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
	
} FF_PRIV;


static int ff_force_seek = 1;

DECLARE_DEBUG_PARAM( "fffs", ff_force_seek );

static int _close( STREAM *s );
static int _flush_packets( AVQueue *q, const char *tag );

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

	// Subtitle
	{ AV_CODEC_ID_DVD_SUBTITLE,SUB_FORMAT_DVD_GFX,	0 },
//	{ AV_CODEC_ID_DVB_SUBTITLE,SUB_FORMAT_DVBT,	0 },
	{ AV_CODEC_ID_TEXT,	SUB_FORMAT_TEXT,	0 },
	{ AV_CODEC_ID_SUBRIP,   SUB_FORMAT_TEXT,        0 },	
	{ AV_CODEC_ID_XSUB,	SUB_FORMAT_XSUB,	0 },
	{ AV_CODEC_ID_SSA,	SUB_FORMAT_SSA,		0 },
	{ AV_CODEC_ID_ASS,	SUB_FORMAT_SSA,		0 },
};


static const char *disposition_name( int disposition )
{
	if( disposition & AV_DISPOSITION_DEFAULT )
		return "(default)";
	if (disposition & AV_DISPOSITION_DUB)
		return "(dub)";
	if (disposition & AV_DISPOSITION_ORIGINAL)
		return "(original)";
	if (disposition & AV_DISPOSITION_COMMENT)
		return "(comment)";
	if (disposition & AV_DISPOSITION_LYRICS)
		return "(lyrics)";
	if (disposition & AV_DISPOSITION_KARAOKE)
		return "(karaoke)";
	if (disposition & AV_DISPOSITION_FORCED)
		return "(forced)";
	if (disposition & AV_DISPOSITION_HEARING_IMPAIRED)
		return "(hearing impaired)";
	if (disposition & AV_DISPOSITION_VISUAL_IMPAIRED)
		return "(visual impaired)";
	if (disposition & AV_DISPOSITION_CLEAN_EFFECTS)
		return "(clean effects)";
	if( disposition & AV_DISPOSITION_ATTACHED_PIC)
		return "(attached pic)";
		
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
//	_parse_format
//
// ************************************************************
static int _parse_format( int etype, FF_PRIV *priv ) 
{
	AVFormatContext *fmt = priv->fmt;

DBGP serprintf("format   [%s]\r\n", fmt->iformat->name );

	if( fmt->pb ) {
		priv->size = avio_size(fmt->pb);
DBGP serprintf("size     %lld\r\n", priv->size );
	}
	
	if (fmt->duration != AV_NOPTS_VALUE && etype != ETYPE_MPEG_TS ) {
		priv->duration = 1000 * (INT64)fmt->duration / AV_TIME_BASE;
DBGP serprintf("duration %d\r\n", priv->duration );
	} else {
		if( priv->s )
			priv->s->no_duration = 1;
DBGP serprintf("duration ---\r\n" );
	}

	if (fmt->start_time != AV_NOPTS_VALUE) {
DBGP serprintf("FFMPEG start    %lld\r\n",  fmt->start_time);
		priv->start_time = 1000 * (INT64)fmt->start_time / AV_TIME_BASE;
DBGP serprintf("start    %d\r\n", priv->start_time );
	}
DBGP serprintf("bitrate  %d\r\n", fmt->bit_rate);

	int i;
	for(i = 0; i < fmt->nb_streams; i++) {
		AVStream *st          = fmt->streams[i];
		AVCodecContext *codec = st->codec;
		int discard = 1;
DBGP serprintf("Stream #%d: ", i);
		if(st->codec->codec_type == AVMEDIA_TYPE_VIDEO){
DBGP serprintf("VIDEO\r\n");
		} else if( st->codec->codec_type == AVMEDIA_TYPE_AUDIO ){
DBGP serprintf("AUDIO\r\n");
		} else if( st->codec->codec_type == AVMEDIA_TYPE_SUBTITLE ){
DBGP serprintf("SUBTITLE\r\n");
		} else if( st->codec->codec_type == AVMEDIA_TYPE_ATTACHMENT ){
DBGP serprintf("ATTACHEMENT\r\n");
		} else {
DBGP serprintf("<unknown> type %d\r\n", st->codec->codec_type);
		}
		int flags = fmt->iformat->flags;
		if (flags & AVFMT_SHOW_IDS) {
DBGP serprintf("\tPID        0x%x\r\n", st->id);
		}
        	AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL,0);
		if (lang) {
DBGP serprintf("\tlanguage   %s -> %s\r\n", lang->value, map_ISO639_code( lang->value ) );
		}
		int gcd = av_gcd(st->time_base.num, st->time_base.den);
DBGP serprintf("\tnum/dem    %d/%d\r\n", st->time_base.num/gcd, st->time_base.den/gcd);
DBGP serprintf("\tcodec_id   %X\r\n", codec->codec_id);
		const AVCodecDescriptor *desc = avcodec_descriptor_get(codec->codec_id);
DBGP serprintf("\tcodec_name %s\r\n", desc ? desc->name : "");
DBGP serprintf("\tcodec_tag  [%.4s]\r\n", &codec->codec_tag);
		if( codec->extradata_size ) {
DBGP serprintf("\textraSz    %d\r\n", codec->extradata_size);
DBGP serprintf("\textra      "); 
DBGP DumpLine( codec->extradata, MIN(128,codec->extradata_size), MIN(128,codec->extradata_size) );
		}
DBGP serprintf("\tbitrate    %d\r\n", codec->bit_rate);
DBGP serprintf("\tdisposition %d / %s\r\n", st->disposition, disposition_name(st->disposition));
		
		if(st->codec->codec_type == AVMEDIA_TYPE_VIDEO){
			//
			// video
			//
			if( st->disposition == AV_DISPOSITION_ATTACHED_PIC ) {
				goto DISCARD_STREAM;
			}
			if(st->avg_frame_rate.den && st->avg_frame_rate.num) {
DBGP serprintf("\tfps        %5.2f fps(r)\r\n", av_q2d(st->avg_frame_rate));
			} else {
DBGP serprintf("\tfps        %5.2f fps(c)\r\n", 1/av_q2d(codec->time_base));
			}
DBGP serprintf("\tPAR        %d/%d\r\n", codec->sample_aspect_ratio.num, codec->sample_aspect_ratio.den ); 
			if ( priv->av.vs_max < VIDEO_STREAM_MAX ) {
				VIDEO_PROPERTIES *video = priv->av.video + priv->av.vs_max;
				
				video->stream = i;

				if(st->avg_frame_rate.den && st->avg_frame_rate.num) {
					video->rate  = st->avg_frame_rate.num;
					video->scale = st->avg_frame_rate.den;
				} else {
					//video->scale  = st->time_base.num;
					//video->rate   = st->time_base.den;
				}
				
				priv->time_base_num = st->time_base.num/gcd;
				priv->time_base_den = st->time_base.den/gcd;
				video->frames = 0;
				video->valid  = 1;
				
				//if( priv->av.vs_max == 0 && video->rate )
				//	priv->duration = (UINT32)( 1000ull * (UINT64)video->frames * (UINT64) video->scale / (UINT64) video->rate);

				video->codec_id	   = codec->codec_id;
				strnZcpy( video->codec_name, desc ? desc->name : "", AV_NAME_LEN );
				
				video->fourcc      = codec->codec_tag;
				video->format      = get_ff_format( codec->codec_id, &video->fourcc  );
				if( video->format == 0 && video->codec_id ) {
					video->format = VIDEO_FORMAT_LAVC;
					video->fourcc = VIDEO_FOURCC_LAVC;
				}
				
				if( codec->extradata_size ) {
					if( codec->extradata_size <= sizeof( video->extraData ) ) {
						video->extraDataSize = codec->extradata_size;
						memcpy( video->extraData, codec->extradata, video->extraDataSize  );	
						if( video->format == VIDEO_FORMAT_H264 && video->extraData[0] == 0x00 ) {
serprintf("FF: parse H264 SPS\n");
							// for non-AVCC H264, parse the SPS/PPS here
							H264_get_video_props( video, video->extraData, video->extraDataSize, &video->sps );
						}
					} else {
						video->extraDataSize  = 0;
						video->extraDataSize2 = codec->extradata_size;
						video->extraData2     = codec->extradata;
					}
				} 
				
				video->width       = codec->width;
				video->height      = codec->height;
				video->aspect_n    = codec->sample_aspect_ratio.num;
				video->aspect_d	   = codec->sample_aspect_ratio.den;
				video->bytesPerSec = codec->bit_rate / 8;
				
				
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
			}
		} else if( st->codec->codec_type == AVMEDIA_TYPE_AUDIO ){
			//
			// audio
			//
DBGP serprintf("\tsampleRate %d\r\n", codec->sample_rate);
DBGP serprintf("\tblockAlign %d\r\n", codec->block_align);
DBGP serprintf("\tchannels   %d\r\n", codec->channels);

			if(st->avg_frame_rate.den && st->avg_frame_rate.num) {
DBGP serprintf("\tfps        %5.2f fps(r)\r\n", av_q2d(st->avg_frame_rate));
			} else if( codec->time_base.den && codec->time_base.num ) {
DBGP serprintf("\tfps        %5.2f fps(c)\r\n", 1/av_q2d(codec->time_base));
			}

			if ( priv->av.as_max < AUDIO_STREAM_MAX ) {	
				AUDIO_PROPERTIES *audio = priv->av.audio + priv->av.as_max;

				audio->codec_id	     = codec->codec_id;
				strnZcpy( audio->codec_name, desc ? desc->name : "", AV_NAME_LEN );
				audio->format        = get_ff_format( codec->codec_id, NULL );

				if( audio->format == 0 && audio->codec_id ) {
					audio->format = WAVE_FORMAT_LAVC;
				}

				audio->stream        = i;
				audio->scale         = st->time_base.num/gcd;
				audio->rate          = st->time_base.den/gcd;
				audio->frames        = 0;
				audio->channels      = codec->channels;
				audio->samplesPerSec = codec->sample_rate;
				audio->bitsPerSample = 0;
				audio->blockAlign    = codec->block_align;
				audio->bytesPerSec   = codec->bit_rate / 8;
				audio->valid         = 1;
				stream_set_audio_name( audio, priv->av.as_max + 1 ); 
				if( st->disposition && st->disposition != AV_DISPOSITION_DEFAULT ) {
					strcat( audio->name, " " );
					strcat( audio->name, disposition_name(st->disposition) );
					if( st->disposition & (AV_DISPOSITION_HEARING_IMPAIRED | AV_DISPOSITION_VISUAL_IMPAIRED)) {
						audio->priority = 2;
					}
				}
				
				if ( audio->format == WAVE_FORMAT_IMA ) {
					audio->samplesPerBlock = 0;
				}

				if( codec->extradata_size ) {
					if( codec->extradata_size <= sizeof( audio->extraData ) ) {
						audio->extraDataSize = codec->extradata_size;
						memcpy( audio->extraData, codec->extradata, audio->extraDataSize  );	
					} else {
						audio->extraDataSize  = 0;
						audio->extraDataSize2 = codec->extradata_size;
						audio->extraData2     = codec->extradata;
					}
				} 
				
				// hack for stupid canon cameras!
				if ( audio->samplesPerSec == 11024 )
					audio->samplesPerSec ++;
				
				//_check_VBR( audio );
				
				priv->av.as_max ++;
				discard = 0;
			} 
		} else if( st->codec->codec_type == AVMEDIA_TYPE_SUBTITLE ){
			//
			// subtitle
			//
			int fmt = get_ff_format( codec->codec_id, NULL );
			if( fmt && priv->av.subs_max < SUB_STREAM_MAX ) {
				SUB_PROPERTIES *sub = priv->av.sub + priv->av.subs_max;
	
				sub->valid          = 1;
				sub->codec_id	    = codec->codec_id;
				strnZcpy( sub->codec_name, desc ? desc->name : "", AV_NAME_LEN );
				sub->format         = fmt;
				sub->gfx            = (sub->format == SUB_FORMAT_DVD_GFX) ? 1 : 0;
				sub->stream         = i;
				sub->scale          = st->time_base.num;
				sub->rate           = st->time_base.den;
				sub->extraData2     = codec->extradata;
				sub->extraDataSize2 = codec->extradata_size;

				if( lang ) {
					snprintf(sub->name, AV_NAME_LEN, "%s", map_ISO639_code( lang->value ) );
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

	if( fmt->nb_chapters ) {
DBGP serprintf("chapters:\r\n");	
		for( i =0; i < fmt->nb_chapters; i++ ) {
			AVChapter *ch = fmt->chapters[i];
			UINT64 start = 1000 * ch->start * ch->time_base.num / ch->time_base.den; 
			UINT64 end   = 1000 * ch->end   * ch->time_base.num / ch->time_base.den; 
        		AVDictionaryEntry *t = av_dict_get(ch->metadata, "title", NULL, 0);
 DBGP serprintf("[%2d] id %08X  start/end %8lld/%8lld  [%s]\r\n", i, ch->id, start, end, t ? t->value : "(no title)" );
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

	av_register_all();
	// allocate private data
	if( !(s->parser_priv = (FF_PRIV*)amalloc( sizeof( FF_PRIV ) ) ) ) {
		goto ErrorExit;
	}
	
	memset( ff_p, 0, sizeof( FF_PRIV ) );
	av_init_props( ff_p );
	ff_p->s = s;
	
	ff_p->flags = flags;
	
	stream_parser_clear_chunks( s );

	if( use_stream_heap ) {
		if( stream_heap_create( buffer_size ) ) {
			goto ErrorExit;
		}
		ff_p->buffer_size = (uint64_t)buffer_size * (uint64_t)90 / 100;	// allow 10% overhead in heap
	} else {
		ff_p->buffer_size = buffer_size;
	}

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
		
#ifdef OPEN_AVIO
	int avio_flags = AVIO_FLAG_READ;
#endif
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

#ifdef OPEN_AVIO
	// open the AVIO by hand so we can pass custom flags
	if( avio_open2( &ff_p->fmt->pb, s->src.url, avio_flags , &ff_p->fmt->interrupt_callback, &ff_p->fmt_opts  ) ) {
serprintf("FFMPEG: cannot open avio for %s\r\n", s->src.url);
		goto ErrorExit3;
	}
#endif
	av_dict_set(&ff_p->fmt_opts, "probesize", "10000000", 0);

	if( avformat_open_input(&ff_p->fmt, s->src.url, NULL, &ff_p->fmt_opts ) != 0) {
serprintf("FFMPEG: cannot open file\r\n");
		goto ErrorExit4;
	}

DBGP serprintf("info\r\n");

	// Retrieve stream information
	if (avformat_find_stream_info(ff_p->fmt, NULL) < 0) {
printf("FFMPEG: cannot find stream info\r\n");
	}
	
	_parse_format( s->etype, ff_p );

	memcpy( &s->av, &ff_p->av, sizeof( AV_PROPERTIES ) );

	s->duration = ff_p->duration;
	s->size     = ff_p->size;
	
	LinkedList_init( &ff_p->aq.list );
	LinkedList_init( &ff_p->vq.list );
	LinkedList_init( &ff_p->sq.list );

	pthread_mutex_init( &ff_p->aq.mutex, NULL );
	pthread_mutex_init( &ff_p->vq.mutex, NULL );
	pthread_mutex_init( &ff_p->sq.mutex, NULL );

	// make lavf parser use this sync mode!
	s->sync_mode = STREAM_SYNC_SAMPLES;

	s->parser_open = 1;

	if( s->video->valid ) {
		ff_p->need_key = 1;
	}
	return 0;

ErrorExit4:
#ifdef OPEN_AVIO
	avio_close(ff_p->fmt->pb);

ErrorExit3:
#endif
	av_dict_free(&ff_p->fmt_opts);
	avformat_network_deinit();

ErrorExit2:
	if( use_stream_heap ) {
		if( ff_p->buffer_size ) {
			stream_heap_destroy();
		}
	}
	
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
#ifdef OPEN_AVIO
			// close the avio
			avio_close(ff_p->fmt->pb);
#endif
			// Close the video file
			avformat_close_input(&ff_p->fmt);
		}


		_flush_packets( &ff_p->vq, "VID" );
		_flush_packets( &ff_p->aq, "AUD" );
		_flush_packets( &ff_p->sq, "SUB" );

		if( use_stream_heap ) {
			if( ff_p->buffer_size ) {
				stream_heap_destroy();
			}
		}

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

#define GET_AUDIO_TS( ts ) ( ts == AV_NOPTS_VALUE ? -1 : (INT64)ts * 1000 * (INT64)s->audio->scale     / s->audio->rate )
#define GET_VIDEO_TS( ts ) ( ts == AV_NOPTS_VALUE ? -1 : (INT64)ts * 1000 * (INT64)ff_p->time_base_num / ff_p->time_base_den )
#define GET_SUB_TS( ts )   ( ts == AV_NOPTS_VALUE ? -1 : (INT64)ts * 1000 * (INT64)s->subtitle->scale  / s->subtitle->rate )

// ************************************************************
//
//	_get_video_time
//
// ************************************************************
static int _get_video_time( STREAM *s, AVPacket *packet )
{
	return ( (use_pts && packet->pts != AV_NOPTS_VALUE ) ? GET_VIDEO_TS( packet->pts ) : GET_VIDEO_TS( packet->dts )) - ff_p->start_time;
}

// ************************************************************
//
//	_get_audio_time
//
// ************************************************************
static int _get_audio_time( STREAM *s, AVPacket *packet )
{
	int t = GET_AUDIO_TS( packet->pts );
	return (t == -1) ? -1 : t - ff_p->start_time;
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
DBGP2 serprintf("FFMPEG full %d %d %d %d\r\n", ff_p->aq.mem_used, ff_p->vq.mem_used, ff_p->sq.mem_used, ff_p->buffer_size);
		if( s->time_parsed > stream_drive_wake_sleep && !(ff_p->flags & STREAM_PARSER_FILE_NONLOCAL) ) {
			// time to sleep
DBGP serprintf("FFMPEG: sleep\r\n");
			ff_p->sleeping = 1;
		}
		return 0;		
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
DBGP2 serprintf("     AUDIO dts/pts %8lld/%8lld     %02X %02X %02X %02X\r\n", GET_AUDIO_TS( packet.dts ), GET_AUDIO_TS( packet.pts ), packet.data[0], packet.data[1],packet.data[2],packet.data[3] );
DBGC1 serprintf("     AUDIO dts/pts %8lld/%8lld     %02X %02X %02X %02X  %d\r\n", GET_AUDIO_TS( packet.dts ), GET_AUDIO_TS( packet.pts ), packet.data[0], packet.data[1],packet.data[2],packet.data[3], packet.size );
		// add audio packet
		_add_packet( &ff_p->aq, &packet );
		if( timestamp )
			*timestamp = GET_AUDIO_TS( packet.pts );
	} else if( s->video->valid && stream == s->video->stream ) {
DBGP2 serprintf("VIDEO      dts/pts %8lld/%8lld  %s  %02X %02X %02X %02X\r\n", GET_VIDEO_TS( packet.dts ), GET_VIDEO_TS( packet.pts ), (packet.flags & AV_PKT_FLAG_KEY) ? "I" : " ",
										packet.data[0], packet.data[1],packet.data[2],packet.data[3]  );
DBGC4 serprintf("VIDEO      dts/pts %8lld/%8lld  %s  %02X %02X %02X %02X\r\n", GET_VIDEO_TS( packet.dts ), GET_VIDEO_TS( packet.pts ), (packet.flags & AV_PKT_FLAG_KEY) ? "I" : " ",
										packet.data[0], packet.data[1],packet.data[2],packet.data[3]  );
		// add video packet
		_add_packet( &ff_p->vq, &packet );
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
	av_free_packet(&packet);
			
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
			if ( s->size > 1024 * 1024ul ) {
				new_pos = s->size - 1024 * 1024ul; // 1MB before end
			} else {
				new_pos = 0;
			}
		}
DBGP serprintf("FFMPEG: new pos: %lld\r\n", new_pos );
	} else {
		new_pos = (INT64)(time + ff_p->start_time) * AV_TIME_BASE / 1000;
DBGP serprintf("FFMPEG: new time: %lld\r\n", new_pos );
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
			int ts = _get_video_time( s, packet );

			if( packet->flags & AV_PKT_FLAG_KEY ) {
				if( ignore_first ) {
DBGP serprintf("ignore! %d\n", ts);
					ignore_first--;
				} else if( ts != -1 ) {
					sc->time = ts;
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
			if( ts >= sc->time ) {
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
	
	if( cdata->time != -1 ) {
		if( ff_p->last_audio_time && abs(cdata->time - ff_p->last_audio_time) > 1000 ) {
serprintf("FF: audio_skip! %d\n", cdata->time - ff_p->last_audio_time );
			cdata->audio_skip = 1;
		}
		ff_p->last_audio_time = cdata->time;
	}
	
DBGC2 serprintf("get audio: size %6d  pos %8lld  tim %8d  pkt %4d  %8d\r\n", packet->size, packet->pos, cdata->time, ff_p->aq.packets, ff_p->aq.mem_used );
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
DBGC8 serprintf("get VIDEO: size %6d  pos %8lld  key %d  tim %8d  pkt %4d  %8d\r\n", packet->size, packet->pos, cdata->key, cdata->time, ff_p->vq.packets, ff_p->vq.mem_used );
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
static int ff_get_subtitle_cdata( STREAM *s, CLEVER_BUFFER *sub_buffer, STREAM_CDATA *cdata )
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
	cdata->time       = (GET_SUB_TS( packet->pts) == -1) ? -1 : GET_SUB_TS(packet->pts) - ff_p->start_time;
	cdata->frame      = 0;
	cdata->pos        = packet->pos;
DBGC32 serprintf("get  sub : size %6d  pos %8lld  time %8d  pkt %4d  %8d\r\n", packet->size, packet->pos, cdata->time, ff_p->sq.packets, ff_p->sq.mem_used );

	
	int duration = GET_SUB_TS( packet->duration );
	if( s->subtitle->format == SUB_FORMAT_SSA ) {
		cdata->size = msk_fixup_ssa( sub_buffer->data, sub_buffer->size, packet->data, packet->size, cdata->time, duration );
	} else if( s->subtitle->format == SUB_FORMAT_TEXT ) {
		cdata->size = msk_fixup_srt( sub_buffer->data, sub_buffer->size, packet->data, packet->size, cdata->time, duration );
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
static int _calc_rate( STREAM *s )
{
	if( s->audio->valid ) {
		pthread_mutex_lock( &ff_p->aq.mutex );
		PacketNode *first = (PacketNode*)ff_p->aq.list.first;
		PacketNode *last  = (PacketNode*)ff_p->aq.list.last;
		if( first && last ) {
			int first_time   = GET_AUDIO_TS( first->packet.dts );
			int last_time    = GET_AUDIO_TS( last->packet.dts );
			UINT64 first_pos = first->packet.pos;
			UINT64 last_pos  = last->packet.pos;

			s->atime_parsed = last_time - first_time;
			if( s->atime_parsed ) {
				s->acurrent_rate = (UINT64)(last_pos - first_pos) * (UINT64)1000 / (UINT64)s->atime_parsed;
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
			int first_time   = GET_VIDEO_TS( first->packet.dts );
			int last_time    = GET_VIDEO_TS( last->packet.dts );
			UINT64 first_pos = first->packet.pos;
			UINT64 last_pos  = last->packet.pos;

			s->vtime_parsed = last_time - first_time;
			if( s->atime_parsed ) {
				s->vcurrent_rate = (UINT64)(last_pos - first_pos) * (UINT64)1000 / (UINT64)s->atime_parsed;
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
	ff_get_subtitle_cdata,
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

	av_register_all();

	int err = 0;
	// Open video file
	priv->fmt = avformat_alloc_context();
	if( avformat_open_input(&priv->fmt, full_path, NULL, NULL ) != 0) {
serprintf("FFMPEG: cannot open file\r\n");
		err = 1;
		goto ErrorExit;
	}

DBGP serprintf("info\r\n");
	// Retrieve stream information
	if (avformat_find_stream_info(priv->fmt, NULL) < 0) {
printf("FFMPEG: cannot find stream info\r\n");
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

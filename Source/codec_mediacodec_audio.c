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

#include "global.h"
#include "types.h"
#include "debug.h"
#include "util.h"
#include "stream.h"
#include "stream_alloc.h"
#include "astdlib.h"
#include "athread.h"
#include "rc_clocks.h"
#include "h264.h"
#include "device_config.h"
#include "xdm_utils.h"
#include "pts_reorder.h"
#include "dec_audio.h"

#include <time.h>
#include <stdint.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#ifdef CONFIG_STREAM

#define DBGS if( 0 || Debug[DBG_STREAM] )
#define DBG DBG_IF(Debug[DBG_STREAM])
#define DBGCV if( 0 || Debug[DBG_CV] )
#define DBGCV2 if( 0 || Debug[DBG_CV] > 1 )
#define DBGCV3 if( 0 || Debug[DBG_CV] > 2 )
#define DBGSI if( 0 || Debug[DBG_SINK] )
#define DBGSI2 if( 0 || Debug[DBG_SINK] > 1 )
int acodecs_is_supported( int format, int is_video, int is_sw_allowed );

#define MEDIACODEC_CAP_AC3             5
#define MEDIACODEC_CAP_E_AC3           6
#define MEDIACODEC_CAP_DTS             7
#define MEDIACODEC_CAP_DTS_HD          8
#define MEDIACODEC_CAP_MP3             9
#define MEDIACODEC_CAP_AAC             10
#define MEDIACODEC_CAP_DOLBY_TRUEHD    14
#define MEDIACODEC_CAP_E_AC3_JOC       18
#define MEDIACODEC_CAP_OPUS            20

typedef struct PRIV {
	struct dec_audio *dec_audio;
	AVCodecParserContext *aparser;
	struct AVCodecContext avctx;
} PRIV;

static const char *mediacodec_capability_name( int capability_bit )
{
	switch( capability_bit ) {
	case MEDIACODEC_CAP_AC3: return "AC3";
	case MEDIACODEC_CAP_E_AC3: return "E_AC3";
	case MEDIACODEC_CAP_DTS: return "DTS";
	case MEDIACODEC_CAP_DTS_HD: return "DTS_HD";
	case MEDIACODEC_CAP_MP3: return "MP3";
	case MEDIACODEC_CAP_AAC: return "AAC";
	case MEDIACODEC_CAP_DOLBY_TRUEHD: return "TRUEHD";
	case MEDIACODEC_CAP_E_AC3_JOC: return "E_AC3_JOC";
	case MEDIACODEC_CAP_OPUS: return "OPUS";
	default: return "unknown";
	}
}

static int wave2libav_codecid( int codecid )
{
	switch( codecid ) {
	case WAVE_FORMAT_AC3:
		return AV_CODEC_ID_AC3;
	case WAVE_FORMAT_EAC3:
	case WAVE_FORMAT_E_AC3_JOC:
		return AV_CODEC_ID_EAC3;
	case WAVE_FORMAT_DTS_HD_MA:
	case WAVE_FORMAT_DTS_HD:
	case WAVE_FORMAT_DTS:
		return AV_CODEC_ID_DTS;
	case WAVE_FORMAT_MPEG:
	case WAVE_FORMAT_MPEGLAYER3:
		return AV_CODEC_ID_MP3;
	case WAVE_FORMAT_TRUEHD:
		return AV_CODEC_ID_TRUEHD;
	case WAVE_FORMAT_AAC:
	case WAVE_FORMAT_AAC_LATM:
		return AV_CODEC_ID_AAC;
	case WAVE_FORMAT_OPUS:
		return AV_CODEC_ID_OPUS;
	default:
		return 0;
	}
}

static int mediacodec_audio_codec_open( AUDIO_PROPERTIES *audio )
{
	PRIV *p = (PRIV *)audio->priv;
	sfdec_codec_t sfdec_codec;

	if( !p ) {
		return 1;
	}
	audio->bitsPerSample = 16;

	memset( &p->avctx, 0, sizeof( p->avctx ) );
	int codecid = wave2libav_codecid( audio->format );
	p->aparser = av_parser_init( codecid );
	if( !p->aparser ) serprintf( "cannot open parser for %04X\r\n", codecid );

	switch( audio->format ) {
	case WAVE_FORMAT_MPEG:
	case WAVE_FORMAT_MPEGLAYER3:
		sfdec_codec = SFDEC_AUDIO_MP3;
		break;
	case WAVE_FORMAT_AAC:
	case WAVE_FORMAT_AAC_LATM:
		sfdec_codec = SFDEC_AUDIO_AAC;
		break;
	case WAVE_FORMAT_AC3:
		sfdec_codec = SFDEC_AUDIO_AC3;
		break;
	case WAVE_FORMAT_EAC3:
	case WAVE_FORMAT_E_AC3_JOC:
		sfdec_codec = SFDEC_AUDIO_EAC3;
		break;
	case WAVE_FORMAT_DTS:
		sfdec_codec = SFDEC_AUDIO_DTS;
		break;
	case WAVE_FORMAT_DTS_HD_MA:
	case WAVE_FORMAT_DTS_HD:
		sfdec_codec = SFDEC_AUDIO_DTS_HD;
		break;
	case WAVE_FORMAT_TRUEHD:
		sfdec_codec = SFDEC_AUDIO_TRUEHD;
		break;
	case WAVE_FORMAT_OPUS:
		sfdec_codec = SFDEC_AUDIO_OPUS;
		break;
	default:
		DBG serprintf("mediacodec_audio_codec_open: unsupported format=%s\n", audio_get_format_name(audio));
		return 1;
	}

	void *extradata = NULL;
	size_t extradata_size = 0;
	if ( audio->extraDataSize ) {
		extradata = audio->extraData;
		extradata_size = audio->extraDataSize;
	}
	if ( audio->extraDataSize2 ) {
		extradata = audio->extraData2;
		extradata_size = audio->extraDataSize2;
	}

	DBGS serprintf("extraDataSize: %d, extraDataSize2 : %d, using %d\r\n", audio->extraDataSize, audio->extraDataSize2, extradata_size);
	DBGS serprintf("codec_delay %lld seek_preroll %lld\n", audio->codec_delay, audio->seek_preroll);
	DBG serprintf("mediacodec_audio_codec_open: format=%s sfdec_codec=%d channels=%d rate=%d extradata=%zu\n",
		audio_get_format_name(audio), sfdec_codec, audio->channels, audio->samplesPerSec, extradata_size);

	p->dec_audio = dec_audio_new( sfdec_codec, 0, 0, audio->samplesPerSec, audio->channels, audio->bitsPerSample, extradata, extradata_size, audio->codec_delay, audio->seek_preroll);
	if(!p->dec_audio) {
		DBG serprintf("mediacodec_audio_codec_open: dec_audio_new failed for format=%s\n", audio_get_format_name(audio));
		return 1;
	}
	return 0;
}

static int mediacodec_audio_codec_delete( AUDIO_PROPERTIES *audio )
{
	PRIV *p = (PRIV *)audio->priv;
	if( !p ) {
		return 0;
	}
	if( p->dec_audio ) {
		dec_audio_delete( p->dec_audio );
		p->dec_audio = NULL;
	}
	if( p->aparser ) {
		av_parser_close( p->aparser );
		p->aparser = NULL;
	}
	free( p );
	audio->priv = NULL;
	return 0;
}
static int mediacodec_audio_codec_new( AUDIO_PROPERTIES *audio )
{
DBGS serprintf( "mediacodec audio new\r\n");
        if( !audio ) {
                return 1;
        }
        if( !(audio->priv = amalloc( sizeof( PRIV ) ) ) ) {
serprintf("mediacodec audio: cannot alloc context\n");
                return 1;
        }
	memset( audio->priv, 0, sizeof( PRIV ) );
        return 0;
}

static int mediacodec_audio_codec_close( AUDIO_PROPERTIES *audio )
{
	PRIV *p = (PRIV *)audio->priv;
	if( !p || !p->dec_audio ) {
		return 0;
	}
	dec_audio_stop_input( p->dec_audio );

	return 0;
}

static int mediacodec_audio_codec_decode( AUDIO_PROPERTIES *audio, UCHAR *data, int size, AUDIO_FRAME *avos_frame,
										  int *_decoded, int *_time )
{
	DBGS serprintf("mediacodec_audio_codec_decode in\n");
	PRIV *p = (PRIV *)audio->priv;
	if( !p || !p->dec_audio ) {
		return 1;
	}
	int t1 = time_update_time();
	*_decoded = (int)dec_audio_send_input( p->dec_audio, data, size, (int64_t)avos_frame->time, 0, 1 );
	sfdec_read_out_t read_out;
	int ret = dec_audio_read( p->dec_audio, 0, &read_out );

	if( read_out.flag & SFDEC_READ_BUF ) {
		avos_frame->data = read_out.buf.out;
		avos_frame->size = read_out.buf.out_size;
		avos_frame->channels = read_out.channels;
		avos_frame->samplesPerSec = read_out.samplesPerSec / audio->channels * read_out.channels;
		avos_frame->format = WAVE_FORMAT_PCM;
		avos_frame->error = 0;
		avos_frame->bits = 16;
		dec_audio_buf_render( p->dec_audio, read_out.buf.sfbuf, 0 );

	} else
		avos_frame->error = 1;
	int t2 = time_update_time();

	*_time = t2 - t1;

	DBGS serprintf("mediacodec_audio_codec_decode out\n");
	return ret;
}

static int mediacodec_audio_codec_flush( AUDIO_PROPERTIES *audio )
{
	DBGS serprintf("mediacodec_flush in\n");
	PRIV *p = (PRIV *)audio->priv;
	if( !p || !p->dec_audio ) {
		return 0;
	}
	dec_audio_flush( p->dec_audio );

	DBGS serprintf("mediacodec_flush out\n");
	return 0;
}
static int mediacodec_audio_codec_delay( AUDIO_PROPERTIES *audio ) { return 0; }
static int mediacodec_audio_codec_get_rc( AUDIO_PROPERTIES *audio, STREAM_RC *rc )
{
	if( !rc ) return 1;
	memset( rc, 0, sizeof( STREAM_RC ) );
	return 0;
}

static int mediacodec_audio_codec_is_supported( AUDIO_PROPERTIES *audio )
{
	int64_t capabilities = device_config_get_mediacodec_audio_capabilities();
	int capability_bit = -1;

	switch( audio->format ) {
	case WAVE_FORMAT_MPEG:
	case WAVE_FORMAT_MPEGLAYER3:
		capability_bit = MEDIACODEC_CAP_MP3;
		break;
	case WAVE_FORMAT_AC3:
		capability_bit = MEDIACODEC_CAP_AC3;
		break;
	case WAVE_FORMAT_EAC3:
		capability_bit = MEDIACODEC_CAP_E_AC3;
		break;
	case WAVE_FORMAT_E_AC3_JOC:
		capability_bit = MEDIACODEC_CAP_E_AC3_JOC;
		break;
	case WAVE_FORMAT_DTS:
		capability_bit = MEDIACODEC_CAP_DTS;
		break;
	case WAVE_FORMAT_DTS_HD:
	case WAVE_FORMAT_DTS_HD_MA:
		capability_bit = MEDIACODEC_CAP_DTS_HD;
		break;
	case WAVE_FORMAT_AAC:
	case WAVE_FORMAT_AAC_LATM:
		capability_bit = MEDIACODEC_CAP_AAC;
		break;
	case WAVE_FORMAT_TRUEHD:
		capability_bit = MEDIACODEC_CAP_DOLBY_TRUEHD;
		break;
	case WAVE_FORMAT_OPUS:
		capability_bit = MEDIACODEC_CAP_OPUS;
		break;
	default:
		break;
	}

	if( capability_bit >= 0 && capabilities >= 0 ) {
		int supported = (capabilities & ((int64_t)1 << capability_bit)) != 0;
		DBGS serprintf("mediacodec_audio_codec_is_supported: format=%s capability=%s flags=0x%" PRIx64 " -> %s\n",
			audio_get_format_name(audio),
			mediacodec_capability_name(capability_bit),
			capabilities,
			supported ? "yes" : "no");
		return supported;
	}

	if( acodecs_is_supported( audio->format, 0, 1 ) ) {
		DBGS serprintf("mediacodec_audio_codec_is_supported: format=%s using legacy JNI capability probe -> yes\n",
			audio_get_format_name(audio));
		return 1;
	}
	DBGS serprintf("mediacodec_audio_codec_is_supported: format=%s using legacy JNI capability probe -> no\n",
		audio_get_format_name(audio));
	return 0;
}

static STREAM_DEC_AUDIO stream_dec_audio_mediacodec = {
	.name = "MediaCodec",
	.new = mediacodec_audio_codec_new,
	.open = mediacodec_audio_codec_open,
	.close = mediacodec_audio_codec_close,
	.decode = mediacodec_audio_codec_decode,
	.flush = mediacodec_audio_codec_flush,
	.delay = mediacodec_audio_codec_delay,
	.get_rc = mediacodec_audio_codec_get_rc,
	.delete = mediacodec_audio_codec_delete,
	.is_supported = mediacodec_audio_codec_is_supported,
};

STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_AC3, stream_dec_audio_mediacodec, 6 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_EAC3, stream_dec_audio_mediacodec, 8 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_E_AC3_JOC, stream_dec_audio_mediacodec, 8 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_MPEG, stream_dec_audio_mediacodec, 2 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_MPEGLAYER3, stream_dec_audio_mediacodec, 2 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_AAC, stream_dec_audio_mediacodec, 8 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_AAC_LATM, stream_dec_audio_mediacodec, 8 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_DTS, stream_dec_audio_mediacodec, 8 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_DTS_HD, stream_dec_audio_mediacodec, 8 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_DTS_HD_MA, stream_dec_audio_mediacodec, 8 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_TRUEHD, stream_dec_audio_mediacodec, 8 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_OPUS, stream_dec_audio_mediacodec, 8 );
#endif

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

#define MEDIACODEC_AUDIO_MAX_INPUT_SIZE (512 * 1024)

typedef struct PRIV {
	struct dec_audio *dec_audio;
	AVCodecParserContext *aparser;
	AVCodecContext *avctx;
	int parser_codec_id;
	int parser_drained;
	int64_t parser_last_input_time;
	UCHAR *access_unit_buffer;
	size_t access_unit_capacity;
	int access_unit_size;
	int64_t access_unit_time;
	UCHAR *pcm_buffer;
	size_t pcm_buffer_capacity;
	int draining;
	int input_eos_queued;
	int output_eos_pending;
} PRIV;

static int mediacodec_audio_ensure_pcm_capacity( PRIV *p, size_t size )
{
	if( size <= p->pcm_buffer_capacity ) {
		return 0;
	}

	UCHAR *buffer = (UCHAR *)arealloc( p->pcm_buffer, size );
	if( !buffer ) {
		return 1;
	}
	p->pcm_buffer = buffer;
	p->pcm_buffer_capacity = size;
	return 0;
}

static int mediacodec_audio_ensure_access_unit_capacity( PRIV *p, size_t size )
{
	if( size > MEDIACODEC_AUDIO_MAX_INPUT_SIZE ) {
		return 1;
	}
	if( size <= p->access_unit_capacity ) {
		return 0;
	}

	UCHAR *buffer = (UCHAR *)arealloc( p->access_unit_buffer, size );
	if( !buffer ) {
		return 1;
	}
	p->access_unit_buffer = buffer;
	p->access_unit_capacity = size;
	return 0;
}

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

static int wave2parser_codecid( int format )
{
	switch( format ) {
	case WAVE_FORMAT_MPEG:
	case WAVE_FORMAT_MPEGLAYER3:
		return AV_CODEC_ID_MP3;
	case WAVE_FORMAT_AAC_LATM:
		return AV_CODEC_ID_AAC_LATM;
	case WAVE_FORMAT_AC3:
		return AV_CODEC_ID_AC3;
	case WAVE_FORMAT_DTS:
		return AV_CODEC_ID_DTS;
	default:
		return AV_CODEC_ID_NONE;
	}
}

static int mediacodec_audio_parser_reset( PRIV *p )
{
	if( p->aparser ) {
		av_parser_close( p->aparser );
		p->aparser = NULL;
	}
	if( !p->avctx ) {
		p->avctx = avcodec_alloc_context3( NULL );
		if( !p->avctx ) {
			return 1;
		}
	}
	p->avctx->codec_type = AVMEDIA_TYPE_AUDIO;
	p->avctx->codec_id = p->parser_codec_id;
	p->parser_drained = 0;
	p->parser_last_input_time = STREAM_NO_PTS_VALUE;
	p->access_unit_size = 0;
	p->access_unit_time = STREAM_NO_PTS_VALUE;
	if( p->parser_codec_id == AV_CODEC_ID_NONE ) {
		return 0;
	}
	p->aparser = av_parser_init( p->parser_codec_id );
	return p->aparser ? 0 : 1;
}

static int mediacodec_audio_store_access_unit( PRIV *p, const UCHAR *data,
	int size, int64_t fallback_time )
{
	if( !data || size <= 0 ||
		mediacodec_audio_ensure_access_unit_capacity( p, (size_t)size ) ) {
		return 1;
	}
	memcpy( p->access_unit_buffer, data, (size_t)size );
	p->access_unit_size = size;
	p->access_unit_time = p->aparser && p->aparser->pts != AV_NOPTS_VALUE ?
		p->aparser->pts : fallback_time;
	return 0;
}

static int mediacodec_audio_parse_input( PRIV *p, UCHAR *data, int size,
	int64_t input_time, int *consumed )
{
	UCHAR *output = NULL;
	int output_size = 0;
	int64_t parser_time = input_time == STREAM_NO_PTS_VALUE ? AV_NOPTS_VALUE : input_time;
	if( input_time != STREAM_NO_PTS_VALUE ) {
		p->parser_last_input_time = input_time;
	}
	int parsed = av_parser_parse2( p->aparser, p->avctx,
		&output, &output_size, data, size,
		parser_time, parser_time, 0 );
	if( parsed < 0 || parsed > size ) {
		return 1;
	}
	*consumed = parsed;
	if( output_size > 0 && mediacodec_audio_store_access_unit( p, output,
		output_size, p->parser_last_input_time ) ) {
		return 1;
	}
	if( size > 0 && parsed == 0 && output_size == 0 ) {
		serprintf("mediacodec_audio: parser made no progress on %d bytes\n", size);
		return 1;
	}
	return 0;
}

static int mediacodec_audio_queue_pending_access_unit( PRIV *p, int *queued )
{
	*queued = 0;
	if( p->access_unit_size <= 0 ) {
		return 0;
	}
	ssize_t accepted = dec_audio_send_input( p->dec_audio,
		p->access_unit_buffer, (size_t)p->access_unit_size,
		p->access_unit_time, 0, 1 );
	if( accepted < 0 || accepted > p->access_unit_size ) {
		return 1;
	}
	if( accepted == 0 ) {
		return 0;
	}
	if( accepted != p->access_unit_size ) {
		serprintf("mediacodec_audio: partial access-unit queue %zd/%d\n",
			accepted, p->access_unit_size);
		return 1;
	}
	p->access_unit_size = 0;
	*queued = 1;
	return 0;
}

static int mediacodec_audio_codec_open( AUDIO_PROPERTIES *audio )
{
	PRIV *p = (PRIV *)audio->priv;
	sfdec_codec_t sfdec_codec;

	if( !p ) {
		return 1;
	}
	audio->bitsPerSample = 16;

	p->parser_codec_id = wave2parser_codecid( audio->format );
	if( mediacodec_audio_parser_reset( p ) ) {
		serprintf("mediacodec_audio_codec_open: cannot open parser codec=%d format=%04X\n",
			p->parser_codec_id, audio->format);
		return 1;
	}

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

	DBGS serprintf("extraDataSize: %d, extraDataSize2: %d, using %zu\r\n",
		audio->extraDataSize, audio->extraDataSize2, extradata_size);
	DBGS serprintf("codec_delay %" PRIu64 " seek_preroll %" PRIu64 "\n",
		(uint64_t)audio->codec_delay, (uint64_t)audio->seek_preroll);
	DBG serprintf("mediacodec_audio_codec_open: format=%s sfdec_codec=%d channels=%d rate=%d extradata=%zu\n",
		audio_get_format_name(audio), sfdec_codec, audio->channels, audio->samplesPerSec, extradata_size);

	p->dec_audio = dec_audio_new( sfdec_codec, 0, MEDIACODEC_AUDIO_MAX_INPUT_SIZE,
		audio->samplesPerSec, audio->channels, audio->bitsPerSample,
		extradata, extradata_size, audio->codec_delay, audio->seek_preroll );
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
	if( p->avctx ) {
		avcodec_free_context( &p->avctx );
	}
	afree( p->access_unit_buffer );
	p->access_unit_buffer = NULL;
	p->access_unit_capacity = 0;
	p->access_unit_size = 0;
	afree( p->pcm_buffer );
	p->pcm_buffer = NULL;
	p->pcm_buffer_capacity = 0;
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
	return dec_audio_stop( p->dec_audio );
}

static int mediacodec_audio_codec_decode( AUDIO_PROPERTIES *audio, UCHAR *data, int size, AUDIO_FRAME *avos_frame,
										  int *_decoded, int *_time )
{
	if( _decoded ) {
		*_decoded = 0;
	}
	if( _time ) {
		*_time = 0;
	}
	if( !avos_frame ) {
		return 1;
	}
	int64_t input_time = avos_frame->time;
	memset( avos_frame, 0, sizeof( *avos_frame ) );
	avos_frame->error = STREAM_ERROR_FATAL;

	if( !audio ) {
		return 1;
	}
	PRIV *p = (PRIV *)audio->priv;
	if( !p || !p->dec_audio ) {
		return 1;
	}
	int t1 = time_update_time();
	if( p->output_eos_pending ) {
		p->output_eos_pending = 0;
		avos_frame->error = 0;
		return STREAM_DEC_AUDIO_DRAINED;
	}

	if( p->aparser ) {
		int queued = 0;
		if( mediacodec_audio_queue_pending_access_unit( p, &queued ) ) {
			serprintf("mediacodec_audio_codec_decode: failed to queue parsed access unit\n");
			goto out;
		}

		if( !p->draining && p->access_unit_size == 0 && size > 0 ) {
			int consumed = 0;
			if( mediacodec_audio_parse_input( p, data, size, input_time, &consumed ) ) {
				serprintf("mediacodec_audio_codec_decode: compressed parser failed\n");
				goto out;
			}
			if( _decoded ) {
				*_decoded = consumed;
			}
			if( !queued && mediacodec_audio_queue_pending_access_unit( p, &queued ) ) {
				serprintf("mediacodec_audio_codec_decode: failed to queue parsed access unit\n");
				goto out;
			}
		} else if( p->draining && p->access_unit_size == 0 && !p->parser_drained ) {
			int consumed = 0;
			if( mediacodec_audio_parse_input( p, NULL, 0,
				p->parser_last_input_time, &consumed ) ) {
				serprintf("mediacodec_audio_codec_decode: parser drain failed\n");
				goto out;
			}
			if( p->access_unit_size == 0 ) {
				p->parser_drained = 1;
			} else if( !queued &&
				mediacodec_audio_queue_pending_access_unit( p, &queued ) ) {
				serprintf("mediacodec_audio_codec_decode: failed to queue drained access unit\n");
				goto out;
			}
		}

		if( p->draining && p->parser_drained &&
			p->access_unit_size == 0 && !queued && !p->input_eos_queued ) {
			int eos_ret = dec_audio_stop_input( p->dec_audio );
			if( eos_ret < 0 ) {
				serprintf("mediacodec_audio_codec_decode: failed to queue input EOS\n");
				goto out;
			}
			p->input_eos_queued = eos_ret == 0;
		}
	} else if( p->draining ) {
		if( !p->input_eos_queued ) {
			int eos_ret = dec_audio_stop_input( p->dec_audio );
			if( eos_ret < 0 ) {
				serprintf("mediacodec_audio_codec_decode: failed to queue input EOS\n");
				goto out;
			}
			p->input_eos_queued = eos_ret == 0;
		}
	} else {
		if( size > MEDIACODEC_AUDIO_MAX_INPUT_SIZE ) {
			serprintf("mediacodec_audio_codec_decode: access unit too large %d/%d\n",
				size, MEDIACODEC_AUDIO_MAX_INPUT_SIZE);
			goto out;
		}
		ssize_t accepted = dec_audio_send_input( p->dec_audio, data, size,
			input_time, 0, 1 );
		if( accepted < 0 || accepted > size ) {
			serprintf("mediacodec_audio_codec_decode: invalid input consumption %zd/%d\n",
				accepted, size);
			goto out;
		}
		if( _decoded ) {
			*_decoded = (int)accepted;
		}
	}

	sfdec_read_out_t read_out = { 0 };
	int ret = dec_audio_read( p->dec_audio, 0, &read_out );
	if( ret < 0 ) {
		serprintf("mediacodec_audio_codec_decode: output dequeue failed (%d)\n", ret);
		// Input already queued to MediaCodec or retained by the parser remains consumed.
		goto out;
	}

	if( read_out.flag & SFDEC_READ_BUF ) {
		if( !read_out.buf.sfbuf || !read_out.buf.out || read_out.buf.out_size <= 0 ||
			read_out.channels <= 0 || read_out.samplesPerSec <= 0 ||
			mediacodec_audio_ensure_pcm_capacity( p, (size_t)read_out.buf.out_size ) ) {
			serprintf("mediacodec_audio_codec_decode: invalid output size=%d channels=%d rate=%d\n",
				read_out.buf.out_size, read_out.channels, read_out.samplesPerSec);
			if( read_out.buf.sfbuf ) {
				dec_audio_buf_release( p->dec_audio, read_out.buf.sfbuf );
			}
			goto out;
		}
		if( read_out.pcmEncoding != 2 ) {
			serprintf("mediacodec_audio_codec_decode: unsupported PCM encoding %d\n",
				read_out.pcmEncoding);
			dec_audio_buf_release( p->dec_audio, read_out.buf.sfbuf );
			goto out;
		}

		memcpy( p->pcm_buffer, read_out.buf.out, (size_t)read_out.buf.out_size );
		if( dec_audio_buf_release( p->dec_audio, read_out.buf.sfbuf ) ) {
			serprintf("mediacodec_audio_codec_decode: output release failed\n");
			goto out;
		}

		avos_frame->data = p->pcm_buffer;
		avos_frame->size = read_out.buf.out_size;
		avos_frame->channels = read_out.channels;
		avos_frame->samplesPerSec = read_out.samplesPerSec;
		avos_frame->format = WAVE_FORMAT_PCM;
		avos_frame->error = 0;
		avos_frame->bits = 16;
		if( read_out.channelMask ) {
			audio->channelMask = read_out.channelMask;
		}
		if( read_out.flag & SFDEC_READ_EOS ) {
			p->output_eos_pending = 1;
		}
	} else {
		// MediaCodec commonly accepts input before producing the first PCM frame.
		avos_frame->format = WAVE_FORMAT_PCM;
		avos_frame->channels = audio->channels;
		avos_frame->samplesPerSec = audio->samplesPerSec;
		avos_frame->bits = 16;
		avos_frame->error = 0;
		if( read_out.flag & SFDEC_READ_EOS ) {
			return STREAM_DEC_AUDIO_DRAINED;
		}
	}

out:
	{
		int t2 = time_update_time();
		if( _time ) {
			*_time = t2 - t1;
		}
	}

	return avos_frame->error ? 1 : 0;
}

static int mediacodec_audio_codec_drain( AUDIO_PROPERTIES *audio )
{
	PRIV *p = (PRIV *)audio->priv;
	if( !p || !p->dec_audio ) {
		return 1;
	}
	p->draining = 1;
	p->input_eos_queued = 0;
	p->output_eos_pending = 0;
	p->parser_drained = 0;
	return 0;
}

static int mediacodec_audio_codec_flush( AUDIO_PROPERTIES *audio )
{
	DBGS serprintf("mediacodec_flush in\n");
	PRIV *p = (PRIV *)audio->priv;
	if( !p || !p->dec_audio ) {
		return 0;
	}
	p->draining = 0;
	p->input_eos_queued = 0;
	p->output_eos_pending = 0;
	int parser_ret = mediacodec_audio_parser_reset( p );
	int codec_ret = dec_audio_flush( p->dec_audio );
	int ret = parser_ret || codec_ret;
	DBGS serprintf("mediacodec_flush out parser=%d codec=%d ret=%d\n",
		parser_ret, codec_ret, ret);
	return ret;
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

	if( audio->request_channels > 0 && audio->request_channels != audio->channels ) {
		DBGS serprintf("mediacodec_audio_codec_is_supported: format=%s requires channel conversion %d->%d -> no\n",
			audio_get_format_name(audio), audio->channels, audio->request_channels);
		return 0;
	}

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
		if( audio->format == WAVE_FORMAT_E_AC3_JOC && !supported ) {
			// The MediaCodec path opens the E-AC3 base decoder for JOC streams.
			supported = (capabilities & ((int64_t)1 << MEDIACODEC_CAP_E_AC3)) != 0;
		}
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
	if( audio->format == WAVE_FORMAT_E_AC3_JOC &&
		acodecs_is_supported( WAVE_FORMAT_EAC3, 0, 1 ) ) {
		DBGS serprintf("mediacodec_audio_codec_is_supported: JOC using legacy E-AC3 base capability -> yes\n");
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
	.drain = mediacodec_audio_codec_drain,
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

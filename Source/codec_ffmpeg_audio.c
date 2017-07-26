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
#include "astdlib.h"
#include "mp3.h"
#include "get.h"
#include "downmix.h"
#include "device_config.h"

#ifdef CONFIG_FFMPEG_AUDIO
#ifdef CONFIG_STREAM

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>

#define DBGS 	if(Debug[DBG_STREAM])

#define DBGCA 	if(Debug[DBG_CA])
#define DBGCA2 	if(Debug[DBG_CA]  > 1 )
#define DBGCA3 	if(Debug[DBG_CA] == 3 )
#define DBGCA4 	if(Debug[DBG_CA] == 4 )
#define DBGCA5 	if(Debug[DBG_CA] == 5 )

#if LIBAVCODEC_VERSION_MICRO >= 100
	#define IS_FFMPEG
#endif

static int sleep_arm = 0;

static int channel_map[8] = { CH_FL, CH_FR, CH_CTR, CH_SUB, CH_BL, CH_BR, CH_SL, CH_SR };

//
//	AUDIO
//
typedef struct PRIV {
	AVCodecContext 	*actx;
	AVCodec 	*acodec;
	AVCodecParserContext *aparser;
	AVFrame         *aframe;
	SHORT		*asamples;
	SHORT		*bsamples;
	int 		open;
	int 		play;
	int 		ignore;
	int 		parse_first;
	int		request_channels;

	unsigned char 	inbuf[16384];
	int 		inbuf_size;
	int 		inbuf_residual;
} PRIV;

static inline int clamp( int v )
{
        if( v > 32767 ) {
                return 32767;
        } else if ( v < -32768 ) {
                return -32768;
        }
        return v;
}

static int ffmpeg_audio_codec_new( AUDIO_PROPERTIES *audio )
{
DBGS serprintf( "stream_dec_audio_new_FFMPEG\r\n");
	if( !audio ) {
		return 1;
	}
	if( !(audio->priv = amalloc( sizeof( PRIV ) ) ) ) {
serprintf("ffa: cannot alloc context\n");
		return 1;		
	}
	return 0;
}

static int ffmpeg_audio_codec_delete( AUDIO_PROPERTIES *audio )
{
	PRIV *p = (PRIV*)audio->priv;
DBGS serprintf( "stream_dec_audio_delete_FFMPEG\r\n");
	if( !p ) {
		return 1;
	}
	afree( p );
	p = NULL;	
	return 0;
}

static AVCodec *get_avcodec( AUDIO_PROPERTIES *audio )
{
	int codec_id;

	switch( audio->format ) {
	case WAVE_FORMAT_MPEGLAYER3:
	case WAVE_FORMAT_MPEG:
		codec_id    = AV_CODEC_ID_MP3;
		break;
	case WAVE_FORMAT_MSAUDIO2:
		codec_id    = AV_CODEC_ID_WMAV2;
		break;
	case WAVE_FORMAT_MSAUDIO3:
		codec_id    = AV_CODEC_ID_WMAPRO;
		break;
	case WAVE_FORMAT_MSAUDIO_SPEECH:
		codec_id    = AV_CODEC_ID_WMAVOICE;
		break;
	case WAVE_FORMAT_MSAUDIO_LOSSLESS:
		codec_id    = AV_CODEC_ID_WMALOSSLESS;
		break;
	case WAVE_FORMAT_AAC:
		codec_id    = AV_CODEC_ID_AAC;
		break;
	case WAVE_FORMAT_AAC_LATM:
		codec_id    = AV_CODEC_ID_AAC_LATM;
		break;
	case WAVE_FORMAT_AC3:
		codec_id    = AV_CODEC_ID_AC3;
		break;
	case WAVE_FORMAT_COOK:
		codec_id    = AV_CODEC_ID_COOK;
		break;
	case WAVE_FORMAT_VOICEAGE_AMR:
		codec_id    = AV_CODEC_ID_AMR_NB;
		break;
	case WAVE_FORMAT_VOICEAGE_AMR_WB:
		codec_id    = AV_CODEC_ID_AMR_WB;
		break;
	case WAVE_FORMAT_OGG1:
		codec_id    = AV_CODEC_ID_VORBIS;
		break;
	case WAVE_FORMAT_FLAC:
		codec_id    = AV_CODEC_ID_FLAC;
		break;
	case WAVE_FORMAT_DTS:
		codec_id    = AV_CODEC_ID_DTS;
		break;
	case WAVE_FORMAT_DTS_HD:
		codec_id    = AV_CODEC_ID_DTS;
		break;
	case WAVE_FORMAT_WAVPACK:
		codec_id    = AV_CODEC_ID_WAVPACK;
		break;
	case WAVE_FORMAT_TTA:
		codec_id    = AV_CODEC_ID_TTA;
		break;
	case WAVE_FORMAT_TRUEHD:
		codec_id    = AV_CODEC_ID_TRUEHD;
		break;
	case WAVE_FORMAT_EAC3:
		codec_id    = AV_CODEC_ID_EAC3;
		break;
	case WAVE_FORMAT_PCM_BLURAY:
		codec_id    = AV_CODEC_ID_PCM_BLURAY;
		break;
	case WAVE_FORMAT_ALAW:
		codec_id    = AV_CODEC_ID_PCM_ALAW;
		break;
	case WAVE_FORMAT_MULAW:
		codec_id    = AV_CODEC_ID_PCM_MULAW;
		break;
	case WAVE_FORMAT_LAVC:
		if( audio->codec_id ) {
			codec_id = audio->codec_id;
			break;
		}
		// fallthrough
	default:
		return NULL;
	}
	
	return avcodec_find_decoder( codec_id );
}

int ffmpeg_audio_get_profile( AUDIO_PROPERTIES *audio, UCHAR *data, int size, int *profile, int *channels )
{
	if( profile )
		*profile = 0;
		
DBGS serprintf( "ffmpeg_audio_get_profile: format %04X  size %d\n", audio->format, size);
	avcodec_register_all();

	AVCodec *acodec = get_avcodec( audio );
	if( !acodec ) {
serprintf("cannot find codec\r\n");
		return 1;
	}
	
	AVCodecContext *actx = avcodec_alloc_context3(acodec);

	actx->sample_rate = audio->samplesPerSec;
	actx->block_align = audio->blockAlign;
	actx->bit_rate    = audio->bytesPerSec * 8;
	actx->channels    = audio->channels;

	if( audio->extraDataSize2 ) {
		actx->extradata      = audio->extraData2;
		actx->extradata_size = audio->extraDataSize2;
	} else {
		actx->extradata      = audio->extraData;
		actx->extradata_size = audio->extraDataSize;
	}

	if (avcodec_open2(actx, acodec, NULL) < 0) {
serprintf("cannot open codec\r\n");
		goto ErrorExit;
	}
	
	AVFrame *aframe = av_frame_alloc();

	AVPacket avpkt = { .data = data, .size = size };
	av_init_packet(&avpkt);

	int got_frame;
	av_frame_unref(aframe);
	avcodec_decode_audio4( actx, aframe, &got_frame, &avpkt);
	if( profile )
		*profile = actx->profile;
	if( channels )
		*channels = actx->channels;
	
	av_free(aframe);

	return 0;

ErrorExit:
	// Close the codec
	if ( actx ) {
		avcodec_close( actx );
		av_free( actx );
	}
	
	return 1;
}

#define MAX_AUDIO_FRAME_SIZE 192000

static int ffmpeg_audio_codec_open( AUDIO_PROPERTIES *audio )
{
	PRIV *p = (PRIV*)audio->priv;
	
DBGS serprintf( "stream_dec_audio_open_FFMPEG: ");
	if (!p)
		return 1;
	memset( p, 0, sizeof( PRIV ) );
	avcodec_register_all();

	int need_parser = 0;

	if (!device_config_is_audio_format_supported(audio->format)) {
		goto ErrorExit;
	}
	switch( audio->format ) {
	case WAVE_FORMAT_MPEGLAYER3:
	case WAVE_FORMAT_MPEG:
		need_parser = 1;		
		p->inbuf_size = 2048;
		break;
	case WAVE_FORMAT_AAC_LATM:
		need_parser = 1;		
		p->inbuf_size = 8192;
		break;
	case WAVE_FORMAT_AC3:
		need_parser = 1;		
		p->inbuf_size = 3840;
		break;
	case WAVE_FORMAT_FLAC:
		need_parser = 1;		
		p->inbuf_size = 8192; 	// FLAC avg block size
		break;
	case WAVE_FORMAT_DTS:
		need_parser = 1;
		p->inbuf_size = sizeof( p->inbuf );
		break;
	}
	
	p->acodec = get_avcodec( audio );
	if( !p->acodec ) {
serprintf("cannot find codec\r\n");
		goto ErrorExit;
	}
	
	p->actx = avcodec_alloc_context3(p->acodec);
	p->request_channels = audio->request_channels;
	
	// provide all the data that the decoder might need
	p->actx->sample_rate      = audio->samplesPerSec;
	p->actx->block_align      = audio->blockAlign;
	p->actx->bit_rate         = audio->bytesPerSec * 8;
	p->actx->channels         = audio->channels;
	p->actx->request_channel_layout = av_get_default_channel_layout(audio->channels);
	if (p->request_channels == 2)
		p->actx->request_channel_layout = av_get_default_channel_layout(audio->channels ? MIN(2, audio->channels) : 2);

DBGCA2	serprintf("requested channel layout id %d for %d channel(s)\r\n", p->actx->request_channel_layout, p->actx->channels);

	if( audio->extraDataSize2 ) {
		p->actx->extradata      = audio->extraData2;
		p->actx->extradata_size = audio->extraDataSize2;
	} else {
		p->actx->extradata      = audio->extraData;
		p->actx->extradata_size = audio->extraDataSize;
	}

	// Open codec
	if (avcodec_open2(p->actx, p->acodec, NULL) < 0) {
serprintf("cannot open codec\r\n");
		goto ErrorExit;
	}
	if( need_parser ) {
		p->aparser = av_parser_init(p->actx->codec_id);
		if( !p->aparser ) {
serprintf("cannot open parser for %04X\r\n", p->actx->codec_id );
			goto ErrorExit;
		}
	}
	
DBGS serprintf("name %s  type %d  id %d \r\n", p->acodec->name, p->acodec->type, p->acodec->id);

	if (!p->request_channels) {
		switch( p->actx->sample_fmt ) {	
		case AV_SAMPLE_FMT_FLT:
		case AV_SAMPLE_FMT_DBL:
		case AV_SAMPLE_FMT_FLTP:
		case AV_SAMPLE_FMT_DBLP:
			p->request_channels = av_get_channel_layout_nb_channels(p->actx->request_channel_layout);
			break;
		default:
			break;
		}
	}
	audio->sourceSamples = audio->samplesPerSec;
	audio->sourceChannels = audio->channels;
	audio->sourceBitsPerSample = audio->bitsPerSample;

	audio->samplesPerSec = p->actx->sample_rate;
	if (p->request_channels == 2) {
serprintf("downmix to stereo S16\r\n");
		audio->channels      = 2;
		audio->bitsPerSample = 16;
	} else {
		audio->channels = p->actx->channels;
		audio->bitsPerSample = 16; //av_get_bytes_per_sample(p->actx->sample_fmt) * 4;
	}
	if (audio->sourceSamples != audio->samplesPerSec)
		serprintf("sample_rate changed! %d\r\n", audio->sourceSamples);
	if (audio->sourceChannels != audio->channels)
		serprintf("channels    changed! %d\r\n", audio->channels);

	p->aframe = av_frame_alloc();
	
	p->asamples = (SHORT*)amalloc(    MAX_AUDIO_FRAME_SIZE);
	p->bsamples = (SHORT*)amalloc(2 * MAX_AUDIO_FRAME_SIZE);

	p->open   = 1;
	p->play   = 0;
	p->ignore = 0;
	p->inbuf_residual = 0;
	
	return 0;

ErrorExit:	
	// Close the codec
	if ( p->actx ) {
		avcodec_close( p->actx );
		av_free( p->actx );
	}
	if( p->aparser )
		av_parser_close( p->aparser );

	return 1;
}

static int ffmpeg_audio_codec_close( AUDIO_PROPERTIES *audio )
{
	PRIV *p = (PRIV*)audio->priv;

DBGS serprintf( "stream_dec_audio_close_FFMPEG\r\n");
	if( !p || !p->open ) {
serprintf("ffad not open!\r\n");
		return 1;
	}
 
	// Close the codec
	if( p->actx ) {
		avcodec_close( p->actx );
		av_free( p->actx );
	}
	if( p->aparser )
		av_parser_close( p->aparser );

	av_free(p->aframe);

	if( p->asamples ) {
		afree( p->asamples );
	}
	if( p->bsamples ) {
		afree( p->bsamples );
	}
	p->open = 0;

	return 0;
}

static unsigned short chk16( UCHAR *data, int size ) 
{
	unsigned short chk = 0; int i; for(i = 0; i < size; i++ ) { chk += *(data++); } return chk;
}

static int convert_to_stereo( PRIV *p, AVFrame *frame, UCHAR **pcm_data, int *out_channels, int *out_bits)
{
	int bps     = av_get_bytes_per_sample(p->actx->sample_fmt) * 8;
	int samples = frame->nb_samples;
	UCHAR *data = frame->data[0];
	
	// convert all to stereo S16!
	// fixme: use the actx->channel_layout to crate a correct channel_map
	switch( p->actx->sample_fmt ) {	
	case AV_SAMPLE_FMT_FLT:
	case AV_SAMPLE_FMT_DBL:
		downmix_float( p->bsamples, data, samples, p->actx->channels, bps, channel_map ); 
		*pcm_data = (UCHAR*)p->bsamples;
		break;
		
	case AV_SAMPLE_FMT_U8:
	case AV_SAMPLE_FMT_S32:
#ifdef IS_FFMPEG
	case AV_SAMPLE_FMT_S64:
#endif
		downmix( p->bsamples, data, samples, p->actx->channels, bps, channel_map ); 
		*pcm_data = (UCHAR*)p->bsamples;
		break;
		
	case AV_SAMPLE_FMT_S16:
		if( p->actx->channels == 2 ) {
			memcpy( p->bsamples, data, samples * 2 * 2);
		} else {
			downmix( p->bsamples, data, samples, p->actx->channels, bps, channel_map ); 
		}
		*pcm_data = (UCHAR*)p->bsamples;
		break;

	case AV_SAMPLE_FMT_U8P:
	case AV_SAMPLE_FMT_S16P:
	case AV_SAMPLE_FMT_S32P:
#ifdef IS_FFMPEG
	case AV_SAMPLE_FMT_S64P:
#endif
		downmix_planar( p->bsamples, frame->data, samples, p->actx->channels, bps, channel_map ); 
		*pcm_data = (UCHAR*)p->bsamples;
		break;
	
	
	case AV_SAMPLE_FMT_FLTP:
	case AV_SAMPLE_FMT_DBLP:
		downmix_float_planar( p->bsamples, frame->data, samples, p->actx->channels, bps, channel_map ); 
		*pcm_data = (UCHAR*)p->bsamples;
		break;

	case AV_SAMPLE_FMT_NONE:
	case AV_SAMPLE_FMT_NB:
		return 0;
	}
	*out_channels = 2;
	*out_bits = 16;
	return samples * 2 * 2;
}

static inline int16_t convert_to_S16(uint8_t *src, int bits, int shift, int fmt) {
	int16_t res = 0;
	int32_t resi = 0;
	switch( fmt ) {
		case AV_SAMPLE_FMT_FLT:
		case AV_SAMPLE_FMT_FLTP:
			res = clamp(lrintf( *(float*)src * (1 << 15)));
			break;
		case AV_SAMPLE_FMT_DBL:
		case AV_SAMPLE_FMT_DBLP:
			res = clamp(lrintf( *(double*)src * (1 << 15)));
			break;
		case AV_SAMPLE_FMT_U8:
		case AV_SAMPLE_FMT_U8P:
			res = get8(src);
			break;
		case AV_SAMPLE_FMT_S16:
		case AV_SAMPLE_FMT_S16P:
			res = getS16LE(src);
			break;
		case AV_SAMPLE_FMT_S32:
		case AV_SAMPLE_FMT_S32P:
			if( bits == 16 )
				resi = getS16LE( src );
			else if ( bits == 24 )
				resi = getS24LE( src );
			else if ( bits == 32 )
				resi = getS32LE(src);
			res = clamp( resi >> shift );
			break;
	}
	return res;
}

static int convert( PRIV *p, AVFrame *frame, UCHAR **out_data, int *out_channels, int *out_bits)
{
	int out_size = 0;

	if (p->request_channels == 2) {
		out_size = convert_to_stereo(p, frame, out_data, out_channels, out_bits);
	} else {
		int bytes_per_samples = av_get_bytes_per_sample(p->actx->sample_fmt);
		int bits  = bytes_per_samples * 8;
		int shift = (bits == 32) ? 16 : (bits == 24 ) ? 8 : 0;
		uint8_t *dest = (uint8_t*) p->bsamples;

		if( p->actx->channels < p->request_channels ) {
serprintf("upmix %d -> %d\n", p->actx->channels, p->request_channels);
			if (av_sample_fmt_is_planar(p->actx->sample_fmt)) {
				int i, j;
serprintf("planar %d / %d\n", p->actx->channels, p->request_channels);
				for (i = 0; i < frame->nb_samples; ++i) {
					for (j = 0; j < p->actx->channels; ++j) {
						uint8_t *src = frame->data[j] + i * bytes_per_samples;
						int16_t res = convert_to_S16(src, bits, shift, p->actx->sample_fmt);
						memcpy(dest, &res, 2);
						dest += 2;
					}
					for (j = 0; j < (p->request_channels - p->actx->channels); ++j) {
						memset(dest, 0, 2);
						dest += 2;
					}
				}
			} else {
				int i, j;
				uint8_t *src = frame->data[0];
				for (i = 0; i < frame->nb_samples; i++) {
					for( j = 0; j < p->actx->channels; j++) {
						int16_t res = convert_to_S16(src, bits, shift, p->actx->sample_fmt);
						memcpy(dest, &res, 2);
						dest += 2;
						src += bytes_per_samples;
					}
					for (j = 0; j < (p->request_channels - p->actx->channels); j++) {
						memset(dest, 0, 2);
						dest += 2;
					}
				}
			}
			*out_channels = p->request_channels;
		} else {
			if (av_sample_fmt_is_planar(p->actx->sample_fmt)) {
				int i, j;
//serprintf("planar %d / %d\n", p->actx->channels, p->request_channels);
				for (i = 0; i < frame->nb_samples; ++i) {
					for (j = 0; j < p->actx->channels; ++j) {
						uint8_t *src = frame->data[j] + i * bytes_per_samples;
						int16_t res = convert_to_S16(src, bits, shift, p->actx->sample_fmt);
						memcpy(dest, &res, 2);
						dest += 2;
					}
				}
			} else {
				int i;
				for (i = 0; i < frame->nb_samples *  p->actx->channels; ++i) {
					int16_t res = convert_to_S16(frame->data[0] + i * bytes_per_samples, bits, shift, p->actx->sample_fmt);
					memcpy(dest, &res, 2);
					dest += 2;
				}
			}
			*out_channels = p->actx->channels;
		}

		out_size  = frame->nb_samples * 2 * *out_channels;
		*out_bits = 16; //we force output to 16 bits
		*out_data = (UCHAR*)p->bsamples;
	}
	return out_size;
}

static int ffmpeg_audio_codec_decode_raw( AUDIO_PROPERTIES *audio, UCHAR *data, int size, AUDIO_FRAME *avos_frame, int *_decoded, int *_time )
{
	PRIV *p = (PRIV*)audio->priv;
	int decoded = 0;
	UCHAR *pcm_data = (UCHAR*)p->asamples;
	
	// error until told otherwise
	if( _decoded )
		*_decoded = 0;
	if( _time )
		*_time = 0;

	memset( avos_frame, 0, sizeof( AUDIO_FRAME ) );
	avos_frame->error = 1;
	
 	if( audio->format == WAVE_FORMAT_AAC && data ) {
		// the TI decoder on the HW needs an ADIF or ADTS header, remove it here for the SIM!
		if( size >= 17 && !strncmp( data, "ADIF", 4 ) ) {
serprintf("adif");
			data    += 17;
			size    -= 17;
			decoded += 17;
		}
	}
DBGCA2 serprintf("ffad siz %6d  ", size );
DBGCA2 serprintf("CHK %04X ", chk16( data, size) );
DBGCA2 if( data ) serprintf("[%02X %02X %02X %02X]  ", data[0], data[1], data[2], data[3] );

DBGCA4 {
serprintf("\r\n");
Dump( data, 64 );
} else DBGCA5 {
serprintf("\r\n");
Dump( data, size );
}
	AVPacket avpkt = { .data = data, .size = size };
	av_init_packet(&avpkt);

	int t1 = time_update_time();
	int got_frame;
	av_frame_unref(p->aframe);
	int bytes = avcodec_decode_audio4( p->actx, p->aframe, &got_frame, &avpkt);
	if( sleep_arm ) {
		msec_sleep( sleep_arm );
	}
	int t2 = time_update_time();
	
	if ( bytes < 0 ) {
		decoded = 0;
	} else {
		decoded += bytes;
	}
		
	if( size && bytes < 0 ) {
serprintf("FFMPEG_AUDIO_DEC ERROR!\r\n");
msec_sleep( 10 );
		decoded     = size;
	//	audio_bytes = 0; 	
	}

	int channels, bits;
	int audio_bytes = convert( p, p->aframe, &pcm_data, &channels, &bits);
	int t3 = time_update_time();

DBGCA2 serprintf("dec %6d  sam %6d  byt %6d  sr %5d  ch %d|%llX  bits %d/%d  fmt %X  tim %3d/%3d\r\n", 
		decoded, p->aframe->nb_samples, audio_bytes, p->actx->sample_rate, p->actx->channels, p->actx->channel_layout, 
		p->actx->bits_per_raw_sample, av_get_bytes_per_sample(p->actx->sample_fmt) * 8, p->actx->sample_fmt, t2 - t1, t3 - t2 );
	
	avos_frame->data          = pcm_data;
	avos_frame->size          = audio_bytes;
	avos_frame->bits          = bits;
	avos_frame->channels      = channels;
	avos_frame->samplesPerSec = p->actx->sample_rate ? p->actx->sample_rate : audio->samplesPerSec;
	avos_frame->format        = WAVE_FORMAT_PCM;
	avos_frame->error         = 0;

	if( p->ignore > 0 ) {
DBGCA2 serprintf("FFMPEG IGNORE!\r\n");
		memset( avos_frame->data, 0, avos_frame->size	);
		p->ignore --;
	}
	p->play = 1;
	
	if( decoded < 0 )
		decoded = 0;
		
	if( _decoded )
		*_decoded = decoded;
	
	if( _time )
		*_time = t3 - t1;
	return 0;
}

static int fill_buffer( PRIV *p, UCHAR **data, int *size, int *decoded ) 
{
	int free = p->inbuf_size - p->inbuf_residual;
	int copy = MIN( free, *size );

	if (copy > 0) {
		memcpy(p->inbuf + p->inbuf_residual, *data, copy);
		p->inbuf_residual += copy;
		*size             -= copy;
		*data             += copy;
	}

	// report what we took from the stream 
	if ( decoded) {
		*decoded += copy;
	}
DBGCA2 serprintf("cpy %4d  buf %4d  ", copy, p->inbuf_residual ); 
DBGCA2 serprintf("[%02X %02X %02X %02X]  ", p->inbuf[0], p->inbuf[1], p->inbuf[2], p->inbuf[3] );
	return copy;
}

static void consume_buffer( PRIV *p, int consumed ) 
{
	// move fresh data to the front of the buffer and adjust fill level 
	p->inbuf_residual -= consumed;
	memmove(p->inbuf, p->inbuf + consumed, p->inbuf_residual);
}

static int ffmpeg_audio_codec_decode_parsed( AUDIO_PROPERTIES *audio, UCHAR *data, int size, AUDIO_FRAME *avos_frame, int *_decoded, int *_time )
{
	PRIV *p = (PRIV*)audio->priv;
	UCHAR *pcm_data = (UCHAR*)p->asamples;
	
	// error until told otherwise
	if( _decoded )
		*_decoded = 0;
	if( _time )
		*_time = 0;

	memset( avos_frame, 0, sizeof( AUDIO_FRAME ) );
	avos_frame->error = 1;
	
DBGCA2 serprintf("ffad siz %6d  ", size );
DBGCA3 serprintf("CHK %04X ", chk16( data, size) );
DBGCA3 if( data ) serprintf("[%02X %02X %02X %02X]  ", data[0], data[1], data[2], data[3] );

	// refill the buffer if necessary
	fill_buffer( p, &data, &size, _decoded );

	if ( p->inbuf_residual < p->inbuf_size ) {
		if( !audio->end ) {
DBGCA2 serprintf("more!\r\n");
			avos_frame->error = 0;
			return 0;
		}
	}
	
DBGCA4 {
serprintf("\r\n");
Dump( p->inbuf, 64 );
} else DBGCA5 {
serprintf("\r\n");
Dump( p->inbuf, p->inbuf_residual );
}

	unsigned char *out = NULL;
	int out_size;
	int parsed = av_parser_parse2( 	p->aparser, p->actx, 
					&out, &out_size, 
					p->inbuf, p->inbuf_residual,
					AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0 );
DBGCA2 printf("acp con %5d/%5d  out %5d  ", parsed, p->inbuf_residual, out_size );				

	if( !out || !out_size ) {
DBGCA2 serprintf("\n");
		consume_buffer( p, parsed );
		avos_frame->error = 0;
		return 0;
	}
DBGCA3 if( out ) {
serprintf("CHK %04X ", chk16( out, out_size) );
serprintf("[%02X %02X %02X %02X]  ", out[0], out[1], out[2], out[3] );
}
	switch( audio->format ) {
	case WAVE_FORMAT_MPEGLAYER3:
	case WAVE_FORMAT_MPEG:
		if( MP3_check_header( out[0], out[1], NULL, NULL, NULL ) ) {
			// ignore data that does not start with header
			consume_buffer( p, parsed );
DBGCA2 serprintf("drop %5d\n", parsed ); 
			if( p->parse_first ) {
				avos_frame->error = 0;
				p->parse_first = 0;
			}
			return 0;
		}
		break;
	}

	AVPacket avpkt = { .data = out, .size = out_size };
	av_init_packet(&avpkt);

	int t1 = time_update_time();
	int got_frame = 0;
	int bytes = avcodec_decode_audio4( p->actx, p->aframe, &got_frame, &avpkt);
	if( sleep_arm ) {
		msec_sleep( sleep_arm );
	}
	int t2 = time_update_time();
	
	if ( bytes < 0 ) {
		bytes = 0;
//		audio_bytes = 0;
	}
	
	consume_buffer( p, parsed );
		
	if( p->inbuf_residual && bytes <= 0 && !p->aframe->nb_samples ) {
serprintf("FFMPEG_AUDIO_DEC ERROR!\r\n");
msec_sleep( 10 );
		bytes = p->inbuf_residual;	
	}

	int channels, bits;
	int audio_bytes = convert( p, p->aframe, &pcm_data, &channels, &bits);
	int t3 = time_update_time();

DBGCA2 serprintf("dec %6d  sam %6d  byt %6d  sr %5d  ch %d|%llX  bits %d/%d  fmt %X  tim %3d/%3d\r\n", 
		bytes, p->aframe->nb_samples, audio_bytes, p->actx->sample_rate, p->actx->channels, p->actx->channel_layout, 
		p->actx->bits_per_raw_sample, av_get_bytes_per_sample(p->actx->sample_fmt) * 8, p->actx->sample_fmt, t2 - t1, t3 - t2 );
	
	avos_frame->data          = pcm_data;
	avos_frame->size          = audio_bytes;
	avos_frame->bits          = bits;
	avos_frame->channels      = channels;
	avos_frame->samplesPerSec = p->actx->sample_rate ? p->actx->sample_rate : audio->samplesPerSec;
	avos_frame->format        = WAVE_FORMAT_PCM;
	avos_frame->error         = 0;

	p->play = 1;
	
	if( _time )
		*_time = t3 - t1;
	return 0;
}

static int ffmpeg_audio_codec_decode( AUDIO_PROPERTIES *audio, UCHAR *data, int size, AUDIO_FRAME *avos_frame, int *_decoded, int *_time )
{
	PRIV *p = (PRIV*)audio->priv;
	if ( p->aparser ) {
		return ffmpeg_audio_codec_decode_parsed( audio, data, size, avos_frame, _decoded, _time );		
	} else {
		return ffmpeg_audio_codec_decode_raw( audio, data, size, avos_frame, _decoded, _time );		
	}
}

static int ffmpeg_audio_codec_flush( AUDIO_PROPERTIES *audio  )
{
	PRIV *p = (PRIV*)audio->priv;
DBGCA serprintf("ffad flush\r\n" );
	
	if( audio->format != WAVE_FORMAT_AAC ) {
		avcodec_flush_buffers( p->actx );
	}
	p->inbuf_residual = 0;

	if( p->aparser ) {
		// close and reinit the parser to flush it
		av_parser_close( p->aparser );
		p->aparser = av_parser_init(p->actx->codec_id);
		p->parse_first = 1;
	}
	
	if( p->play ) {
	 	p->ignore = 1;
	}

	return 0;
}

static int ffmpeg_audio_codec_delay( AUDIO_PROPERTIES *audio )
{
	return 0;
}

static int ffmpeg_audio_codec_get_rc( AUDIO_PROPERTIES *audio, STREAM_RC *rc)
{
	if( !rc )
		return 1;
	memset( rc, 0, sizeof( STREAM_RC ) );
	return 0;
}

static int ffmpeg_audio_codec_is_supported( AUDIO_PROPERTIES *audio )
{
	avcodec_register_all();
	return get_avcodec(audio) && device_config_is_audio_format_supported(audio->format) ? 1 : 0;
}

static STREAM_DEC_AUDIO stream_dec_audio_ffmpeg = 
{
	.name    = "ffmpeg",
	.new	 = ffmpeg_audio_codec_new,
	.open    = ffmpeg_audio_codec_open,
	.close   = ffmpeg_audio_codec_close,
	.decode  = ffmpeg_audio_codec_decode,
	.flush   = ffmpeg_audio_codec_flush,
	.delay   = ffmpeg_audio_codec_delay,
	.get_rc  = ffmpeg_audio_codec_get_rc,
	.delete  = ffmpeg_audio_codec_delete,
	.is_supported = ffmpeg_audio_codec_is_supported,
};

STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_LAVC, 		stream_dec_audio_ffmpeg, 8 );

#ifdef CONFIG_FF_MP3
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_MPEGLAYER3, 	stream_dec_audio_ffmpeg, 2 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_MPEG, 		stream_dec_audio_ffmpeg, 2 );
#endif
#ifdef CONFIG_FF_WMA
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_MSAUDIO2, 	stream_dec_audio_ffmpeg, 2 );
#endif
#ifdef CONFIG_FF_WMA_PRO
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_MSAUDIO3, 	stream_dec_audio_ffmpeg, 6 );
#endif
#ifdef CONFIG_FF_AAC
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_AAC, 		stream_dec_audio_ffmpeg, 8 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_AAC_LATM,	stream_dec_audio_ffmpeg, 8 );
#endif
#ifdef CONFIG_AAC_LATM
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_AAC_LATM,	stream_dec_audio_ffmpeg, 8 );
#endif
#ifdef CONFIG_FF_VORBIS
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_OGG1, 		stream_dec_audio_ffmpeg, 6 );
#endif
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_COOK, 		stream_dec_audio_ffmpeg, 2 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_FLAC, 		stream_dec_audio_ffmpeg, 6 );
#ifdef CONFIG_DTS
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_DTS, 		stream_dec_audio_ffmpeg, 8 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_DTS_HD, 		stream_dec_audio_ffmpeg, 8 );
#endif
#ifdef CONFIG_WAVPACK
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_WAVPACK,		stream_dec_audio_ffmpeg, 6 );
#endif
#ifdef CONFIG_TTA
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_TTA, 		stream_dec_audio_ffmpeg, 6 );
#endif
#ifdef CONFIG_WMA_SPEECH
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_MSAUDIO_SPEECH, 	stream_dec_audio_ffmpeg, 6 );
#endif
#ifdef CONFIG_WMA_LOSSLESS
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_MSAUDIO_LOSSLESS, stream_dec_audio_ffmpeg, 6 );
#endif
#ifdef CONFIG_FF_AMR_NB
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_VOICEAGE_AMR,	stream_dec_audio_ffmpeg, 2 );
#endif
#ifdef CONFIG_FF_AMR_WB
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_VOICEAGE_AMR_WB,	stream_dec_audio_ffmpeg, 2 );
#endif
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_AC3, 		stream_dec_audio_ffmpeg, 6 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_EAC3,		stream_dec_audio_ffmpeg, 8 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_TRUEHD,		stream_dec_audio_ffmpeg, 8 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_PCM_BLURAY,	stream_dec_audio_ffmpeg, 8 );

STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_ALAW, 		stream_dec_audio_ffmpeg, 2 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_MULAW, 		stream_dec_audio_ffmpeg, 2 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_IMA, 		stream_dec_audio_ffmpeg, 2 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_PCM, 		stream_dec_audio_ffmpeg, 8 );


#ifdef DEBUG_MSG
static STREAM_REG_DEC_AUDIO reg_aac  = { WAVE_FORMAT_AAC,        &stream_dec_audio_ffmpeg, 8 };
static STREAM_REG_DEC_AUDIO reg_latm = { WAVE_FORMAT_AAC_LATM,   &stream_dec_audio_ffmpeg, 8 };
static STREAM_REG_DEC_AUDIO reg_ac3  = { WAVE_FORMAT_AC3,        &stream_dec_audio_ffmpeg, 6 };
static STREAM_REG_DEC_AUDIO reg_mp3  = { WAVE_FORMAT_MPEGLAYER3, &stream_dec_audio_ffmpeg, 2 };
static STREAM_REG_DEC_AUDIO reg_mp2  = { WAVE_FORMAT_MPEG,       &stream_dec_audio_ffmpeg, 2 };
static STREAM_REG_DEC_AUDIO reg_wma  = { WAVE_FORMAT_MSAUDIO2,   &stream_dec_audio_ffmpeg, 2 };
static STREAM_REG_DEC_AUDIO reg_pro  = { WAVE_FORMAT_MSAUDIO3,   &stream_dec_audio_ffmpeg, 6 };

static void _reg_ffa( void ) 
{
serprintf("register lavc for AAC\r\n");
	stream_unregister_dec_audio( WAVE_FORMAT_AAC );
	stream_register_dec_audio( &reg_aac );
	stream_unregister_dec_audio( WAVE_FORMAT_AAC_LATM );
	stream_register_dec_audio( &reg_latm );
serprintf("register lavc for AC3\r\n");
	stream_unregister_dec_audio( WAVE_FORMAT_AC3 );
	stream_register_dec_audio( &reg_ac3 );
serprintf("register lavc for MP3\r\n");
	stream_unregister_dec_audio( WAVE_FORMAT_MPEGLAYER3 );
	stream_register_dec_audio( &reg_mp3 );
serprintf("register lavc for MP2\r\n");
	stream_unregister_dec_audio( WAVE_FORMAT_MPEG );
	stream_register_dec_audio( &reg_mp2 );
serprintf("register lavc for WMA\r\n");
	stream_unregister_dec_audio( WAVE_FORMAT_MSAUDIO2 );
	stream_register_dec_audio( &reg_wma );
serprintf("register lavc for WMA PRO\r\n");
	stream_unregister_dec_audio( WAVE_FORMAT_MSAUDIO3 );
	stream_register_dec_audio( &reg_pro );
}

DECLARE_DEBUG_COMMAND_VOID( "regfa", 	_reg_ffa );
DECLARE_DEBUG_PARAM       ( "ffasa", sleep_arm );
#endif

#endif // CONFIG_STREAM

#ifdef DEBUG_MSG
static void ff_cm( int argc, char **argv )
{
	int map[6] = { 0 };
	if( argc > 1 ) { 
		map[0] = atoi( argv[1] );
		if( argc > 2 ) map[1] = atoi( argv[2] );
		if( argc > 3 ) map[2] = atoi( argv[3] );
		if( argc > 4 ) map[3] = atoi( argv[4] );
		if( argc > 5 ) map[4] = atoi( argv[5] );
		if( argc > 6 ) map[5] = atoi( argv[6] );
		memcpy(channel_map, map, sizeof(channel_map));
	}
	serprintf("new channel map: %d  %d  %d  %d  %d  %d\r\n", channel_map[0], channel_map[1], channel_map[2], channel_map[3], channel_map[4], channel_map[5] );
}
DECLARE_DEBUG_COMMAND( "ffcm", ff_cm );
#endif

#endif


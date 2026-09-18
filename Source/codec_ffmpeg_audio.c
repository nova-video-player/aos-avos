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
#include "device_config.h"
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <libavutil/channel_layout.h>
#include <stdbool.h>

#ifdef CONFIG_FFMPEG_AUDIO
#ifdef CONFIG_STREAM

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>

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

static uint64_t ff_channel_layout_get_mask(const AVChannelLayout *layout)
{
	if( !layout ) {
		return 0;
	}
	if( layout->order == AV_CHANNEL_ORDER_NATIVE ) {
		return layout->u.mask;
	}

	uint64_t mask = 0;
	int i;
	for( i = 0; i < layout->nb_channels; i++ ) {
		enum AVChannel ch = av_channel_layout_channel_from_index( layout, i );
		if( ch >= 0 && ch < 63 ) {
			mask |= (1ULL << ch);
		}
	}
	return mask;
}

static void update_audio_channel_mask( AUDIO_PROPERTIES *audio, const AVChannelLayout *layout )
{
	if( !audio || !layout )
		return;
	uint64_t mask = ff_channel_layout_get_mask( layout );
	if( mask )
		audio->channelMask = (int)mask;
}

/*
 * AVCodecContext owns extradata and frees it from avcodec_free_context().
 * Stream properties do not transfer ownership, so copy their payload instead
 * of lending it to FFmpeg. This also provides FFmpeg's required zero padding.
 */
static int ffmpeg_audio_copy_extradata( AVCodecContext *ctx, const void *data, int size )
{
	if( !data || size <= 0 )
		return 0;

	ctx->extradata = av_mallocz( (size_t)size + AV_INPUT_BUFFER_PADDING_SIZE );
	if( !ctx->extradata )
		return 1;

	memcpy( ctx->extradata, data, size );
	ctx->extradata_size = size;
	return 0;
}

//
//	AUDIO
//
typedef struct PRIV {
	AVCodecContext 	*actx;
	const AVCodec 	*acodec;
	AVCodecParserContext *aparser;
	SwrContext      *swr_ctx;
	AVChannelLayout  swr_in_layout;
	AVChannelLayout  swr_out_layout;
	enum AVSampleFormat swr_in_fmt;
	int             swr_in_rate;
	AVFrame         *aframe;
	AVPacket        *avpkt;
	SHORT		*bsamples;
	int             bsamples_capacity;
	int 		open;
	int		request_channels;

	unsigned char 	inbuf[16384 + AV_INPUT_BUFFER_PADDING_SIZE];
	int draining;
	int parser_drained;
	int eos_sent;
} PRIV;

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
	audio->priv = NULL;
	return 0;
}

static const AVCodec *get_avcodec( AUDIO_PROPERTIES *audio )
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
	case WAVE_FORMAT_DTS_HD:
	case WAVE_FORMAT_DTS_HD_MA:
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
	case WAVE_FORMAT_E_AC3_JOC:
		codec_id    = AV_CODEC_ID_EAC3;
		break;
	case WAVE_FORMAT_PCM_BLURAY:
		codec_id    = AV_CODEC_ID_PCM_BLURAY;
		break;
	case WAVE_FORMAT_PCM:
		if( audio->codec_id ) {
			codec_id = audio->codec_id;
			break;
		}
		switch( audio->bitsPerSample ) {
		case 8:
			codec_id = AV_CODEC_ID_PCM_U8;
			break;
		case 24:
			codec_id = audio->byteOrder ? AV_CODEC_ID_PCM_S24BE : AV_CODEC_ID_PCM_S24LE;
			break;
		case 32:
			codec_id = audio->byteOrder ? AV_CODEC_ID_PCM_S32BE : AV_CODEC_ID_PCM_S32LE;
			break;
		case 16:
		default:
			codec_id = audio->byteOrder ? AV_CODEC_ID_PCM_S16BE : AV_CODEC_ID_PCM_S16LE;
			break;
		}
		break;
	case WAVE_FORMAT_ALAW:
		codec_id    = AV_CODEC_ID_PCM_ALAW;
		break;
	case WAVE_FORMAT_MULAW:
		codec_id    = AV_CODEC_ID_PCM_MULAW;
		break;
	case WAVE_FORMAT_OPUS:
		codec_id    = AV_CODEC_ID_OPUS;
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
	const AVCodec *acodec = get_avcodec( audio );
	if( !acodec ) {
serprintf("cannot find codec\r\n");
		return 1;
	}
	
	AVCodecContext *actx = avcodec_alloc_context3(acodec);
	if( !actx ) {
		return 1;
	}

	actx->sample_rate = audio->samplesPerSec;
	actx->block_align = audio->blockAlign;
	actx->bit_rate    = audio->bytesPerSec * 8;
	av_channel_layout_default(&actx->ch_layout, audio->channels);
	actx->ch_layout.nb_channels    = audio->channels;

	if( ffmpeg_audio_copy_extradata( actx,
			audio->extraDataSize2 ? audio->extraData2 : audio->extraData,
			audio->extraDataSize2 ? audio->extraDataSize2 : audio->extraDataSize ) )
		goto ErrorExit;

	if (avcodec_open2(actx, acodec, NULL) < 0) {
serprintf("cannot open codec\r\n");
		goto ErrorExit;
	}
	
	AVFrame *aframe = av_frame_alloc();
	AVPacket *avpkt = av_packet_alloc();
	if ( !aframe || !avpkt ) {
		goto ErrorExit2;
	}

	if (!data || size <= 0 || av_new_packet(avpkt, size) < 0)
		goto ErrorExit2;
	memcpy(avpkt->data, data, size);

	av_frame_unref(aframe);
	int ret = avcodec_send_packet(actx, avpkt);
    if (ret < 0) {
serprintf("%s: failed sending packet for decoding (%s)\n", __FUNCTION__, av_err2str(ret));
        goto ErrorExit2;
    }
	ret = avcodec_receive_frame(actx, aframe);
    if (ret < 0) {
serprintf("%s: failed receiving an audio frame from audio decoder (%s)\n", __FUNCTION__, av_err2str(ret));
        goto ErrorExit2;
    }

	if( profile )
		*profile = actx->profile;
	if( channels )
		*channels = actx->ch_layout.nb_channels;

	while ( ret >= 0) {
		// drain the decoder, should not be necessary, reusing aframe since we don't care about the data
		int ret_rx_post = avcodec_receive_frame(actx, aframe);
		if (ret_rx_post == 0) {
serprintf("%s: got an unexpected additional audio frame (%s)\n", __FUNCTION__, av_err2str(ret_rx_post));
		} else {
			break;
		}
	}
	
	av_packet_free( &avpkt );
	av_frame_free( &aframe );
	avcodec_free_context( &actx );

	return 0;

ErrorExit2:
	if ( avpkt ) {
		av_packet_free( &avpkt );
	}
	if ( aframe ) {
		av_frame_free( &aframe );
	}

ErrorExit:
	// Close the codec
	if ( actx ) {
                avcodec_free_context( &actx );
	}
	
	return 1;
}


static int ffmpeg_audio_codec_open( AUDIO_PROPERTIES *audio )
{
	PRIV *p = (PRIV*)audio->priv;
	
DBGS serprintf( "stream_dec_audio_open_FFMPEG: ");
	if (!p)
		return 1;
	memset( p, 0, sizeof( PRIV ) );


	int need_parser = 0;

	if (!device_config_is_audio_format_supported(audio->format)) {
		goto ErrorExit;
	}
	switch( audio->format ) {
	case WAVE_FORMAT_MPEGLAYER3:
	case WAVE_FORMAT_MPEG:
		need_parser = 1;		
		break;
	case WAVE_FORMAT_AAC_LATM:
		need_parser = 1;		
		break;
	case WAVE_FORMAT_AC3:
		need_parser = 1;		
		break;
	case WAVE_FORMAT_FLAC:
		need_parser = 1;		
		break;
	case WAVE_FORMAT_DTS:
		need_parser = 1;
		break;
	}
	
	p->acodec = get_avcodec( audio );
	if( !p->acodec ) {
serprintf("cannot find codec\r\n");
		goto ErrorExit;
	}
	
	p->actx = avcodec_alloc_context3(p->acodec);
	if( !p->actx ) {
		serprintf( "cannot allocate codec context\r\n" );
		goto ErrorExit;
	}
	p->request_channels = audio->request_channels;
DBGS serprintf("codec_ffmpeg_audio: audio->request_channels=%d on entry\r\n", audio->request_channels);

	// provide all the data that the decoder might need
	p->actx->sample_rate      = audio->samplesPerSec;
	p->actx->block_align      = audio->blockAlign;
	p->actx->bit_rate         = audio->bytesPerSec * 8;

	av_channel_layout_default(&p->actx->ch_layout, audio->channels);
	p->actx->ch_layout.nb_channels = audio->channels;

	char layout_desc[256];
	av_channel_layout_describe(&p->actx->ch_layout, layout_desc, sizeof(layout_desc));
DBGCA2  serprintf("requested channel layout: %s for %d channel(s)\r\n", layout_desc, p->actx->ch_layout.nb_channels);

	if( ffmpeg_audio_copy_extradata( p->actx,
			audio->extraDataSize2 ? audio->extraData2 : audio->extraData,
			audio->extraDataSize2 ? audio->extraDataSize2 : audio->extraDataSize ) ) {
		serprintf( "cannot allocate codec extradata\r\n" );
		goto ErrorExit;
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

	audio->sourceSamples = audio->samplesPerSec;
	audio->sourceChannels = audio->channels;
	audio->sourceBitsPerSample = audio->bitsPerSample;

	audio->samplesPerSec = p->actx->sample_rate;
	if (p->request_channels == 2) {
serprintf("downmix to stereo S16\r\n");
		audio->channels      = 2;
		audio->bitsPerSample = 16;
	} else {
		audio->channels = p->actx->ch_layout.nb_channels;
		// If a specific channel count was requested (e.g., 6.1 -> 5.1), use it.
		if (p->request_channels > 0 && p->request_channels != p->actx->ch_layout.nb_channels) {
			audio->channels = p->request_channels;
		}
		audio->bitsPerSample = 16; //av_get_bytes_per_sample(p->actx->sample_fmt) * 4;
	}
	update_audio_channel_mask( audio, &p->actx->ch_layout );
	if (audio->sourceSamples != audio->samplesPerSec)
		serprintf("sample_rate changed! %d\r\n", audio->sourceSamples);
	if (audio->sourceChannels != audio->channels)
		serprintf("channels    changed! %d\r\n", audio->channels);

	p->aframe = av_frame_alloc();
	p->avpkt  = av_packet_alloc();
	if( !p->aframe || !p->avpkt ) {
		goto ErrorExit;
	}
	

	p->open   = 1;
	
	return 0;

ErrorExit:
	// Close the codec
	if ( p->aframe ) {
		av_frame_free( &p->aframe );
	}
	if ( p->avpkt ) {
		av_packet_free( &p->avpkt );
	}
	if ( p->actx ) {
                avcodec_free_context( &p->actx );
		p->actx = NULL;
	}
	if( p->aparser )
		av_parser_close( p->aparser );
	p->aparser = NULL;

	if( p->swr_ctx ) {
		swr_free( &p->swr_ctx );
	}
	av_channel_layout_uninit( &p->swr_in_layout );
	av_channel_layout_uninit( &p->swr_out_layout );

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
                avcodec_free_context( &p->actx );
	}
	if( p->aparser )
		av_parser_close( p->aparser );

	if( p->swr_ctx ) {
		swr_free( &p->swr_ctx );
	}
	av_channel_layout_uninit( &p->swr_in_layout );
	av_channel_layout_uninit( &p->swr_out_layout );

	av_frame_free(&p->aframe);

	if( p->avpkt ) {
		av_packet_free(&p->avpkt);
	}

	if( p->bsamples ) {
		afree( p->bsamples );
	}


	p->open = 0;

	return 0;
}

// These decoded E-AC3/TrueHD speakers have no redistribution rules in swr's
// automatic matrix. Keep Nova's historical destinations, including CUSTOM
// SIDE_SURROUND channel IDs, without relabeling or overwriting sample planes.
static enum AVChannel downmix_channel(enum AVChannel ch)
{
	switch (ch) {
	case AV_CHAN_WIDE_LEFT: return AV_CHAN_FRONT_LEFT;
	case AV_CHAN_WIDE_RIGHT: return AV_CHAN_FRONT_RIGHT;
	case AV_CHAN_SURROUND_DIRECT_LEFT:
	case AV_CHAN_SIDE_SURROUND_LEFT: return AV_CHAN_SIDE_LEFT;
	case AV_CHAN_SURROUND_DIRECT_RIGHT:
	case AV_CHAN_SIDE_SURROUND_RIGHT: return AV_CHAN_SIDE_RIGHT;
	case AV_CHAN_LOW_FREQUENCY_2: return AV_CHAN_LOW_FREQUENCY;
	default: return ch;
	}
}

static int set_downmix_matrix(SwrContext *swr, const AVChannelLayout *input,
    const AVChannelLayout *output)
{
	// swr_build_matrix2 uses a 64x64 work area, even for smaller layouts.
	double *base = av_calloc(64 * 64, sizeof(*base));
	double matrix[8 * 64] = {0};
	AVChannelLayout canonical = {0};
	uint64_t mask = 0;
	int ret = AVERROR(ENOMEM);
	if (!base)
		return ret;
	for (int i = 0; i < input->nb_channels; i++) {
		enum AVChannel ch = downmix_channel(av_channel_layout_channel_from_index(input, i));
		if (ch < 0 || ch >= 64) {
			ret = AVERROR(EINVAL);
			goto done;
		}
		mask |= UINT64_C(1) << ch;
	}
	ret = av_channel_layout_from_mask(&canonical, mask);
	if (ret < 0)
		goto done;
	// Build unnormalized coefficients, then expand each canonical column back
	// to every original source plane. Duplicate destinations must all contribute.
	ret = swr_build_matrix2(&canonical, output, M_SQRT1_2, M_SQRT1_2,
		0.5, INT_MAX, 1.0, base, 64, AV_MATRIX_ENCODING_NONE, NULL);
	if (ret < 0)
		goto done;
	for (int i = 0; i < input->nb_channels; i++) {
		enum AVChannel ch = downmix_channel(av_channel_layout_channel_from_index(input, i));
		int col = av_channel_layout_index_from_channel(&canonical, ch);
		for (int o = 0; o < output->nb_channels; o++)
			matrix[o * 64 + i] = base[o * 64 + col];
		if (output->nb_channels == 2) {
			// Preserve the actual coefficients from downmix.c, not just similarly
			// named swr options: its automatic LFE/BC scaling and normalization
			// differ. Mono is duplicated at unity; stereo mixes clip at S16 output.
			double left = 0, right = 0;
			switch (ch) {
			case AV_CHAN_FRONT_LEFT:
			case AV_CHAN_FRONT_LEFT_OF_CENTER: left = 1.0; break;
			case AV_CHAN_FRONT_RIGHT:
			case AV_CHAN_FRONT_RIGHT_OF_CENTER: right = 1.0; break;
			case AV_CHAN_FRONT_CENTER:
			case AV_CHAN_BACK_CENTER: left = right = 0.7; break;
			case AV_CHAN_LOW_FREQUENCY: left = right = 0.5; break;
			case AV_CHAN_BACK_LEFT:
			case AV_CHAN_SIDE_LEFT: left = 0.7; break;
			case AV_CHAN_BACK_RIGHT:
			case AV_CHAN_SIDE_RIGHT: right = 0.7; break;
			default: left = matrix[i]; right = matrix[64 + i]; break;
			}
			if (input->nb_channels == 1)
				left = right = 1.0;
			matrix[i] = left;
			matrix[64 + i] = right;
		}
	}
	if (output->nb_channels != 2) {
		// Retain swr's multichannel headroom policy, including the 6.1 -> 5.1
		// path. Normalize after expansion so aliased speakers count in the sum.
		double peak = 1.0;
		for (int o = 0; o < output->nb_channels; o++) {
			double sum = 0;
			for (int i = 0; i < input->nb_channels; i++)
				sum += fabs(matrix[o * 64 + i]);
			if (sum > peak) peak = sum;
		}
		for (int o = 0; o < output->nb_channels; o++)
			for (int i = 0; i < input->nb_channels; i++)
				matrix[o * 64 + i] /= peak;
	}
	ret = swr_set_matrix(swr, matrix, 64);
done:
	av_channel_layout_uninit(&canonical);
	av_free(base);
	return ret;
}

// Match the physical PCM ordering used by AudioTrack's channel-count masks.
// libswresample handles integer/float, packed/planar and channel rematrixing.
static int convert(PRIV *p, AVFrame *frame, UCHAR **out_data,
    int *out_channels, int *out_bits)
{
	AVChannelLayout input = {0}, output = {0};
	int channels = p->request_channels > 0 ? p->request_channels : frame->ch_layout.nb_channels;
	if (channels <= 0 || channels > 8 || frame->ch_layout.nb_channels <= 0 ||
	    frame->ch_layout.nb_channels >= 64 || frame->nb_samples <= 0 || frame->sample_rate <= 0 ||
	    !frame->extended_data || !av_get_bytes_per_sample(frame->format))
		return -1;
	int planes = av_sample_fmt_is_planar(frame->format) ? frame->ch_layout.nb_channels : 1;
	for (int i = 0; i < planes; i++)
		if (!frame->extended_data[i])
			return -1;
	if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
		av_channel_layout_default(&input, frame->ch_layout.nb_channels);
	else if (av_channel_layout_copy(&input, &frame->ch_layout) < 0)
		return -1;
	if (channels == 5)
		av_channel_layout_from_mask(&output, AV_CH_LAYOUT_QUAD | AV_CH_LOW_FREQUENCY);
	else if (channels == 7)
		av_channel_layout_from_mask(&output, AV_CH_LAYOUT_6POINT1_BACK);
	else
		av_channel_layout_default(&output, channels);

	int failed = 1;
	if (!p->swr_ctx || p->swr_in_rate != frame->sample_rate ||
	    p->swr_in_fmt != frame->format ||
	    av_channel_layout_compare(&p->swr_in_layout, &input) ||
	    av_channel_layout_compare(&p->swr_out_layout, &output)) {
		swr_free(&p->swr_ctx);
		av_channel_layout_uninit(&p->swr_in_layout);
		av_channel_layout_uninit(&p->swr_out_layout);
		if (av_channel_layout_copy(&p->swr_in_layout, &input) < 0 ||
		    av_channel_layout_copy(&p->swr_out_layout, &output) < 0 ||
		    swr_alloc_set_opts2(&p->swr_ctx, &output, AV_SAMPLE_FMT_S16,
			frame->sample_rate, &input, frame->format, frame->sample_rate, 0, NULL) < 0)
			goto done;
		if (set_downmix_matrix(p->swr_ctx, &input, &output) < 0 || swr_init(p->swr_ctx) < 0) {
			swr_free(&p->swr_ctx);
			goto done;
		}
		p->swr_in_rate = frame->sample_rate;
		p->swr_in_fmt = frame->format;
	}
	int capacity = swr_get_out_samples(p->swr_ctx, frame->nb_samples);
	if (capacity < 0)
		goto done;
	int bytes = av_samples_get_buffer_size(NULL, channels, capacity, AV_SAMPLE_FMT_S16, 1);
	if (bytes < 0)
		goto done;
	if (bytes > p->bsamples_capacity) {
		SHORT *buffer = arealloc(p->bsamples, bytes);
		if (!buffer)
			goto done;
		p->bsamples = buffer;
		p->bsamples_capacity = bytes;
	}
	uint8_t *dest = (uint8_t *)p->bsamples;
	int samples = swr_convert(p->swr_ctx, &dest, capacity,
		(const uint8_t **)frame->extended_data, frame->nb_samples);
	if (samples < 0)
		goto done;
	*out_data = dest;
	*out_channels = channels;
	*out_bits = 16;
	failed = 0;
done:
	av_channel_layout_uninit(&input);
	av_channel_layout_uninit(&output);
	return failed ? -1 : samples * channels * 2;
}

static int ffmpeg_audio_output(AUDIO_PROPERTIES *audio, AUDIO_FRAME *frame)
{
	PRIV *p = audio->priv;
	int bytes = convert(p, p->aframe, &frame->data, &frame->channels, &frame->bits);
	if (bytes < 0) {
		frame->error = STREAM_ERROR_FATAL;
		av_frame_unref(p->aframe);
		return 1;
	}
	frame->size = bytes;
	frame->samplesPerSec = p->aframe->sample_rate;
	update_audio_channel_mask(audio, &p->swr_out_layout);
	av_frame_unref(p->aframe);
	return 0;
}

// Input is owned until send_packet accepts it. Receive every available frame
// before accepting more input; one compressed packet can produce many frames.
static int ffmpeg_audio_codec_decode(AUDIO_PROPERTIES *audio, UCHAR *data, int size,
    AUDIO_FRAME *frame, int *decoded, int *elapsed)
{
	PRIV *p = audio->priv;
	int start = time_update_time();
	if (sleep_arm > 0)
		msec_sleep(sleep_arm);
	int input_time = frame->time;
	*decoded = 0;
	*elapsed = 0;
	memset(frame, 0, sizeof(*frame));
	frame->format = WAVE_FORMAT_PCM;
	frame->bits = 16;
	frame->channels = audio->channels;
	frame->samplesPerSec = audio->samplesPerSec;
	if (!p || !p->open || size < 0 || (size && !data)) {
		frame->error = STREAM_ERROR_FATAL;
		return 1;
	}

	for (;;) {
		int ret = avcodec_receive_frame(p->actx, p->aframe);
		if (ret >= 0) {
			*elapsed = time_update_time() - start;
			return ffmpeg_audio_output(audio, frame);
		}
		if (ret == AVERROR_EOF)
			return STREAM_DEC_AUDIO_DRAINED;
		if (ret != AVERROR(EAGAIN)) {
			serprintf("ffmpeg audio receive failed: %s\n", av_err2str(ret));
			frame->error = 1;
			return 1;
		}

		if (!p->avpkt->size) {
			UCHAR *packet_data = data;
			int packet_size = size;
			if (p->aparser && (!p->draining || !p->parser_drained)) {
				// Parsers may read SIMD padding past their logical input. Copy a
				// bounded portion and zero its tail, including on short chunks.
				int input_size = p->draining ? 0 : MIN(size, 16384);
				if (input_size)
					memcpy(p->inbuf, data, input_size);
				memset(p->inbuf + input_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
				int used = av_parser_parse2(p->aparser, p->actx,
					&packet_data, &packet_size,
					input_size ? p->inbuf : NULL, input_size,
					AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
				if (used < 0 || used > input_size ||
				    (input_size && !used && !packet_size)) {
					frame->error = STREAM_ERROR_FATAL;
					return 1;
				}
				*decoded = used;
				if (p->draining && !packet_size)
					p->parser_drained = 1;
			} else if (p->draining) {
				packet_size = 0;
			} else {
				*decoded = size;
			}
			if (packet_size > 0) {
				if (av_new_packet(p->avpkt, packet_size) < 0) {
					frame->error = STREAM_ERROR_FATAL;
					return 1;
				}
				memcpy(p->avpkt->data, packet_data, packet_size);
				p->avpkt->pts = input_time == STREAM_NO_PTS_VALUE ? AV_NOPTS_VALUE : input_time;
			} else if (!p->draining) {
				return 0;
			}
		}

		if (p->avpkt->size) {
			int ret = avcodec_send_packet(p->actx, p->avpkt);
			if (ret == AVERROR(EAGAIN))
				return 0; // retain the access unit for the next receive/send cycle
			av_packet_unref(p->avpkt);
			if (ret < 0) {
				serprintf("ffmpeg audio send failed: %s\n", av_err2str(ret));
				frame->error = 1;
				return 1;
			}
			// This call has already accepted its input. Do not parse or queue
			// it again if the decoder needs another packet before producing PCM.
			ret = avcodec_receive_frame(p->actx, p->aframe);
			if (ret >= 0) {
				*elapsed = time_update_time() - start;
				return ffmpeg_audio_output(audio, frame);
			}
			if (ret != AVERROR(EAGAIN)) {
				frame->error = 1;
				return 1;
			}
			return 0;
		}
		if (!p->draining || p->eos_sent) {
			frame->error = STREAM_ERROR_FATAL;
			return 1;
		}
		ret = avcodec_send_packet(p->actx, NULL);
		if (ret == AVERROR(EAGAIN))
			return 0;
		if (ret < 0 && ret != AVERROR_EOF) {
			frame->error = STREAM_ERROR_FATAL;
			return 1;
		}
		p->eos_sent = 1;
	}
}

static int ffmpeg_audio_codec_drain(AUDIO_PROPERTIES *audio)
{
	PRIV *p = audio->priv;
	if (!p || !p->open)
		return 1;
	p->draining = 1;
	return 0;
}

static int ffmpeg_audio_codec_flush(AUDIO_PROPERTIES *audio)
{
	PRIV *p = audio->priv;
	avcodec_flush_buffers(p->actx);
	av_packet_unref(p->avpkt);
	av_frame_unref(p->aframe);
	p->draining = p->parser_drained = p->eos_sent = 0;
	if (p->swr_ctx)
		swr_free(&p->swr_ctx);
	if (p->aparser) {
		av_parser_close(p->aparser);
		p->aparser = av_parser_init(p->actx->codec_id);
		if (!p->aparser)
			return 1;
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
	return get_avcodec(audio) && device_config_is_audio_format_supported(audio->format) ? 1 : 0;
}

static STREAM_DEC_AUDIO stream_dec_audio_ffmpeg = 
{
	.name    = "ffmpeg",
	.new	 = ffmpeg_audio_codec_new,
	.open    = ffmpeg_audio_codec_open,
	.close   = ffmpeg_audio_codec_close,
	.decode  = ffmpeg_audio_codec_decode,
	.drain   = ffmpeg_audio_codec_drain,
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
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_DTS_HD_MA,	stream_dec_audio_ffmpeg, 8 );
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
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_E_AC3_JOC,	stream_dec_audio_ffmpeg, 8 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_TRUEHD,		stream_dec_audio_ffmpeg, 8 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_PCM_BLURAY,	stream_dec_audio_ffmpeg, 8 );

STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_ALAW, 		stream_dec_audio_ffmpeg, 2 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_MULAW, 		stream_dec_audio_ffmpeg, 2 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_IMA, 		stream_dec_audio_ffmpeg, 2 );
STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_PCM, 		stream_dec_audio_ffmpeg, 8 );

STREAM_REGISTER_DEC_AUDIO( WAVE_FORMAT_OPUS, 		stream_dec_audio_ffmpeg, 8 );

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

#endif

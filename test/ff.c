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

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "dump.h"

#define STANDALONE
#define CONFIG_VOBSUB
#define serprintf printf

#define DBGV1 if( 1 )
#define DBGV2 if( 1 )
#define DUMP_PPM

#include "../Source/vobsub.c"

static int video_stream, audio_stream, sub_stream;
int video_scale;
int video_rate;
int audio_scale;
int audio_rate;
int sub_scale;
int sub_rate;

int thread_count = 2;

VOBSUB *vobsub;
VIDEO_FRAME frame;

#define DUMP_VIDEO
#define DECODE_VIDEO

#define DUMP_AUDIO
//#define PARSE_AUDIO
//#define DECODE_AUDIO

#define DUMP_SUB
//#define DECODE_SUB

static void DumpPacketHead(const AVPacket *packet)
{
	unsigned short chk = 0;
	int i;

	if (packet->data) {
		for (i = 0; i < packet->size; i++)
			chk += packet->data[i];
	}
	printf("CHK %04X ", chk);

	if (packet->data && packet->size >= 8) {
		const unsigned char *p = packet->data;
		printf("[%02X %02X %02X %02X %02X %02X %02X %02X]  ",
		       p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
	}
}

int GetNextFrame(AVFormatContext *fmt, AVCodecContext *audio_codec, AVCodecParserContext *acp, AVCodecContext *video_codec, AVFrame *vFrame, AVFrame *aFrame)
{
	static AVPacket *packet;
	static int	eof;
	static int	flush_sent;
	static int	packet_count = 0;
	static int	frame_count = 0;
	int		ret;

	if (!packet) {
		packet = av_packet_alloc();
		if (!packet)
			return 0;
	}

	// Decode packets until we have decoded a complete frame
	while (1) {
#ifdef DECODE_VIDEO
		// Return buffered frames before feeding the decoder another packet.
		if (video_codec) {
			av_frame_unref(vFrame);
			ret = avcodec_receive_frame(video_codec, vFrame);
			if (ret == 0) {
#ifdef DUMP_VIDEO
printf(" VF [%4d] key %d  type %d  pts %8lld  \r\n", frame_count++,
							!!(vFrame->flags & AV_FRAME_FLAG_KEY),
							vFrame->pict_type,
							(long long)vFrame->best_effort_timestamp );
#endif
				return 1;
			}
			if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
				printf("Error while decoding frame: %d\r\n", ret);
				return 0;
			}
			if (ret == AVERROR_EOF)
				return 0;
		}
#endif

		if (eof) {
			// Flush the decoder once so delayed frames are not lost.
#ifdef DECODE_VIDEO
			if (video_codec && !flush_sent) {
				ret = avcodec_send_packet(video_codec, NULL);
				if (ret < 0 && ret != AVERROR_EOF) {
					printf("Error while draining decoder: %d\r\n", ret);
					return 0;
				}
				flush_sent = 1;
				continue;
			}
#endif
			return 0;
		}

		// Read the next packet, skipping all packets that aren't for this stream
		do {
			// Free old packet
			av_packet_unref(packet);

			// Read new packet
			if (av_read_frame(fmt, packet) < 0) {
				eof = 1;
				break;
			}

			if( packet->stream_index == audio_stream ) {
#ifdef DUMP_AUDIO
printf("A   [%4d]   pts/dts %8lld/%8lld  size %6d  pos %8lld  ", packet_count++,
							(long long)packet->pts * 1000 * audio_scale / audio_rate,
							(long long)packet->dts * 1000 * audio_scale / audio_rate,
							packet->size, packet->pos );
DumpPacketHead(packet);
#ifdef PARSE_AUDIO
				if( acp ) {
					unsigned char *data = packet->data;
					int            size = packet->size;
					while( size ) {
						unsigned char *out;
						int out_size;
						int len = av_parser_parse2( 	acp, audio_codec,
										&out, &out_size,
										data, size,
										packet->pts, packet->dts, packet->pos );
printf("acp %4d  out %4d  ", len, out_size );
						size -= len;
						data += len;
					}
				}
#endif
#ifdef DECODE_AUDIO
				int got_frame = 0;
				ret = 0;
				if (audio_codec && packet->size > 0) {
					av_frame_unref(aFrame);
					ret = avcodec_send_packet(audio_codec, packet);
					if (ret >= 0)
						ret = avcodec_receive_frame(audio_codec, aFrame);
					got_frame = (ret == 0);
				}
printf("dec %5d  got %d  out %5d", ret, got_frame, got_frame ? aFrame->nb_samples : 0 );
#endif
printf("\r\n" );
#endif
				av_packet_unref(packet);
			} else if( packet->stream_index == sub_stream ) {
#ifdef DUMP_SUB
printf("SUB [%4d]   pts     %8lld           size %6d  pos %8lld  ", packet_count++,
							(long long)packet->pts * 1000 * sub_scale / sub_rate,
							packet->size, packet->pos );
{ unsigned short chk = 0; const unsigned char *p = packet->data; int i; if (p) for(i = 0; i < packet->size; i++ ) { chk += *(p++); }
printf("CHK %04X ", chk );
}
//{ unsigned char *p = packet->data; printf("[%02X %02X %02X %02X]  ", *p, *(p+1), *(p+2), *(p+3) ); }
//Dump( packet->data, packet->size );
printf("[%s]", packet->data ? (char *)packet->data : "" );
printf("\r\n" );
#ifdef DECODE_SUB
	VIDEO_FRAME *f = &frame;
	VOBSUB_parse_chunk( vobsub, packet->data, packet->size, 0, &f );
#endif
#endif
				av_packet_unref(packet);
			}
		} while (packet->stream_index != video_stream);

		if (eof)
			continue;

		if( video_codec ) {
#ifdef DUMP_VIDEO
printf(" V  [%4d] %s pts/dts %8lld/%8lld  size %6d  pos %8lld  ", packet_count++, (packet->flags & AV_PKT_FLAG_KEY) ? "I" : " ",
							(long long)packet->pts * 1000 * video_scale / video_rate,
							(long long)packet->dts * 1000 * video_scale / video_rate,
							packet->size, packet->pos );
DumpPacketHead(packet);
#endif
#ifdef DECODE_VIDEO
			if (packet->size > 0) {
				ret = avcodec_send_packet(video_codec, packet);
				if (ret < 0 && ret != AVERROR(EAGAIN)) {
					printf("Error while sending packet: %d\r\n", ret);
					av_packet_unref(packet);
					return 0;
				}
				printf("send %6d", ret);
			}
#endif
			printf("\r\n");
		}
		av_packet_unref(packet);
	}
}

int ffmpeg_test( const char *name )
{
printf("[%s]\r\n\n", name );
	avformat_network_init();

	AVFormatContext *fmt = NULL;

	// Open video file
	if (avformat_open_input(&fmt, name, NULL, NULL) != 0) {
printf("cannot open file\r\n");
		return 1;
	}

	// Retrieve stream information
	if (avformat_find_stream_info(fmt, NULL) < 0) {
printf("cannot find stream info\r\n");
	}

	av_dump_format(fmt, 0, name, 0);

	AVCodecContext 	*vCodecCtx = NULL;
	AVCodecContext 	*aCodecCtx = NULL;
	const AVCodec 	*vCodec    = NULL;
	const AVCodec 	*aCodec    = NULL;
	const AVCodec 	*sCodec    = NULL;
	AVFrame 	*vFrame    = NULL;
	AVFrame 	*aFrame    = NULL;

	AVCodecParserContext *acp = NULL;

printf("streams: %d\r\n", fmt->nb_streams );
	// Find the first video stream
	video_stream = -1;
	int i;
	for (i = 0; i < fmt->nb_streams; i++)
		if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			if( fmt->streams[i]->disposition != AV_DISPOSITION_ATTACHED_PIC ) {
				video_stream = i;
				break;
			}
		}

	if (video_stream == -1) {
printf("cannot find video stream\r\n");
	} else {
		AVCodecParameters *par = fmt->streams[video_stream]->codecpar;
		video_rate  = fmt->streams[video_stream]->time_base.den;
		video_scale = fmt->streams[video_stream]->time_base.num;
printf("V: time_base %d / %d = %2.4f\r\n", video_rate, video_scale, (float)video_rate / (float)video_scale );
		// Find the decoder for the video stream
		vCodec = avcodec_find_decoder(par->codec_id);
		if (vCodec == NULL) {
printf("cannot find video codec\r\n");
		} else {
printf("V: name %s  type %d  id %04X \r\n", vCodec->name, vCodec->type, vCodec->id);
			vCodecCtx = avcodec_alloc_context3(vCodec);
			if (!vCodecCtx || avcodec_parameters_to_context(vCodecCtx, par) < 0) {
printf("cannot allocate video codec context\r\n");
				avcodec_free_context(&vCodecCtx);
			}
		}
printf("V: extradata_size %d\r\n",	  par->extradata_size);
if( par->extradata_size ) {
	Dump( par->extradata, par->extradata_size );
}
	}
printf("\r\n");

	// Find the first audio stream
	audio_stream = -1;
	for (i = 0; i < fmt->nb_streams; i++)
		if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			audio_stream = i;
			break;
		}

	if (audio_stream == -1) {
printf("cannot find audio stream\r\n");
	} else {
		// Get the codec parameters for the audio stream
		AVCodecParameters *par = fmt->streams[audio_stream]->codecpar;
		audio_rate  = fmt->streams[audio_stream]->time_base.den;
		audio_scale = fmt->streams[audio_stream]->time_base.num;
printf("a: time_base %d / %d = %2.4f\r\n", audio_rate, audio_scale, (float)audio_rate / (float)audio_scale );
printf("a: sample_rate	  %d\r\n", par->sample_rate);
printf("a: block_align	  %d\r\n", par->block_align);
printf("a: bit_rate	  %lld\r\n", (long long)par->bit_rate);
printf("a: channels	  %d\r\n", par->ch_layout.nb_channels);
printf("a: extradata_size %d\r\n",	par->extradata_size);
if( par->extradata_size ) {
//	Dump( par->extradata, par->extradata_size );
}
		// Find the decoder for the audio stream
		aCodec = avcodec_find_decoder(par->codec_id);


		if (aCodec == NULL) {
printf("cannot find audio codec\r\n");
		} else {
printf("A: name %s  type %d  id %04X \r\n", aCodec->name, aCodec->type, aCodec->id);
			aCodecCtx = avcodec_alloc_context3(aCodec);
			if (!aCodecCtx || avcodec_parameters_to_context(aCodecCtx, par) < 0) {
printf("cannot allocate audio codec context\r\n");
				avcodec_free_context(&aCodecCtx);
			}
		}


#ifdef PARSE_AUDIO
		acp = av_parser_init(par->codec_id);
		if( !acp ) {
printf("cannot init parser for audio codec id: %04X", par->codec_id );
		} else {
printf("A: got parser for: %04X", par->codec_id );
		}
#endif
	}
printf("\r\n");

	// Find the first sub stream
	sub_stream = -1;
	for (i = 3; i < fmt->nb_streams; i++)
		if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
			sub_stream = i;
			break;
		}

	if (sub_stream == -1) {
printf("cannot find sub stream\r\n");
	} else {
		// Get the codec parameters for the sub stream
		AVCodecParameters *par = fmt->streams[sub_stream]->codecpar;
		sub_rate  = fmt->streams[sub_stream]->time_base.den;
		sub_scale = fmt->streams[sub_stream]->time_base.num;
printf("s: ID             %08X\r\n", par->codec_id );
printf("s: time_base      %d / %d\r\n", fmt->streams[sub_stream]->time_base.den, fmt->streams[sub_stream]->time_base.num );
        	AVDictionaryEntry *lang = av_dict_get(fmt->streams[sub_stream]->metadata, "language", NULL,0);
		if (lang) {
printf("s: language   %s\r\n", lang->value );
		}

printf("s: extradata_size %d\r\n", par->extradata_size);
//Dump( par->extradata, par->extradata_size );
		// Find the decoder for the sub stream
		sCodec = avcodec_find_decoder(par->codec_id);

#ifdef DECODE_SUB
		vobsub = VOBSUB_create( NULL );
		frame.size = 2*720*576;
		frame.data[0] = malloc( frame.size );
		frame.colorspace = AV_IMAGE_GRAYSCALE;
		frame.linestep[0] = 720;
		frame.width    = 720;
		frame.height   = 576;
#endif
		if (sCodec == NULL) {
printf("cannot find sub codec\r\n");
		} else {
printf("S: name %s  type %d  id %d \r\n", sCodec->name, sCodec->type, sCodec->id);
		}
	}
printf("\r\n");

#ifdef DECODE_VIDEO
	if (vCodec && vCodecCtx) {
printf("open video\r\n");

		vCodecCtx->thread_count = thread_count;

		// Open codec
		if (avcodec_open2(vCodecCtx, vCodec, NULL) < 0) {
printf("cannot open video codec\r\n");
			return 1;
		}
printf("alloc frame\r\n");
		vFrame = av_frame_alloc();
	}
#endif
#ifdef DECODE_AUDIO
	if (aCodec && aCodecCtx) {
printf("open audio\r\n");
		// Open codec
		if (avcodec_open2(aCodecCtx, aCodec, NULL) < 0) {
printf("cannot open audio codec\r\n");
			return 1;
		}
		aFrame = av_frame_alloc();
	}
#endif

	int max = 10000000;
	while(max-- && GetNextFrame(fmt, aCodecCtx, acp, vCodecCtx, vFrame, aFrame)) {
//printf("key %d  type %d  line %d\r\n", vFrame->key_frame, vFrame->pict_type, vFrame->linesize[0]);
	}

#ifdef DECODE_VIDEO
	// Close the codec
	if( vCodecCtx ) {
                avcodec_free_context( &vCodecCtx );
	}
	if( vFrame )
		av_frame_free( &vFrame );
#endif
#ifdef DECODE_AUDIO
	if( aCodecCtx ) {
                avcodec_free_context( &aCodecCtx );
	}
	if( aFrame )
		av_frame_free( &aFrame );
#endif
#ifdef PARSE_AUDIO
	if( acp ) {
		av_parser_close( acp );
	}
#endif
	// Close the video file
	avformat_close_input(&fmt);

	return 0;
}

int main( int argc, char **argv )
{
	if( argc < 2 )
		return 1;

	ffmpeg_test( argv[1] );
	return 0;
}

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

int GetNextFrame(AVFormatContext *fmt, AVCodecContext *audio_codec, AVCodecParserContext *acp, AVCodecContext *video_codec, AVFrame *vFrame, AVFrame *aFrame)
{
	static AVPacket packet;
	static int 	bytesRemaining = 0;
	static uint8_t 	*rawData;
	static int 	fFirstTime = 1;
	int 		bytesDecoded;
	int 		got_frame;
	static int	packet_count = 0;
	// First time we're called, set packet.data to NULL to indicate it
	// doesn't have to be freed
	if (fFirstTime) {
		fFirstTime = 0;
		packet.data = NULL;
	}
	// Decode packets until we have decoded a complete frame
	while (1) {
		// Read the next packet, skipping all packets that aren't for this stream
		do {
			// Free old packet
			if (packet.data != NULL)
				av_free_packet(&packet);

			// Read new packet
			if (av_read_frame(fmt, &packet) < 0)
				goto loop_exit;
				
			if( packet.stream_index == audio_stream ) {
#ifdef DUMP_AUDIO
printf("A   [%4d]   pts/dts %8lld/%8lld  size %6d  pos %8lld  ", packet_count++, 
							packet.pts * 1000 * audio_scale / audio_rate, 
							packet.dts * 1000 * audio_scale / audio_rate, 
							packet.size, packet.pos );
{ unsigned short chk = 0; unsigned char *p = packet.data; int i; for(i = 0; i < packet.size; i++ ) { chk += *(p++); }
printf("CHK %04X ", chk ); 
}
{ unsigned char *p = packet.data; printf("[%02X %02X %02X %02X %02X %02X %02X %02X]  ", *p, *(p+1), *(p+2), *(p+3), *(p+4), *(p+5), *(p+6), *(p+7) ); }
#ifdef PARSE_AUDIO
				if( acp ) {
					unsigned char *data = packet.data;
					int            size = packet.size;
					while( size ) {
						unsigned char *out;
						int out_size;
						int len = av_parser_parse2( 	acp, audio_codec, 
										&out, &out_size, 
										data, size,
										packet.pts, packet.dts, packet.pos );
printf("acp %4d  out %4d  ", len, out_size );				
						size -= len;
						data += len;	
					}
				}
#endif
#ifdef DECODE_AUDIO
		//		AVPacket avpkt = { .data = data, .size = size };
		//		av_init_packet(&avpkt);

				int got_frame = 0;
				av_frame_unref(aFrame);
				int ret = avcodec_decode_audio4(audio_codec, aFrame, &got_frame, &packet);
printf("dec %5d  got %d  out %5d", ret, got_frame, aFrame->nb_samples );
#endif
printf("\r\n" );
#endif
				av_free_packet(&packet);
			} else if( packet.stream_index == sub_stream ) {
#ifdef DUMP_SUB
printf("SUB [%4d]   pts     %8lld           size %6d  pos %8lld  ", packet_count++, 
							packet.pts * 1000 * sub_scale / sub_rate, 
							packet.size, packet.pos );
{ unsigned short chk = 0; unsigned char *p = packet.data; int i; for(i = 0; i < packet.size; i++ ) { chk += *(p++); }
printf("CHK %04X ", chk ); 
}
//{ unsigned char *p = packet.data; printf("[%02X %02X %02X %02X]  ", *p, *(p+1), *(p+2), *(p+3) ); }
//Dump( packet.data, packet.size );
printf("[%s]", packet.data );
printf("\r\n" );
#ifdef DECODE_SUB
	VIDEO_FRAME *f = &frame;
	VOBSUB_parse_chunk( vobsub, packet.data, packet.size, 0, &f );
#endif
#endif
				av_free_packet(&packet);
			}
		} while (packet.stream_index != video_stream);
		bytesRemaining = packet.size;
		rawData        = packet.data;
//Dump( rawData, packet.size );

		if( video_codec ) {
#ifdef DUMP_VIDEO
printf(" V  [%4d] %s pts/dts %8lld/%8lld  size %6d  pos %8lld  ", packet_count++, packet.flags?"I":" ", 
								packet.pts * 1000 * video_scale / video_rate, 
								packet.dts * 1000 * video_scale / video_rate,
								packet.size, packet.pos );
			// Work on the current packet until we have decoded all of it
			while (bytesRemaining > 0) {
				// Decode the next chunk of data
//printf("\t\t\t\t\t\t\t\t\t\t");
{ unsigned short chk = 0; unsigned char *p = rawData; int i; for(i = 0; i < bytesRemaining; i++ ) { chk += *(p++); }
printf("CHK %04X ", chk ); 
}
{ unsigned char *p = rawData; printf("[%02X %02X %02X %02X %02X %02X %02X %02X]  ", *p, *(p+1), *(p+2), *(p+3), *(p+4), *(p+5), *(p+6), *(p+7)); }
#ifdef DECODE_VIDEO
				video_codec->reordered_opaque = packet.pts * 1000 * video_scale / video_rate;
		
				AVPacket avpkt = { .data = rawData, .size = bytesRemaining };
				av_init_packet(&avpkt);
				
				bytesDecoded = avcodec_decode_video2(video_codec, vFrame, &got_frame, &avpkt);
printf("byt %6d  got %3d  key %d  type %d  pts %6d  ", bytesDecoded, got_frame , vFrame->key_frame, vFrame->pict_type, vFrame->reordered_opaque);
//printf("line %3d %3d %3d", vFrame->linesize[0], vFrame->linesize[1], vFrame->linesize[2]);
printf("\n");
				// Was there an error?
				if (bytesDecoded < 0) {
printf("Error while decoding frame\n");
					return 0;
				}

				bytesRemaining -= bytesDecoded;
				rawData        += bytesDecoded;
#else
printf("\r\n");
				bytesRemaining = 0;
				got_frame  = 1;
#endif
				// Did we finish the current frame? Then we can return
				if (got_frame)
					return 1;
			}
#endif
		} else {
			bytesRemaining = 0;
		}
	}

loop_exit:

#ifdef DECODE_VIDEO
	if( video_codec ) {
		// Decode the rest of the last frame
		//bytesDecoded = avcodec_decode_video(video_codec, vFrame, &frameFinished, rawData, bytesRemaining);
	}
#endif	
	// Free last packet
	if (packet.data != NULL)
		av_free_packet(&packet);

	return got_frame != 0;
}

int ffmpeg_test( const char *name )
{
printf("[%s]\r\n\n", name );
	av_register_all();
	avformat_network_init();

	AVFormatContext *fmt = avformat_alloc_context();

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
	AVCodecContext 	*sCodecCtx = NULL;
	AVCodec 	*vCodec    = NULL;
	AVCodec 	*aCodec    = NULL;
	AVCodec 	*sCodec    = NULL;
	AVFrame 	*vFrame    = NULL;
	AVFrame 	*aFrame    = NULL;

	AVCodecParserContext *acp = NULL;

printf("streams: %d\r\n", fmt->nb_streams );
	// Find the first video stream
	video_stream = -1;
	int i;
	for (i = 0; i < fmt->nb_streams; i++)
		if (fmt->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			if( fmt->streams[i]->disposition != AV_DISPOSITION_ATTACHED_PIC ) {
				video_stream = i;
				break;
			}
		}

	if (video_stream == -1) {
printf("cannot find video stream\r\n");
	} else {
		vCodecCtx   = fmt->streams[video_stream]->codec;
		video_rate  = fmt->streams[video_stream]->time_base.den;
		video_scale = fmt->streams[video_stream]->time_base.num;
printf("V: time_base %d / %d = %2.4f\r\n", video_rate, video_scale, (float)video_rate / (float)video_scale );
		// Find the decoder for the video stream
		vCodec = avcodec_find_decoder(vCodecCtx->codec_id);
		if (vCodec == NULL) {
printf("cannot find video codec\r\n");
			vCodecCtx = NULL;
		} else {
printf("V: name %s  type %d  id %04X \r\n", vCodec->name, vCodec->type, vCodec->id);
printf("V: extradata_size %d\r\n",	  vCodecCtx->extradata_size);
if( vCodecCtx->extradata_size ) {
	Dump( vCodecCtx->extradata, vCodecCtx->extradata_size );
}
		}
	}
printf("\r\n");

	// Find the first audio stream
	audio_stream = -1;
	for (i = 0; i < fmt->nb_streams; i++)
		if (fmt->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
			audio_stream = i;
			break;
		}

	if (audio_stream == -1) {
printf("cannot find audio stream\r\n");
	} else {
		// Get a pointer to the codec context for the audio stream
		aCodecCtx   = fmt->streams[audio_stream]->codec;
		audio_rate  = fmt->streams[audio_stream]->time_base.den;
		audio_scale = fmt->streams[audio_stream]->time_base.num;
printf("a: time_base %d / %d = %2.4f\r\n", audio_rate, audio_scale, (float)audio_rate / (float)audio_scale );
printf("a: sample_rate	  %d\r\n", aCodecCtx->sample_rate);
printf("a: block_align	  %d\r\n", aCodecCtx->block_align);
printf("a: bit_rate	  %d\r\n", aCodecCtx->bit_rate);
printf("a: channels	  %d\r\n", aCodecCtx->channels);
printf("a: extradata_size %d\r\n",	aCodecCtx->extradata_size);
if( aCodecCtx->extradata_size ) {
//	Dump( aCodecCtx->extradata, aCodecCtx->extradata_size );
}
		// Find the decoder for the audio stream
		aCodec = avcodec_find_decoder(aCodecCtx->codec_id);

		
		if (aCodec == NULL) {
printf("cannot find audio codec\r\n");
		} else {
printf("A: name %s  type %d  id %04X \r\n", aCodec->name, aCodec->type, aCodec->id);
		}
	
	
#ifdef PARSE_AUDIO
		acp = av_parser_init(aCodecCtx->codec_id);
		if( !acp ) {
printf("cannot init parser for audio codec id: %04X", aCodecCtx->codec_id );		
		} else {
printf("A: got parser for: %04X", aCodecCtx->codec_id );		
		}
#endif
	}
printf("\r\n");

	// Find the first sub stream
	sub_stream = -1;
	for (i = 3; i < fmt->nb_streams; i++)
		if (fmt->streams[i]->codec->codec_type == AVMEDIA_TYPE_SUBTITLE) {
			sub_stream = i;
			break;
		}

	if (sub_stream == -1) {
printf("cannot find sub stream\r\n");
	} else {
		// Get a pointer to the codec context for the sub stream
		sCodecCtx = fmt->streams[sub_stream]->codec;
		sub_rate  = fmt->streams[sub_stream]->time_base.den;
		sub_scale = fmt->streams[sub_stream]->time_base.num;
printf("s: ID             %08X\r\n", sCodecCtx->codec_id );
printf("s: time_base      %d / %d\r\n", fmt->streams[sub_stream]->time_base.den, fmt->streams[sub_stream]->time_base.num );
        	AVDictionaryEntry *lang = av_dict_get(fmt->streams[sub_stream]->metadata, "language", NULL,0);
		if (lang) {
printf("s: language   %s\r\n", lang->value );
		}

printf("s: extradata_size %d\r\n", sCodecCtx->extradata_size);
//Dump( sCodecCtx->extradata, sCodecCtx->extradata_size );
		// Find the decoder for the sub stream
		sCodec = avcodec_find_decoder(sCodecCtx->codec_id);

#ifdef DECODE_SUB
		vobsub = VOBSUB_create();
		frame.size = 2*720*576;
		frame.data = malloc( frame.size );
		frame.colorspace = IMAGE_GRAYSCALE;
		frame.linestep = 720;
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
	if (vCodec ) {
printf("open video\r\n");

		vCodecCtx->thread_count = thread_count;
		
		// Inform the codec that we can handle truncated bitstreams -- i.e.,
		// bitstreams where frame boundaries can fall in the middle of packets
		if( vCodec->capabilities & CODEC_CAP_TRUNCATED) {
printf("can handle truncated\r\n");
			vCodecCtx->flags |= CODEC_FLAG_TRUNCATED;
		}
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
	if (aCodec ) {
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
	if( vCodec ) {
		avcodec_close( vCodecCtx );
		if( vFrame )
			av_free(vFrame);
	}
#endif	
#ifdef DECODE_AUDIO
	if( aCodec ) {
		avcodec_close( aCodecCtx );
		if( aFrame )
			av_free(aFrame);
	}
#endif	
#ifdef PARSE_AUDIO
	if( acp ) {
		av_parser_close( acp );
	}
#endif	
	// Close the video file
	avformat_close_input(&fmt);
}

int main( int argc, char **argv )
{
	if( argc < 2 )
		return 1;
		
	ffmpeg_test( argv[1] );
	return 0;
}

/*
 * Copyright 2025 Courville Software
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
#include "stream_filter_audio.h"
#include "debug.h"
#include "atime.h"
#include "astdlib.h"
#include "util.h"

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>

#define DBG if(1)
#define DBG2 if(0)

struct ctx {
	AVFilterGraph *filter_graph;
	AVFilterContext *abuffer_ctx;
	AVFilterContext *aformat_in_ctx;
	AVFilterContext *dynaudnorm_ctx;
	AVFilterContext *aformat_out_ctx;
	AVFilterContext *abuffersink_ctx;
	AVFrame *in_frame;
	AVFrame *out_frame;
	int channels;
	int level;
	int nightmode;
	uint8_t channel_layout[64]; // can be cast to char* to get channel layout string
};

static const int64_t get_channel_layout_from_channels( int channels )
{
	switch( channels ) {
	case 1: return AV_CH_LAYOUT_MONO;
	case 2: return AV_CH_LAYOUT_STEREO;
	case 3: return AV_CH_LAYOUT_2POINT1;
	case 4: return AV_CH_LAYOUT_QUAD;
	case 5: return AV_CH_LAYOUT_5POINT0;
	case 6: return AV_CH_LAYOUT_5POINT1;
	case 7: return AV_CH_LAYOUT_6POINT1;
	case 8: return AV_CH_LAYOUT_7POINT1;
	default: return 0; // Invalid or unsupported channel layout
	}
}

static const int64_t get_sample_format_from_bits_per_sample( int bits_per_sample )
{
	switch( bits_per_sample ) {
	case 8: return AV_SAMPLE_FMT_U8;
	case 16: return AV_SAMPLE_FMT_S16;
	case 24: return AV_SAMPLE_FMT_S32;
	case 32: return AV_SAMPLE_FMT_FLT;
	default: return AV_SAMPLE_FMT_NONE; // Invalid or unsupported sample format
	}
}

// Helper function to setup the filter graph
static int setup_filter_graph( struct ctx *ctx )
{
	int ret;

	// Create filter graph
	ctx->filter_graph = avfilter_graph_alloc();
	if( !ctx->filter_graph ) {
		serprintf( "facom setup: error allocating filter graph\n" );
		return 1;
	}

	// setup input buffer
	const AVFilter *abuffer = avfilter_get_by_name( "abuffer" );
	ctx->abuffer_ctx = avfilter_graph_alloc_filter( ctx->filter_graph, abuffer, "src" );
	if ( !ctx->abuffer_ctx ) {
		serprintf( "facom setup: error allocating buffer source\n" );
		return -1;
	}
	char abuffer_args[256];
	snprintf( abuffer_args, sizeof( abuffer_args ), "channel_layout=%s:sample_fmt=%s:time_base=%d/%d:sample_rate=%d",
			  (char *)ctx->channel_layout, av_get_sample_fmt_name( ctx->in_frame->format ), 1, ctx->in_frame->sample_rate,
			  ctx->in_frame->sample_rate );
	ret = avfilter_init_str( ctx->abuffer_ctx, abuffer_args );
	if( ret < 0 ) {
		serprintf( "facom setup: error initializing buffer source with args '%s': %s\n", abuffer_args, av_err2str( ret ) );
		return ret;
	} else {
		DBG serprintf( "facom setup: buffer source initialized with args '%s'\n", abuffer_args );
	}

	// dynaudnorm input needs to be in AV_SAMPLE_FMT_FLTP, use aformat to convert
	const AVFilter *aformat_in = avfilter_get_by_name( "aformat" );
	ctx->aformat_in_ctx = avfilter_graph_alloc_filter( ctx->filter_graph, aformat_in, "convert_in" );
	if( !ctx->aformat_in_ctx ) {
		serprintf( "facom setup: error creating buffer aformat_in %s\n", av_err2str( ret ) );
		return -1;
	}
	char aformat_in_args[256];
	// snprintf( aformat_in_args, sizeof( aformat_in_args ), "sample_fmts=%s", av_get_sample_fmt_name( AV_SAMPLE_FMT_DBLP ));
	snprintf( aformat_in_args, sizeof( aformat_in_args ), "sample_fmts=%s:sample_rates=%d:channel_layouts=%s",
			  av_get_sample_fmt_name( AV_SAMPLE_FMT_DBLP ), ctx->in_frame->sample_rate, (char *)ctx->channel_layout );
	ret = avfilter_init_str( ctx->aformat_in_ctx, aformat_in_args );
	if( ret < 0 ) {
		serprintf( "Error initializing aformat_in filter: %s with %s\n", av_err2str( ret ), aformat_in_args );
		return ret;
	} else {
		DBG serprintf( "facom setup: aformat_in filter initialized with %s\n", aformat_in_args );
	}

	// dynaudnorm for audio normalization
	const AVFilter *dynaudnorm = avfilter_get_by_name( "dynaudnorm" );
	ctx->dynaudnorm_ctx = avfilter_graph_alloc_filter( ctx->filter_graph, dynaudnorm, "dynaudnorm" );
	if( !ctx->dynaudnorm_ctx ) {
		serprintf( "facom setup: error creating dynaudnorm filter %s\n", av_err2str( ret ) );
		return -1;
	}
	char dynaudnorm_args[128];
	snprintf( dynaudnorm_args, sizeof( dynaudnorm_args ), "f=150:g=31" );
	// TODO MARC f=1:g=1 yields 'f=1:g=1': Math result not representable test the various options here at init first before setting them with _set_param
	ret = avfilter_init_str( ctx->dynaudnorm_ctx, dynaudnorm_args );
	if( ret < 0 ) {
		serprintf( "facom setup: error initializing dynaudnorm filter with args '%s': %s\n", dynaudnorm_args, av_err2str( ret ) );
		return ret;
	} else {
		DBG serprintf( "facom setup: dynaudnorm filter initialized with args '%s'\n", dynaudnorm_args );
	}

	// recovert back to original format wit another aformat
	const AVFilter *aformat_out = avfilter_get_by_name( "aformat" );
	ctx->aformat_out_ctx = avfilter_graph_alloc_filter( ctx->filter_graph, aformat_out, "convert_out" );
	if( !ctx->aformat_out_ctx ) {
		serprintf( "facom setup: error creating buffer aformat_out %s\n", av_err2str( ret ) );
		return -1;
	}
	char aformat_out_args[256];
	// snprintf( aformat_out_args, sizeof( aformat_out_args ), "sample_fmts=%s", av_get_sample_fmt_name(ctx->in_frame->format ) );
	snprintf( aformat_out_args, sizeof( aformat_out_args ), "sample_fmts=%s:sample_rates=%d:channel_layouts=%s",
			  av_get_sample_fmt_name( ctx->in_frame->format ), ctx->in_frame->sample_rate, (char *)ctx->channel_layout );
	ret = avfilter_init_str( ctx->aformat_out_ctx, aformat_out_args );
	if( ret < 0 ) {
		serprintf( "facom: error initializing aformat_out filter: %s with %s\n", av_err2str( ret ), aformat_out_args );
		return ret;
	} else {
		DBG serprintf( "facom setup: aformat_out filter initialized with %s\n", aformat_out_args );
	}

	// output buffer sink
	const AVFilter *abuffersink = avfilter_get_by_name( "abuffersink" );
	ctx->abuffersink_ctx = avfilter_graph_alloc_filter( ctx->filter_graph, abuffersink, "sink" );
	if( !ctx->abuffersink_ctx ) {
		serprintf( "facom setup: error creating buffer sink %s\n", av_err2str( ret ) );
		return -1;
	}
	// this filter takes no options but needs to be initialized
	ret = avfilter_init_str( ctx->abuffersink_ctx, NULL );
	if( ret < 0 ) {
		serprintf( "facom setup: error initializing buffer sink %s\n", av_err2str( ret ) );
		return ret;
	}

	// connect all the filters
	ret = avfilter_link( ctx->abuffer_ctx, 0, ctx->aformat_in_ctx, 0 );
	if( ret < 0 ) {
		serprintf( "facom setup: error linking buffer source to format_in %s\n", av_err2str( ret ) );
		return ret;
	} else {
		DBG serprintf( "facom setup: buffer source linked to format_in\n" );
	}

	ret = avfilter_link( ctx->aformat_in_ctx, 0, ctx->dynaudnorm_ctx, 0 );
	if( ret < 0 ) {
		serprintf( "facom setup: error linking buffer format_in to dynaudnorm %s\n", av_err2str( ret ) );
		return ret;
	} else {
		DBG serprintf( "facom setup: buffer format_in linked to dynaudnorm\n" );
	}

	ret = avfilter_link( ctx->dynaudnorm_ctx, 0, ctx->aformat_out_ctx, 0 );
	if( ret < 0 ) {
		serprintf( "facom setup: error linking dynaudnorm to buffer format_out %s\n", av_err2str( ret ) );
		return ret;
	} else {
		serprintf( "facom setup: dynaudnorm linked to buffer format_out\n" );
	}

	ret = avfilter_link( ctx->aformat_out_ctx, 0, ctx->abuffersink_ctx, 0 );
	if( ret < 0 ) {
		serprintf( "facom setup: error linking format_out to buffer sink %s\n", av_err2str( ret ) );
		return ret;
	} else {
		serprintf( "facom setup: format_out linked to buffer sink\n" );
	}

	ret = avfilter_graph_config( ctx->filter_graph, NULL );
	if( ret < 0 ) {
		char *graph_desc = avfilter_graph_dump( ctx->filter_graph, NULL );
		if( graph_desc ) {
			serprintf( "facom setup: filter graph before error:\n%s\n", graph_desc );
			av_free( graph_desc );
		}
		serprintf( "facom setup: error configuring filter graph %s\n", av_err2str( ret ) );
		return ret;
	} else {
		DBG serprintf( "facom setup: filter graph configured\n" );
		char *graph_desc = avfilter_graph_dump( ctx->filter_graph, NULL );
		if( graph_desc ) {
			DBG serprintf( "facom setup: filter graph\n%s\n", graph_desc );
			av_free( graph_desc );
		}
	}

	return 0;
}

static int _open( STREAM_FILTER_AUDIO *f, AUDIO_PROPERTIES *audio )
{
	struct ctx *ctx = acalloc( 1, sizeof( struct ctx ) );
	if( !ctx ) return 1;

	f->priv = ctx;

	ctx->channels = audio->channels;

	// Create input/output frames
	ctx->in_frame = av_frame_alloc();
	ctx->out_frame = av_frame_alloc();
	if( !ctx->in_frame || !ctx->out_frame ) {
		serprintf( "facom open: error allocating frames\n" );
		if( ctx->in_frame ) av_frame_free( &ctx->in_frame );
		if( ctx->out_frame ) av_frame_free( &ctx->out_frame );
		afree( ctx );
		return 1;
	}
	// Set frame parameters
	ctx->in_frame->format = get_sample_format_from_bits_per_sample( audio->bitsPerSample );
	ctx->in_frame->sample_rate = audio->samplesPerSec;
	// define channel layout string to be used in filter graph
	av_channel_layout_default( &ctx->in_frame->ch_layout, ctx->channels );
	int ret = av_channel_layout_describe( &ctx->in_frame->ch_layout, ctx->channel_layout, sizeof( ctx->channel_layout ) );
	if( ret < 0 ) {
		serprintf( "facom open: error describing channel layout %s\n", av_err2str( ret ) );
		return ret;
	}
	serprintf( "facom open: in frame format %s for bits_per_sample %d and ch_layout %s\n", av_get_sample_fmt_name( ctx->in_frame->format ),
			   audio->bitsPerSample, (char *)ctx->channel_layout );

	// Initialize the filter graph based on nightmode
	if( setup_filter_graph( ctx ) != 0 ) {
		serprintf( "facom open: error setting up filter graph\n" );
		return 1;
	}

	return 0;
}

static int _filter( STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame )
{
	struct ctx *ctx = f->priv;
	int ret = 0;
	// only process if nightmode or audioboost is enabled
	//if( ( ctx->level + 4 * ctx->nightmode ) > 0 ) {

	// update input frame parameters
	ctx->in_frame->nb_samples = frame->size / ( av_get_bytes_per_sample( ctx->in_frame->format ) * ctx->channels );
	ctx->in_frame->data[0] = frame->data;
	ctx->in_frame->linesize[0] = frame->size;

	// push frame into filter graph
	ret = av_buffersrc_add_frame( ctx->abuffer_ctx, ctx->in_frame );
	if( ret < 0 ) {
		serprintf( "facom filter: error adding frame to buffer source %s\n", av_err2str( ret ) );
		return ret;
	}
	// get filtered frame
	ret = av_buffersink_get_frame( ctx->abuffersink_ctx, ctx->out_frame );
	if( ret < 0 ) {
		serprintf( "facom filter: error getting frame from buffer sink %s\n", av_err2str( ret ) );
		return ret;
	}
	// copy processed data back to input frame (ensure size compatibility)
	memcpy( frame->data, ctx->out_frame->data[0], MIN( frame->size, ctx->out_frame->linesize[0] ) );
	DBG2 serprintf( "facom filter: copied %d bytes from out_frame to input frame\n", ctx->out_frame->linesize[0] );
	av_frame_unref( ctx->out_frame );
	//}
	return 0;
}

// TODO MARC loudnorm audioboost, dynaudnorm night mode
// default f=500:g=31:p=0.95:m=10.0
// recommended dynaudnorm=f=150:g=13
// dynaudnorm=f=200:g=15
// -af loudnorm=i=-18:tp=-3:lra=17
// "dynaudnorm=p=1/sqrt(2):m=100:s=12:g=15"
// dynaudnorm=framelen=200:gausssize=11:maxgain=30:targetrms=0.5:altboundary=1:compress=8:correctdc=1[int]

static int _set_param( STREAM_FILTER_AUDIO *f, void *params, void *night_on )
{
	int *level = params;
	int *nightmode = night_on;
	struct ctx *ctx = f->priv;
	int ret = 0;
	if( ( ctx->level != *level ) || ( ctx->nightmode != *nightmode ) ) {
		ctx->level = *level;
		ctx->nightmode = *nightmode;

		if( !ctx->filter_graph || !ctx->dynaudnorm_ctx ) {
			serprintf( "facom: set_param filter graph not initialized\n" );
			return -1;
		}

		if( ctx->nightmode ) {
			// Night mode compression settings
			ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "framelen", "150", NULL, 0, 0 );
			if( ret < 0 ) serprintf( "facom: set_param framelen %s\n", av_err2str( ret ) );
			ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "gausssize", "11", NULL, 0, 0 );
			if( ret < 0 ) serprintf( "facom: set_param gausssize %s\n", av_err2str( ret ) );
			ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "peak", "0.95", NULL, 0, 0 );
			if( ret < 0 ) serprintf( "facom: set_param peak %s\n", av_err2str( ret ) );
			ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "targetrms", "0.0", NULL, 0, 0 );
			if( ret < 0 ) serprintf( "facom: set_param targetrms %s\n", av_err2str( ret ) );
			if( ctx->level > 0 ) {
				// Combine night mode with boost
				ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "maxgain", "16", NULL, 0, 0 );
				if( ret < 0 ) serprintf( "facom: set_param maxgain %s\n", av_err2str( ret ) );
			} else {
				// Night mode only
				ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "maxgain", "10", NULL, 0, 0 );
				if( ret < 0 ) serprintf( "facom: set_param maxgain %s\n", av_err2str( ret ) );
			}
		} else {
			// disable night mode
			ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "framelen", "1", NULL, 0, 0 );
			if( ret < 0 ) serprintf( "facom: set_param framelen %s\n", av_err2str( ret ) );
			ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "gausssize", "1", NULL, 0, 0 );
			if( ret < 0 ) serprintf( "facom: set_param gausssize %s\n", av_err2str( ret ) );
			ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "targetrms", "0.0", NULL, 0, 0 );
			if( ret < 0 ) serprintf( "facom: set_param targetrms %s\n", av_err2str( ret ) );
			ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "peak", "1.0", NULL, 0, 0 );
			if( ret < 0 ) serprintf( "facom: set_param peak %s\n", av_err2str( ret ) );
			//avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "compress", "0", NULL, 0, 0 );
			if( ctx->level > 0 ) {
				// Boost only (no compression)
				ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "maxgain", "6", NULL, 0, 0 );
				if( ret < 0 ) serprintf( "facom: set_param maxgain %s\n", av_err2str( ret ) );
			} else {
				ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "maxgain", "0", NULL, 0, 0 );
				if( ret < 0 ) serprintf( "facom: set_param maxgain %s\n", av_err2str( ret ) );
			}
		}
	}
	serprintf( "facomp: set_param level %d nightmode %d done\n", *level, *nightmode );
	return 0;
}

static int _close( STREAM_FILTER_AUDIO *f ) {
	DBG serprintf( "facomp: close\n" );
	if( f && f->priv ){
		struct ctx *ctx = f->priv;
		if( ctx->in_frame ) av_frame_free( &ctx->in_frame );
		if( ctx->out_frame ) av_frame_free( &ctx->out_frame );
		if( ctx->filter_graph ) avfilter_graph_free( &ctx->filter_graph );
	}
	return 0;
}

int _delete( STREAM_FILTER_AUDIO *f )
{
	DBG serprintf( "facomp: delete\n" );
	if( f ) {
		if( f->priv ) {
			struct ctx *ctx = f->priv;
			if( ctx->abuffer_ctx && ctx->abuffersink_ctx ) {
				// Flush the filter graph (DO NOT do the flushing in _flush since it yields to have to recreate the filter graph by ffmpeg design)
				av_buffersrc_add_frame( ctx->abuffer_ctx, NULL );
				av_buffersink_get_frame( ctx->abuffersink_ctx, NULL );
			}
			afree( f->priv );
		}
		afree( f );
	}
	return 0;
}

static int _flush( STREAM_FILTER_AUDIO *f )
{
	// note: no real fitler graph flushing is done here, it is done in _delete since otherwise we need to recreate the filter graph
	DBG serprintf( "facomp: flush\n" );
	return 0;
}

int _delay( STREAM_FILTER_AUDIO *f ) {
	struct ctx *ctx = f->priv;
	if( ( ctx->level + 4 * ctx->nightmode ) > 0 ) {
		int64_t delay = 0;
		if( ctx && ctx->filter_graph ) {
			// get delay from dynaudnorm filter
			if( ctx->dynaudnorm_ctx ) {
				int64_t filter_delay = 0;
				int ret = av_opt_get_int( ctx->dynaudnorm_ctx, "delay", 0, &filter_delay );
				if( ret >= 0 ) {
					delay += filter_delay;
				}
			}
			// convert from samples to milliseconds
			if( ctx->in_frame->sample_rate > 0 ) {
				delay = ( delay * 1000 ) / ctx->in_frame->sample_rate;
			}
		}
		DBG serprintf( "facomp: delay %lld ms\n", delay );
		return (int)delay;
	} else {
		DBG serprintf( "facomp: no delay\n" );
		return 0;
	}
}

STREAM_FILTER_AUDIO *stream_filter_audio_compress_new( void )
{
	STREAM_FILTER_AUDIO *f = acalloc( 1, sizeof( STREAM_FILTER_AUDIO ) );

	if( !f ) return NULL;

	static char name[] = "dynaudnorm";
	f->name = name;
	f->delete = _delete;
	f->open = _open;
	f->close = _close;
	f->filter = _filter;
	f->set_param = _set_param;
	f->flush = _flush;
	f->delay = _delay;

	return f;
}

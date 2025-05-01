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
	AVFilterContext *buffersrc_ctx;
	AVFilterContext *buffereffect_ctx;
	AVFilterContext *buffersink_ctx;
	AVFrame *in_frame;
	AVFrame *out_frame;
	int sample_rate;
	int channels;
	int level;
	int nightmode;
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
	char args[512];
	int ret;

	// TODO MARC no out_ch_layout i.e. ctx->out_frame->ch_layout used (check open) was set here

	static enum AVSampleFormat out_sample_fmts[2];
	out_sample_fmts[1] = AV_SAMPLE_FMT_S16;
	out_sample_fmts[1] = -1;

	// TODO MARC change above
	// ctx->in_frame->format

	static int64_t out_channel_layouts[2];
	out_channel_layouts[0] = get_channel_layout_from_channels( ctx->channels );
	out_channel_layouts[1] = -1;

	static int out_sample_rates[2];
	out_sample_rates[0] = ctx->sample_rate;
	out_sample_rates[1] = -1;

	// Setup input buffer source
	char in_ch_layout_str[128];
	ret = av_channel_layout_describe( &ctx->in_frame->ch_layout, in_ch_layout_str, sizeof( in_ch_layout_str ) );
	if( ret < 0 ) {
		serprintf( "facom setup: error describing input channel layout: %s\n", av_err2str( ret ) );
		return ret;
	}

	snprintf( args, sizeof( args ), "sample_rate=%d:sample_fmt=%s:channel_layout=%s", ctx->sample_rate,
			  av_get_sample_fmt_name( ctx->in_frame->format ), in_ch_layout_str );

	const AVFilter *abuffer = avfilter_get_by_name( "abuffer" );

	ret = avfilter_graph_create_filter( &ctx->buffersrc_ctx, abuffer, "in", args, NULL, ctx->filter_graph );
	if( ret < 0 ) {
		serprintf( "facom setup: error creating buffer source %s\n", av_err2str( ret ) );
		return ret;
	} else {
		DBG serprintf( "facom setup: source buffer created\n" );
	}

	// Setup buffer sink
	const AVFilter *abuffersink = avfilter_get_by_name( "abuffersink" );
	ret = avfilter_graph_create_filter( &ctx->buffersink_ctx, abuffersink, "out", NULL, NULL, ctx->filter_graph );
	if( ret < 0 ) {
		serprintf( "facom setup: error creating buffer sink %s\n", av_err2str( ret ) );
		return ret;
	} else {
		DBG serprintf( "facom setup: sink buffer created\n" );
	}

	ret = av_opt_set_int_list( ctx->buffersink_ctx, "sample_fmts", out_sample_fmts, -1, AV_OPT_SEARCH_CHILDREN );
	if( ret < 0 ) {
		serprintf( "facom setup: cannot set output sample format %s\n", av_err2str( ret ) );
		return ret;
	}

	// TODO MARC according to ffmpeg documentation, the channel layout should be set in the sink filter (filter takes no option)

	// TODO MARC this is where it fails ERROR "facom setup: cannot set output channel layout Option not found"

	/*
	ret = av_opt_set_int_list( ctx->buffersink_ctx, "channel_layouts", out_channel_layouts, -1, AV_OPT_SEARCH_CHILDREN );
	if( ret < 0 ) {
		serprintf( "facom setup: cannot set output channel layout %s\n", av_err2str( ret ) );
		return ret;
	}
	*/
	ret = av_opt_set_int_list( ctx->buffersink_ctx, "sample_rates", out_sample_rates, -1, AV_OPT_SEARCH_CHILDREN );
	if( ret < 0 ) {
		serprintf( "facom setup: cannot set output sample rate %s\n", av_err2str( ret ) );
		return ret;
	}

	
	// Create dynaudnorm filter when nightmode is enabled
	const AVFilter *dynaudnorm = avfilter_get_by_name( "dynaudnorm" );
	ret = avfilter_graph_create_filter( &ctx->buffereffect_ctx, dynaudnorm, "dynaudnorm", "f=150:g=31", NULL, ctx->filter_graph );
	if( ret < 0 ) {
		serprintf( "facom setup: error creating dynaudnorm filter %s\n", av_err2str( ret ) );
		return ret;
	} else {
		DBG serprintf( "facom setup: dynaudnorm filter created\n" );
	}

	// Connect the filters with dynaudnorm
	ret = avfilter_link( ctx->buffersrc_ctx, 0, ctx->buffereffect_ctx, 0 );
	if( ret < 0 ) {
		serprintf( "facom setup: error linking buffer source to dynaudnorm %s\n", av_err2str( ret ) );
		return ret;
	} else {
		DBG serprintf( "facom setup: buffer source linked to dynaudnorm\n" );
	}

	ret = avfilter_link( ctx->buffereffect_ctx, 0, ctx->buffersink_ctx, 0 );
	if( ret < 0 ) {
		serprintf( "facom setup: error linking dynaudnorm to buffer sink %s\n", av_err2str( ret ) );
		return ret;
	} else {
		serprintf( "facom setup: dynaudnorm linked to buffer sink\n" );
	}
	

	ret = avfilter_graph_config( ctx->filter_graph, NULL );
	if( ret < 0 ) {
		serprintf( "facom setup: error configuring filter graph %s\n", av_err2str( ret ) );
		return ret;
	} else {
		DBG serprintf( "facom setup: filter graph configured\n" );
	}

	char *graph_desc = avfilter_graph_dump( ctx->filter_graph, NULL );
	if( graph_desc ) {
		DBG serprintf( "Filter graph:\n%s\n", graph_desc );
		av_free( graph_desc );
	}

	return 0;
}

static int _open( STREAM_FILTER_AUDIO *f, AUDIO_PROPERTIES *audio )
{
	struct ctx *ctx = acalloc( 1, sizeof( struct ctx ) );
	if( !ctx ) return 1;

	f->priv = ctx;

	ctx->sample_rate = audio->samplesPerSec;
	ctx->channels = audio->channels;

	// Create filter graph
	ctx->filter_graph = avfilter_graph_alloc();
	if( !ctx->filter_graph ) {
		serprintf( "facom open: error allocating filter graph\n" );
		return 1;
	}

	// Create input/output frames
	ctx->in_frame = av_frame_alloc();
	ctx->out_frame = av_frame_alloc();
	if( !ctx->in_frame || !ctx->out_frame ) {
		serprintf( "facom open: error allocating frames\n" );
		return 1;
	}

	// Set frame parameters
	ctx->in_frame->format = get_sample_format_from_bits_per_sample( audio->bitsPerSample );
	ctx->in_frame->sample_rate = ctx->sample_rate;
	ctx->in_frame->ch_layout = (AVChannelLayout){ 0 };
	av_channel_layout_default( &ctx->in_frame->ch_layout, ctx->channels );

	ctx->out_frame->format = get_sample_format_from_bits_per_sample( audio->bitsPerSample );
	ctx->out_frame->sample_rate = ctx->sample_rate;
	ctx->out_frame->ch_layout = (AVChannelLayout){ 0 };
	av_channel_layout_default( &ctx->out_frame->ch_layout, ctx->channels );

	// Initialize the filter graph based on nightmode
	if( setup_filter_graph( ctx ) != 0 ) {
		serprintf( "facom open: error setting up filter graph\n" );
		return 1;
	}

	return 0;
}

// TODO MARC follow https://ffmpeg.org/doxygen/trunk/doc_2examples_2filtering_audio_8c-example.html

static int _filter( STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame )
{
	struct ctx *ctx = f->priv;

	if( ( ctx->level + 4 * ctx->nightmode ) > 0 ) {
		// TODO MARC already configured in open!!!!
		// Setup input frame

		ctx->in_frame->nb_samples = frame->size / ( av_get_bytes_per_sample( ctx->in_frame->format ) *
													ctx->channels ); // Correct sample calculation
		ctx->in_frame->data[0] = frame->data;
		ctx->in_frame->linesize[0] = frame->size;

		// Push frame into filter graph
		int ret = av_buffersrc_add_frame( ctx->buffersrc_ctx, ctx->in_frame );
		if( ret < 0 ) return ret;
		if( ret < 0 ) {
			serprintf( "facom filter: error adding frame to buffer source %s\n", av_err2str( ret ) );
			return ret;
		} else {
			DBG2 serprintf( "facom filter: frame added to buffer source\n" );
		}

		// Get filtered frame
		ret = av_buffersink_get_frame( ctx->buffersink_ctx, ctx->out_frame );
		if( ret < 0 ) {
			serprintf( "facom filter: error getting frame from buffer sink %s\n", av_err2str( ret ) );
			return ret;
		} else {
			DBG2 serprintf( "facom filter: frame retrieved from buffer sink\n" );
		}

		// Copy processed data back to input frame (ensure size compatibility)
		memcpy( frame->data, ctx->out_frame->data[0], MIN( frame->size, ctx->out_frame->linesize[0] ) );

		av_frame_unref( ctx->out_frame );
		av_frame_free( &ctx->out_frame );  // Free the frame after using it
		ctx->out_frame = av_frame_alloc(); // Allocate a new frame for the next iteration
		if( !ctx->out_frame ) {
			// Handle allocation failure
			serprintf( "facom filter: error allocating output frame %s\n", av_err2str( ret ) );
			return -1;
		} else {
			DBG2 serprintf( "facom filter: output frame allocated\n" );
		}
	}
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

		if( ctx->nightmode ) {
			// Night mode compression settings
			ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "framelen", "150", NULL, 0, 0 );
			serprintf( "facom: set_param %s\n", av_err2str( ret ) );
			ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "gausssize", "11", NULL, 0, 0 );
			serprintf( "facom: set_param %s\n", av_err2str( ret ) );
			ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "peak", "0.95", NULL, 0, 0 );
			serprintf( "facom: set_param %s\n", av_err2str( ret ) );
			ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "targetrms", "0.0", NULL, 0, 0 );
			serprintf( "facom: set_param %s\n", av_err2str( ret ) );
			if( ctx->level > 0 ) {
				// Combine night mode with boost
				ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "maxgain", "16", NULL, 0, 0 );
				serprintf( "facom: set_param %s\n", av_err2str( ret ) );
			} else {
				// Night mode only
				ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "maxgain", "10", NULL, 0, 0 );
				serprintf( "facom: set_param %s\n", av_err2str( ret ) );
			}
		} else {
			// disable night mode
			ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "framelen", "1", NULL, 0, 0 );
			serprintf( "facom: set_param %s\n", av_err2str( ret ) );

			ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "gausssize", "1", NULL, 0, 0 );
			serprintf( "facom: set_param %s\n", av_err2str( ret ) );

			ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "targetrms", "0.0", NULL, 0, 0 );
			serprintf( "facom: set_param %s\n", av_err2str( ret ) );

			ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "peak", "1.0", NULL, 0, 0 );
			serprintf( "facom: set_param %s\n", av_err2str( ret ) );

			//avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "compress", "0", NULL, 0, 0 );
			if( ctx->level > 0 ) {
				// Boost only (no compression)
				ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "maxgain", "6", NULL, 0, 0 );
				serprintf( "facom: set_param %s\n", av_err2str( ret ) );

			} else {
				ret = avfilter_graph_send_command( ctx->filter_graph, "dynaudnorm", "maxgain", "0", NULL, 0, 0 );
				serprintf( "facom: set_param %s\n", av_err2str( ret ) );
			}
		}
	}
	serprintf( "facomp: set_param level %d nightmode %d done\n", *level, *nightmode );
	return 0;
}

static int _close( STREAM_FILTER_AUDIO *f ) { return 0; }

int _delete( STREAM_FILTER_AUDIO *f )
{
	if( f && f->priv ) {
		struct ctx *ctx = f->priv;

		av_frame_free( &ctx->in_frame );
		av_frame_free( &ctx->out_frame );
		avfilter_graph_free( &ctx->filter_graph );
		afree( ctx );
	}
	afree( f );
	return 0;
}

static int _flush( STREAM_FILTER_AUDIO *f )
{
	DBG2 serprintf("facomp: flush\n" );
	return 0;
}

int _delay( STREAM_FILTER_AUDIO *f ) { return 0; }

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
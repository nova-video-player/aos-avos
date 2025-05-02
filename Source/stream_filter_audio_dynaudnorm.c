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

	AVDictionary *options_dict = NULL;
	uint8_t options_str[1024];

	// define channel layout string to be used in filter graph
	uint8_t ch_layout[64]; // can be cast to char* to get channel layout string
	ret = av_channel_layout_describe( &ctx->in_frame->ch_layout, ch_layout, sizeof( ch_layout ) );
	if( ret < 0 ) {
		serprintf( "facom setup: error describing channel layout %s\n", av_err2str( ret ) );
		return ret;
	}

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
			  (char *)ch_layout,
			  av_get_sample_fmt_name( ctx->in_frame->format ),
			  1, ctx->in_frame->sample_rate,
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
			  av_get_sample_fmt_name( AV_SAMPLE_FMT_DBLP ), ctx->in_frame->sample_rate, (char *)ch_layout );
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
	// snprintf( aformat_out_args, sizeof( aformat_out_args ), "sample_fmts=%s", av_get_sample_fmt_name(
	// ctx->in_frame->format ) );
	snprintf( aformat_out_args, sizeof( aformat_out_args ), "sample_fmts=%s:sample_rates=%d:channel_layouts=%s",
			  av_get_sample_fmt_name( ctx->in_frame->format ), ctx->in_frame->sample_rate, (char *)ch_layout );
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
	serprintf( "facom open: in frame format %s for bits_per_sample %d\n",
			   av_get_sample_fmt_name( ctx->in_frame->format ), audio->bitsPerSample );
	ctx->in_frame->sample_rate = ctx->sample_rate;
	av_channel_layout_default( &ctx->in_frame->ch_layout, ctx->channels );

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

	if( ( ctx->level + 4 * ctx->nightmode ) > 0 ) {
		// TODO MARC already configured in open!!!!
		// Setup input frame

		ctx->in_frame->nb_samples = frame->size / ( av_get_bytes_per_sample( ctx->in_frame->format ) *
													ctx->channels ); // Correct sample calculation
		ctx->in_frame->data[0] = frame->data;
		ctx->in_frame->linesize[0] = frame->size;

		// Push frame into filter graph
		int ret = av_buffersrc_add_frame( ctx->abuffer_ctx, ctx->in_frame );
		if( ret < 0 ) return ret;
		if( ret < 0 ) {
			serprintf( "facom filter: error adding frame to buffer source %s\n", av_err2str( ret ) );
			return ret;
		} else {
			DBG2 serprintf( "facom filter: frame added to buffer source\n" );
		}

		// Get filtered frame
		ret = av_buffersink_get_frame( ctx->abuffersink_ctx, ctx->out_frame );
		if( ret < 0 ) {
			serprintf( "facom filter: error getting frame from buffer sink %s\n", av_err2str( ret ) );
			return ret;
		} else {
			DBG2 serprintf( "facom filter: frame retrieved from buffer sink\n" );
		}

		// Copy processed data back to input frame (ensure size compatibility)
		memcpy( frame->data, ctx->out_frame->data[0], MIN( frame->size, ctx->out_frame->linesize[0] ) );

		av_frame_unref( ctx->out_frame );
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

static int _close( STREAM_FILTER_AUDIO *f ) {
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
	if( f ) {
		if( f->priv ) {
			afree( f->priv );
		}
		afree( f );
	}
	return 0;
}

static int _flush( STREAM_FILTER_AUDIO *f )
{
	DBG2 serprintf( "facomp: flush\n" );
	if( f && f->priv ) {
		struct ctx *ctx = f->priv;
		if( ctx->abuffer_ctx && ctx->abuffersink_ctx ) {
			// Flush the filter graph
			av_buffersrc_add_frame( ctx->abuffer_ctx, NULL );
			av_buffersink_get_frame( ctx->abuffersink_ctx, NULL );
		}
	}
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

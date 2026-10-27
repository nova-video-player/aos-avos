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
#include "stream_sink_video.h"
#include "stream_resizer.h"
#include "stream.h"
#include "stream_alloc.h"
#include "audio_interface.h"
#include "util.h"
#include "fb.h"
#include "atime.h"
#include "astdlib.h"
#include "frame_q.h"
#include "image.h"
#include "athread.h"

#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>

#include <string.h>

#ifdef CONFIG_STREAM
#ifdef CONFIG_FB_QVFB

#include <SDL2/SDL.h>

#define DBGS 	if(Debug[DBG_STREAM])
#define DBGSI 	if(Debug[DBG_SINK])
#define DBGSI2  if(Debug[DBG_SINK] > 1)
#define DBGQ   	if(Debug[DBG_Q])

static int _sink_force_single_frame = 1;
static int _sdl_initialized = 0;

// Global SDL context that persists across stream restarts
typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
} SDL_GLOBAL_CONTEXT;

static SDL_GLOBAL_CONTEXT _sdl_global = {NULL, NULL};

#ifdef DEBUG_MSG
DECLARE_DEBUG_TOGGLE("sfsf", _sink_force_single_frame );
#endif

typedef struct SINK_PRIV {
	int		width;
	int		height;
	int 		colorspace;
	int		num_frames;
	VIDEO_FRAME	*frames[STREAM_MAX_FRAMES];

	VIDEO_FRAME	*blit_frame;

	STREAM_RSZ_PARAMS rsz;

	FRAME_Q empty;		// empty frames to be filled
	FRAME_Q full;		// full frames  to be displayed

	pthread_t 	venc_thread_handle;
	pthread_mutex_t venc_mutex;
	pthread_cond_t  venc_cond;
	int		venc_run;

	int 		venc_ref_time;
	int		venc_put_time;
	int 		venc_time;
	
	VIDEO_FRAME	*out_frame;
	
	int 		dropped;
	int 		total_dropped;

    SDL_Window *sdl_window;
    SDL_Renderer *sdl_renderer;
    SDL_Texture *sdl_texture;
    int sdl_fullscreen;
    int sdl_window_width, sdl_window_height;
    int sdl_renderer_is_gl;	// renderer owns an OpenGL/GLES context (bound to one thread at a time)

	// venc_thread drops venc_mutex while it sleeps for a frame and while it renders one.
	// _flush() runs in that window (seek) and hands every frame, including the one the thread
	// holds, back to the empty queue. These two fields keep the two from stepping on each other:
	//  - flush_gen:  bumped by _flush(). A thread that finds it changed after re-locking no longer
	//                owns its frame and must not render it or put it back on the empty queue.
	//  - rendering:  set (under the mutex) while the thread uses its frame without the mutex.
	//                _flush() waits for it to clear, so the decoder can't get the frame back and
	//                free/overwrite its AVFrame while dec->render() is still reading it.
	unsigned int	flush_gen;
	int		rendering;
} SINK_PRIV;

// An OpenGL context is current on exactly one thread. The renderer is created on the
// thread that opens the sink, but frames are drawn from venc_thread. If the opening
// thread keeps the context current, SDL_RenderCopy/Present on venc_thread fail
// (eglMakeCurrent -> EGL_BAD_ACCESS), nothing is ever presented, and on Wayland the
// window never gets mapped. So every thread releases the context when it is done and
// SDL re-binds it lazily on whichever thread draws next.
// (Metal on macOS has no such constraint: sdl_renderer_is_gl stays 0 there.)
static void sdl_release_gl_context( SINK_PRIV *p )
{
    if( p->sdl_renderer_is_gl && p->sdl_window ) {
        SDL_GL_MakeCurrent( p->sdl_window, NULL );
    }
}

// Size the window so it fits on the display (1920x1088 video on a 1080p, scaled desktop
// is bigger than the screen), keeping the video's aspect ratio.
static void sdl_fit_window_size( SINK_PRIV *p, int *out_w, int *out_h )
{
    int w = p->width  > 0 ? p->width  : 800;
    int h = p->height > 0 ? p->height : 480;
    SDL_Rect r;

    if( !p->sdl_fullscreen && SDL_GetDisplayUsableBounds( 0, &r ) == 0 && r.w > 0 && r.h > 0 ) {
        int max_w = r.w * 9 / 10;
        int max_h = r.h * 9 / 10;
        if( w > max_w || h > max_h ) {
            if( (long long)max_w * h < (long long)max_h * w ) {
                // width limited
                h = (int)( (long long)h * max_w / w );
                w = max_w;
            } else {
                // height limited
                w = (int)( (long long)w * max_h / h );
                h = max_h;
            }
        }
    }
    *out_w = w > 0 ? w : 1;
    *out_h = h > 0 ? h : 1;
}

static void blit_video_frame( SINK_PRIV *p, VIDEO_FRAME *frame )
{
    static int errors = 0;
    uint8_t *data[4];
    int linesize[4];
    const uint8_t * indata = frame->data[0];

    if (!p->sdl_texture) {
        serprintf("ERROR: blit_video_frame called but sdl_texture is NULL\n");
        return;
    }

    av_image_fill_arrays(data, linesize,
                             indata,
                             AV_PIX_FMT_BGR32,
                             p->width,
                             p->height, 1);

    if( SDL_UpdateTexture(p->sdl_texture, NULL, data[0], linesize[0]) != 0 ||
        SDL_RenderClear(p->sdl_renderer) != 0 ||
        SDL_RenderCopy(p->sdl_renderer, p->sdl_texture, NULL, NULL) != 0 ) {
        if( errors++ < 10 ) {
            serprintf("blit_video_frame: SDL error: %s\n", SDL_GetError());
        }
        return;
    }
    SDL_RenderPresent(p->sdl_renderer);
}


static int _put_time( STREAM_SINK_VIDEO *sink, int time )
{
	SINK_PRIV *p = sink->priv;
	
	p->venc_put_time = time;
	p->venc_ref_time = atime();

DBGSI2 serprintf("[[put %8d]]", time );	
	return 0;
}

static int _get_time( STREAM_SINK_VIDEO *sink )
{
	SINK_PRIV *p = sink->priv;

	int diff = atime() - p->venc_ref_time;
	p->venc_time = p->venc_put_time + diff;

	// Account for audio latency to sync video with actual audio playback
	if (sink->ctx) {
		STREAM *stream = (STREAM *)sink->ctx;
		if (stream->audio_ctx) {
			int buffer_delay = stream_get_anchor_delay_ms(stream, 1);
			// Add system audio latency (macOS typically 20-100ms)
			// This is the latency from when audio is sent to OS until it's actually heard
			int system_latency = 40;  // Adjust this value to fine-tune sync (typical: 20-100ms)
			int total_delay = buffer_delay + system_latency;
			p->venc_time -= total_delay;
		}
	}

//serprintf("get %8d + %8d = %8d\n", p->venc_put_time, diff, p->venc_time );

	return p->venc_time;
}

// ************************************************************
//
//	venc_thread
//
// ************************************************************
static void *venc_thread( void *data )
{
	STREAM_SINK_VIDEO *sink = (STREAM_SINK_VIDEO *)data;
	SINK_PRIV *p = sink->priv;
DBGS serprintf("venc_thread::Starting\r\n");	
	
	pthread_mutex_lock( &p->venc_mutex );

	while( p->venc_run ) {
		
		// get frame
		while (p->venc_run && !(p->out_frame = frame_q_get( &p->full )) ) {
			pthread_cond_wait(&p->venc_cond, &p->venc_mutex);
		}
 
		if (!p->out_frame) {
serprintf("venc_thread::stop 0\n");
			continue;
		}

		VIDEO_FRAME *f = p->out_frame;
		const unsigned int gen = p->flush_gen;	// frame belongs to us as long as this doesn't change
		int venc_time;
		int blit_duration;
RETRY:
		venc_time = _get_time(sink);
		blit_duration = f->blit_time - venc_time;


		int do_render = 1;
		if( p->venc_put_time == 1 ) {
			p->venc_put_time --;
		} else if (blit_duration > 10) {
			// too early, wait
			if( !p->venc_put_time ) {
				// put is zero, better wait a bit and retry
				pthread_mutex_unlock(&p->venc_mutex);
				serprintf("$");
				msec_sleep( 10 );
				pthread_mutex_lock( &p->venc_mutex );
				if( !p->venc_run ) {
					break;
				}
				if( p->flush_gen != gen ) {
					// flushed while we waited: the frame is back in the empty queue, not ours
					continue;
				}
				goto RETRY;
			}
DBGSI serprintf("wait %3d  f %08d  v %08d\n", blit_duration, f->blit_time, venc_time);
			pthread_mutex_unlock(&p->venc_mutex);
			msec_sleep( blit_duration - 10 );
			pthread_mutex_lock(&p->venc_mutex);
			if( !p->venc_run ) {
				break;
			}
			if( p->flush_gen != gen ) {
				// flushed while we slept: drop it, a stale frame must not be shown after a seek
				continue;
			}
		} else if( blit_duration < -100 && p->dropped < 5 ) {
			// too late, drop it
			p->dropped ++;
			p->total_dropped ++;
			do_render = 0;
DBGSI serprintf("drop %3d  f %08d  v %08d\n", blit_duration, f->blit_time, venc_time);
		}
		// From here until we re-lock, f is used without the mutex. Tell _flush() to wait for us.
		p->rendering = 1;
		pthread_mutex_unlock(&p->venc_mutex);
		if( do_render ) {
DBGSI serprintf("blit %3d\n", blit_duration);
			p->dropped = 0;
			VIDEO_FRAME *blit_f = f;
 
 			if( f->dec ) {
				STREAM_DEC_VIDEO *dec = (STREAM_DEC_VIDEO*)f->dec;
			 	if( dec->render ) {
					blit_f = p->blit_frame;
					dec->render( dec, blit_f, f );
				}
			}
 			blit_video_frame( p, blit_f );
		} else {
 			if( f->dec ) {
				STREAM_DEC_VIDEO *dec = (STREAM_DEC_VIDEO*)f->dec;
			 	if( dec->render ) {
					dec->render( dec, NULL, f );
				}
			}
		}
		pthread_mutex_lock(&p->venc_mutex);
		// _flush() could not run while rendering was set, so f is still ours: give it back.
		p->rendering = 0;
		pthread_cond_broadcast(&p->venc_cond);	// wakes a _flush() waiting for us
		frame_q_put( &p->empty, f );
		p->out_frame = NULL;
	
	}
	pthread_mutex_unlock(&p->venc_mutex);

	// hand the GL context back so sdl_close() can destroy the texture from its own thread
	sdl_release_gl_context( p );

DBGS serprintf("venc_thread::stopped\r\n");
	return NULL;
}

static int _put( STREAM_SINK_VIDEO *sink, VIDEO_FRAME *frame )
{
	SINK_PRIV *p = sink->priv;
//serprintf("put %d %d\r\n", frame->index, frame->blit_time );
	
	pthread_mutex_lock(&p->venc_mutex);
	frame_q_put( &p->full, frame );
	pthread_cond_broadcast(&p->venc_cond);
	pthread_mutex_unlock(&p->venc_mutex);

DBGQ serprintf("SINK[%d|%d] ", frame->index, frame_q_count( &p->full ) );
	return p->venc_time;
}

static int _get( STREAM_SINK_VIDEO *sink, VIDEO_FRAME **frame )
{
	SINK_PRIV *p = sink->priv;
	VIDEO_FRAME *_frame = frame_q_get( &p->empty );
	
	if( !_frame ) {
		return 1;
	}
		
//serprintf("get %d\r\n", _frame->index);
	*frame = _frame;
	return 0; 
}

static int _flush( STREAM_SINK_VIDEO *sink )
{
	SINK_PRIV *p = sink->priv;
DBGS serprintf("stream_sink_video_flush_SIM\r\n");
	pthread_mutex_lock ( &p->venc_mutex );

	// venc_thread may be rendering a frame without the mutex. Once we put every frame back on
	// the empty queue the decoder can take (and free/overwrite) it, so wait until that render
	// is done. The wait is bounded by one blit; venc_thread never waits on the caller of flush.
	while( p->rendering ) {
		pthread_cond_wait( &p->venc_cond, &p->venc_mutex );
	}
	// a frame venc_thread is merely waiting on (sleeping for its time) is taken away from it:
	p->flush_gen++;

	frame_q_flush( &p->empty );
	frame_q_flush( &p->full  );
	p->out_frame = NULL;
	
	int i;
	for( i = 0; i < p->num_frames; i++ ) {
		p->frames[i]->time = -1;	
		frame_q_put( &p->empty, p->frames[i] );	
	}

	pthread_mutex_unlock ( &p->venc_mutex );
	return 0;
}

static int alloc_frames( SINK_PRIV *p, int cached ) 
{
DBGS serprintf("stream_sink_video: alloc_frames(%d)\r\n", p->num_frames);
	int i;
	for( i = 0; i < STREAM_MAX_FRAMES; i++ ) {
		p->frames[i] = NULL;
	}

	int max = p->num_frames ? p->num_frames : STREAM_MAX_FRAMES;
	for( i = 0; i < max; i++ ) {
		if( !(p->frames[i] = frame_alloc_with_cs_and_mem( p->width, p->height, p->colorspace, _sink_force_single_frame ? STREAM_MEM_BYO : STREAM_MEM_NRM, 1 ) ) ) {
			if( p->num_frames ) {
				// we were asked for num_frames
serprintf("stream_sink_video: OOM\r\n");
				return 1;
			} else {
				// that is all we can do, stop
				break;
			}
		}
		p->frames[i]->index = i;
	}
	p->num_frames = i;
DBGS serprintf("stream_sink_video: allocated %d frames\r\n", p->num_frames);

	if( _sink_force_single_frame ) {
		// alloc blit frame:
		if( !(p->blit_frame = frame_alloc_with_cs_and_mem( p->width, p->height, p->colorspace, STREAM_MEM_NRM, 1 ) ) ) {
serprintf("stream_sink_video: cant alloc blit frame\r\n");
			return 1;
		}
	}
	
	return 0;
}

static int free_frames( SINK_PRIV *p ) 
{
	int i;
	
	for( i = 0; i < p->num_frames; i++ ) {
		if( p->frames[i] ) {
			frame_free( p->frames[i] );
		}
	}
	if( p->blit_frame ) {
		frame_free( p->blit_frame );
	}
	return 0;
}

static VIDEO_FRAME *_get_frame( STREAM_SINK_VIDEO *sink, int index )
{
	if(!sink || !sink->is_open)
		return NULL;
		
	SINK_PRIV *p = sink->priv;
	return index < p->num_frames ? p->frames[index] : NULL;
}

static int _clear( STREAM_SINK_VIDEO *sink )
{
	SINK_PRIV *p = sink->priv;
	int i;
	for( i = 0; i < p->num_frames; i++ ) {
		if( p->frames[i]->data[0] ) {
			image_full_window( (IMAGE*)p->frames[i] );
			image_fill_window( (IMAGE*)p->frames[i], YUV_BLACK );
		}
	}

	return 0;
}

static int sdl_open( STREAM_SINK_VIDEO *sink )
{
    SINK_PRIV *p = sink->priv;

    serprintf("sdl_open: initializing SDL for %dx%d\n", p->width, p->height);

    // Only initialize SDL once
    if (!_sdl_initialized) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            serprintf("SDL_Init failed: %s\n", SDL_GetError());
            return -1;
        }
        _sdl_initialized = 1;
        serprintf("sdl_open: SDL_Init done\n");
    }

    // window size that fits the display, video aspect ratio preserved
    int win_w, win_h;
    sdl_fit_window_size( p, &win_w, &win_h );
    p->sdl_window_width  = win_w;
    p->sdl_window_height = win_h;

    // Use global SDL context that persists across streams
    if (_sdl_global.window) {
        // Window already exists from previous stream
        serprintf("sdl_open: reusing global SDL window\n");
        if (p->sdl_texture) {
            SDL_DestroyTexture(p->sdl_texture);
            p->sdl_texture = NULL;
        }
        // Reuse global renderer
        p->sdl_window = _sdl_global.window;
        p->sdl_renderer = _sdl_global.renderer;
        if (!p->sdl_fullscreen) {
            SDL_SetWindowSize(p->sdl_window, win_w, win_h);
        }
    } else {
        // First run - create window
        int flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
        (p->sdl_fullscreen ? SDL_WINDOW_FULLSCREEN : 0);

        serprintf("sdl_open: creating window %dx%d (video %dx%d)\n", win_w, win_h, p->width, p->height);
        p->sdl_window = SDL_CreateWindow("nova video player (avos)",
                                         SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                         win_w, win_h, flags);
        if (!p->sdl_window) {
            serprintf("Couldn't create window: %s\n", SDL_GetError());
            return -1;
        }

        p->sdl_renderer = SDL_CreateRenderer(p->sdl_window, -1, 0);
        if (!p->sdl_renderer) {
            serprintf("Couldn't create accelerated renderer (%s), trying software\n", SDL_GetError());
            p->sdl_renderer = SDL_CreateRenderer(p->sdl_window, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!p->sdl_renderer) {
            serprintf("Couldn't create renderer: %s\n", SDL_GetError());
            SDL_DestroyWindow(p->sdl_window);
            p->sdl_window = NULL;
            return -1;
        }

        serprintf("sdl_open: window=%p, renderer=%p\n", p->sdl_window, p->sdl_renderer);

        // Store in global context
        _sdl_global.window = p->sdl_window;
        _sdl_global.renderer = p->sdl_renderer;
    }

    SDL_RendererInfo info;
    memset(&info, 0, sizeof(info));
    SDL_GetRendererInfo(p->sdl_renderer, &info);
    p->sdl_renderer_is_gl = info.name && strncmp(info.name, "opengl", 6) == 0;
    serprintf("sdl_open: video driver '%s', renderer '%s', window %dx%d\n",
              SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?",
              info.name ? info.name : "?", win_w, win_h);

    // letterbox the video into whatever size the window currently has
    SDL_RenderSetLogicalSize(p->sdl_renderer, p->width, p->height);

    // present once right away: Wayland doesn't map a window until it has a buffer
    SDL_SetRenderDrawColor(p->sdl_renderer, 0, 0, 0, 255);
    SDL_RenderClear(p->sdl_renderer);
    SDL_RenderPresent(p->sdl_renderer);

    p->sdl_texture = SDL_CreateTexture(p->sdl_renderer,
                                            SDL_PIXELFORMAT_ABGR8888,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            p->width, p->height);
    if (!p->sdl_texture) {
        serprintf("SDL_CreateTexture failed: %s\n", SDL_GetError());
        return -1;
    }

    // the venc thread is the one that draws from now on: let go of the GL context here
    sdl_release_gl_context( p );

    serprintf("sdl_open: texture=%p, SDL ready\n", p->sdl_texture);
    return 0;
}

static int _open( STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video, void *ctx, int num_frames, STREAM_RC *rc )
{
	if (sink->is_open)
		return 1;
	
	sink->ctx = ctx;
	SINK_PRIV *p = sink->priv;
	p->num_frames = num_frames + 1;
	if( p->num_frames < 8 ) {
		p->num_frames = 8;
	}

	if( p->num_frames > STREAM_MAX_FRAMES )
		return 1;
	
	sink->resize( sink, video );
DBGS serprintf("stream_sink_video_open: WxH %d x %d  num_frames %d  cached %d\r\n", p->width, p->height, p->num_frames, rc->output_cached);
	
	// allocate out frames
	if( alloc_frames( p, rc->output_cached ) ) {
		free_frames( p );
		return 1;
	}
	
	frame_q_init( &p->empty, "empty" );
	frame_q_init( &p->full,  "full"  );
	pthread_mutex_init( &p->venc_mutex, NULL );
	pthread_cond_init ( &p->venc_cond, NULL );
	
	_clear( sink );
	_flush( sink );

	// Initialize SDL BEFORE starting the venc_thread to avoid race condition
	if( sdl_open(sink) ) {
		return 1;
	}

	// lock and start the venc_thread
	pthread_mutex_lock( &p->venc_mutex );
	p->venc_run = 1;
	apthread_create( &p->venc_thread_handle, 0, venc_thread, (void*)sink, "video sink venc" );

	pthread_mutex_unlock( &p->venc_mutex );

	sink->is_open = 1;

	return 0;
}

static int sdl_close( STREAM_SINK_VIDEO *sink ) {
    SINK_PRIV *p = sink->priv;

    // Only destroy the texture - keep window and renderer for next stream
    if (p->sdl_texture) {
        SDL_DestroyTexture(p->sdl_texture);
        p->sdl_texture = NULL;
    }

    // destroying the texture re-bound the GL context to this thread; release it
    // so the next stream (possibly on another thread) can use the shared renderer
    sdl_release_gl_context( p );

    // Clear pointers but don't destroy - they're in global context
    p->sdl_renderer = NULL;
    p->sdl_window = NULL;

    serprintf("sdl_close: texture cleaned up, global SDL resources preserved\n");

    return 0;
}

static int _close( STREAM_SINK_VIDEO *sink )
{
	SINK_PRIV *p = sink->priv;

DBGS serprintf("stream_sink_video_close_SIM, dropped %d\n", p->total_dropped);
	if( !sink->is_open ) {
serprintf("ssv not open!\r\n");
		return 1;
	}

	if( p->venc_run ) {
DBGS serprintf("waiting for venc thread to join\r\n");
		pthread_mutex_lock( &p->venc_mutex );
		p->venc_run = 0;
		pthread_cond_broadcast(&p->venc_cond);
		pthread_mutex_unlock( &p->venc_mutex );
		apthread_join( p->venc_thread_handle, NULL );
DBGS serprintf("venc_thread joined\r\n");
	}
	
	pthread_mutex_destroy( &p->venc_mutex );
	pthread_cond_destroy(&p->venc_cond);

	// free the out frames
	free_frames( p );

    sdl_close(sink);

	sink->is_open = 0;
	return 0;
}

static int _delete( STREAM_SINK_VIDEO *sink )
{
DBGS serprintf("ssv: delete\r\n");
	// free the priv data
	afree( sink->priv );
	
	afree( sink );
	return 0;
}

static int _end( STREAM_SINK_VIDEO *sink )
{
	return 0;
}

static int _syncable( STREAM_SINK_VIDEO *sink )
{
	return 1;
}

static int _resize( STREAM_SINK_VIDEO *sink, VIDEO_PROPERTIES *video )
{
	SINK_PRIV *p = sink->priv;
	
	video_resizer_DSS.setup( &video_resizer_DSS, &sink->primary, video, &p->rsz );

DBGS serprintf("ssv4l2: resize: rotation: %d\r\n", p->rsz.rotation);
DBGS Rect_Dump( &p->rsz.src, "src" );
DBGS Rect_Dump( &p->rsz.dst, "dst" );
	
	p->width      = video->width;
	p->height     = video->height;
	p->colorspace = video->colorspace;

	return 0;
}

STREAM_SINK_VIDEO *stream_sink_video_SIM2_new( void ) 
{
	STREAM_SINK_VIDEO *sink = acalloc( 1, sizeof( STREAM_SINK_VIDEO ) );
	if( sink ) {
		sink->name        = "SIM2";
		sink->open        = _open;
		sink->close 	  = _close;
		sink->delete 	  = _delete;
		sink->put	  = _put;
		sink->get	  = _get;
		sink->flush	  = _flush;
		sink->end	  = _end;
		sink->syncable	  = _syncable;
		sink->get_frame   = _get_frame;
		sink->get_time	  = _get_time;
		sink->put_time	  = _put_time;
		sink->clear	  = _clear;
		sink->resize	  = _resize;
		
		sink->primary     = STREAM_SINK_DEFAULT_SCREEN;

		sink->allocates_frames = 1;
				
		// allocate private data
		if( !(sink->priv = acalloc( 1, sizeof( SINK_PRIV ) ) ) ) {
			afree( sink );
			return NULL;
		}
	}
	return sink;	
}

#endif
#endif

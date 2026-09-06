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
#include "stream.h"
#include "stream_alloc.h"
#include "stream_subtitle.h"
#include "debug.h"
#include "atime.h"
#include "util.h"
#include "sub_engine.h"
// NOTE: the subtitle engine clock is registered once in avos_mp_video_open()
// (engine_clock_cb -> stream_get_current_time) and lives for the whole stream
// session. This file no longer owns or re-registers a clock -- see the removed
// g_player_time/engine_clock and the comments in _get_next_int_sub()/_get_next_ext_sub().

#define DBGS if(Debug[DBG_STREAM])
#define DBG  if(Debug[DBG_SUB])

// s->subtitle_ext_needs_refeed is shared across threads with no lock --
// see the full rationale in stream_sub_ext.c (duplicated here rather than
// shared via a header, to avoid touching stream.h). Use acquire/release
// atomics instead of a bare read/write.
static inline void _needs_refeed_set( STREAM *s, int val )
{
	__atomic_store_n( &s->subtitle_ext_needs_refeed, val, __ATOMIC_RELEASE );
}
static inline int _needs_refeed_get( STREAM *s )
{
	return __atomic_load_n( &s->subtitle_ext_needs_refeed, __ATOMIC_ACQUIRE );
}

#ifdef CONFIG_STREAM

// -----------------------------------------------------------------------------
// Format classification helpers
//
// Three distinct cases in the pipeline:
//
//   CASE 1 — RAW PASSTHROUGH (SRT/TEXT, SSA/ASS):
//     Demuxed packet is already clean text. Feed directly to sub_engine.
//     No sub_dec needed.
//
//   CASE 2 — FFDEC-THEN-ENGINE (WEBVTT, MOV_TEXT):
//     Packet is binary-wrapped (wvtt box / tx3g atom). codec_ffsub decodes it
//     to plain text first, then that text is fed to sub_engine.
//     sub_dec IS opened, but output goes to engine not Java.
//
//   CASE 3 — FFDEC-THEN-ENGINE-BITMAP (PGS, VobSub):
//     Packet is a compressed bitmap. codec_ffsub decodes to BGRA pixels, then
//     sub_engine_feed_bitmap uploads them as an OpenGL texture.
//     sub_dec IS opened, output goes to engine bitmap path.
// -----------------------------------------------------------------------------

static inline int _is_raw_text(int fmt) {
    // Internal embedded raw passthrough: SSA/ASS and SRT/TEXT packets
    // go straight from the demuxer to sub_engine_feed with no decoding.
    // SUB_FORMAT_EXT is EXTERNAL only — never appears on internal tracks.
    return (fmt == SUB_FORMAT_SSA  ||
            fmt == SUB_FORMAT_TEXT);
}

static inline int _is_ext_text(int fmt) {
    // External text tracks: all non-bitmap external formats.
    // SUB_FORMAT_SSA here means an external .ass/.ssa file detected by
    // subtitle_ssa.c and registered by stream_sub_ext_check().
    // SUB_FORMAT_EXT covers SRT/VTT/SMI/SUB/MPL2 external files.
    return (fmt == SUB_FORMAT_SSA ||
            fmt == SUB_FORMAT_EXT);
}

static inline int _is_ffdec_text(int fmt) {
    return (fmt == SUB_FORMAT_WEBVTT  ||
            fmt == SUB_FORMAT_MOV_TEXT);
}

static inline int _is_ffdec_bitmap(int fmt) {
    return (fmt == SUB_FORMAT_PGS     ||
            fmt == SUB_FORMAT_DVD_GFX);
}

// *****************************************************************************
//
//	stream_open_sub_dec
//
// *****************************************************************************
static int stream_open_sub_dec( STREAM *s )
{
	if( s->sub_dec) {
DBGS serprintf("stream_open_sub_dec\r\n");
		// open the decoder
		if( s->sub_dec->open( s->sub_dec, s->subtitle, s ) ) {
serprintf("error opening sub_dec!\r\n");
			s->sub_dec->destroy( s->sub_dec );
			s->sub_dec = NULL;
			s->cdata_sub.valid = 0;
			return 1;
		}
		return 0;
	} 
serprintf("no sub_dec found!\r\n");
	
	return 1;
}

// *****************************************************************************
//
//	stream_close_sub_dec
//
// *****************************************************************************
void stream_close_sub_dec( STREAM *s )
{
	if( s->sub_dec) {
		s->sub_dec->close( s->sub_dec );
		s->sub_dec->destroy( s->sub_dec );
		s->sub_dec = NULL;
		s->cdata_sub.valid = 0;
	}
}

// *****************************************************************************
//
//	stream_drop_subtitles
//
// *****************************************************************************
void stream_drop_subtitles( STREAM *s )
{
DBGS serprintf("stream_drop_subtitles\r\n");
	s->av.subs_max     = 0;
	s->subtitle->valid = 0;
	if( s->buffer )
		s->buffer->subtitle = 0;
	stream_parser_clear_subtitle_chunks( s );
	s->flags |= STREAM_NO_SUBTITLES;
}

void stream_buffer_fix_subs( STREAM_BUFFER *buffer );

// *****************************************************************************
//
//	alloc_sub_frame
//
// *****************************************************************************
static void alloc_sub_frame( STREAM *s )
{
	if( !s->subtitle_frame ) {
		int cs = AV_IMAGE_BGRA_32;
		if( _is_ffdec_bitmap(s->subtitle->format) ) {
			int w = MAX( 720, s->video->width  );
			int h = MAX( 576, s->video->height );
DBG serprintf("stream_subtitle: alloc_sub_frame: %dx%d\n", w, h);
			s->subtitle_frame = frame_alloc_with_cs_and_mem( w, h, cs, STREAM_MEM_NRM, 0);
		} else {
			// Text formats (raw or ffdec): just need a large enough text buffer
			s->subtitle_frame = frame_alloc_with_cs_and_mem( 128, 8, cs, STREAM_MEM_NRM, 1);
		}
	}					 
}

// *****************************************************************************
//
//	_feed_bitmap_to_engine
//
//	Bridges a decoded BGRA VIDEO_FRAME from codec_ffsub into the C engine's
//	bitmap path (sub_format_gfx.c -> OpenGL texture upload).
//
// *****************************************************************************
static void _feed_bitmap_to_engine( STREAM *s, VIDEO_FRAME *f )
{
	if( !s->sub_engine || !f ) return;

	// Apply subtitle offset to the decoded frame time
	f->time += RST_TO_TS_DELTA(s->subtitle_offset, int);

DBG serprintf("sub int GFX->engine: video %8d  start %8d  dur %8d  [%dx%d]\r\n",
              s->video_time, f->time, f->duration, f->window.width, f->window.height);

	sub_engine_feed_bitmap(
		(SUB_ENGINE*)s->sub_engine,
		f->data[0],
		f->window.width,
		f->window.height,
		f->linestep[0],
		f->colorspace,
		f->window.x,
		f->window.y,
		f->time,
		f->duration
	);
}

// *****************************************************************************
//
//	_bridge_embedded_fonts
//
// *****************************************************************************
// Bridges the demuxer-layer ATTACHED_FONT records in s->av.font[] (harvested
// from container attachments -- e.g. MKV AVMEDIA_TYPE_ATTACHMENT streams --
// by stream_parser_ffmpeg.c's _parse_format(), see av.h) into the
// sub-engine-layer SUB_EMBEDDED_FONT shape sub_engine_open_track() expects
// (see sub_format.h). Two distinct struct types by design, not an accidental
// duplicate: av.h must not depend on anything sub-engine/libass-shaped, the
// same reasoning that already keeps SUB_PROPERTIES and SUB_FORMAT_OPEN_PARAMS
// independent (codec_private below is bridged from s->subtitle->extraData2
// exactly the same way).
//
// One shared helper instead of duplicating this loop at each of the three
// sub_engine_open_track() call sites below. `out` must have room for at
// least s->av.fonts_max entries (ATTACHED_FONT_MAX covers every caller here).
// Returns how many entries were written (only valid==1, non-empty entries
// are copied).
static int _bridge_embedded_fonts( STREAM *s, SUB_EMBEDDED_FONT *out, int out_cap )
{
	int n = 0;
	int i;
	for( i = 0; i < s->av.fonts_max && n < out_cap; i++ ) {
		ATTACHED_FONT *f = s->av.font + i;
		if( !f->valid || !f->data || f->size <= 0 ) continue;
		out[n].name = f->filename;
		out[n].data = f->data;
		out[n].size = f->size;
		n++;
	}
	return n;
}

// *****************************************************************************
//
//	_get_next_int_sub
//
// *****************************************************************************
static void _get_next_int_sub( STREAM *s, int time )
{
	if( !s->seek ) {
		int fmt = s->subtitle->format;

		// Initialize once per track
		if( !s->sub_dec && !s->subtitle_frame ) {

			if( _is_raw_text(fmt) ) {
				// CASE 1: Raw passthrough — open C engine, no sub_dec needed
				if (s->sub_engine) {
					int engine_fmt = sub_fmt_from_format(fmt);
					SUB_EMBEDDED_FONT embedded_fonts[ATTACHED_FONT_MAX];
					int embedded_fonts_count = _bridge_embedded_fonts( s, embedded_fonts, ATTACHED_FONT_MAX );
					sub_engine_open_track(
						(SUB_ENGINE*)s->sub_engine,
						engine_fmt,
						s->video ? s->video->width  : 0,
						s->video ? s->video->height : 0,
						s->subtitle->extraData2,
						s->subtitle->extraDataSize2,
						embedded_fonts, embedded_fonts_count,
						NULL); // internal track, fed synchronously off this
						       // thread only -- no checkpointed job to pin
						       // a generation token to; see sub_engine.h
					// NOTE: do NOT call sub_engine_start() here. avos_mp_video_open() already
					// registered the engine clock once (engine_clock_cb -> stream_get_current_time),
					// and that registration stays valid for the stream's entire lifetime, including
					// across track (re)opens.
				}
				// No sub_dec for raw text formats

			} else if( _is_ffdec_text(fmt) ) {
				// CASE 2: FFmpeg-decode-then-engine — open BOTH sub_dec AND C engine
				s->sub_dec = stream_get_new_dec_sub( fmt );
				if( s->sub_dec && stream_open_sub_dec( s ) ) {
					stream_drop_subtitles( s );
					return;
				}
				if (s->sub_engine) {
					// All ffdec text formats funnel through the SRT wrapper in the engine
					SUB_EMBEDDED_FONT embedded_fonts[ATTACHED_FONT_MAX];
					int embedded_fonts_count = _bridge_embedded_fonts( s, embedded_fonts, ATTACHED_FONT_MAX );
					sub_engine_open_track(
						(SUB_ENGINE*)s->sub_engine,
						SUB_FMT_SRT,
						s->video ? s->video->width  : 0,
						s->video ? s->video->height : 0,
						NULL, 0,
						embedded_fonts, embedded_fonts_count,
						NULL); // internal track -- see the CASE 1 call above
				}

			} else if( _is_ffdec_bitmap(fmt) ) {
				// CASE 3: FFmpeg-decode-then-engine-bitmap — open sub_dec, engine already
				// open (SUB_FMT_GFX track is opened by codec_ffsub's _open() directly)
				s->sub_dec = stream_get_new_dec_sub( fmt );
				if( s->sub_dec && stream_open_sub_dec( s ) ) {
					stream_drop_subtitles( s );
					return;
				}
			}

			alloc_sub_frame( s );
			
			if( !s->subtitle_frame ) {
serprintf("cannot allocate subtitle frame!\r\n");
				stream_close_sub_dec( s );
				// no subs, disable it
				stream_drop_subtitles( s );
				return;
			}
		}
		if( !s->cdata_sub.valid ) {
			if ( !s->parser->get_subtitle_cdata ) { 
				return;
			}
			if( s->parser->get_subtitle_cdata( s, &s->sub_buffer, &s->cdata_sub ) ) {
				// no subs, need to advance the sub last pos....
				stream_buffer_fix_subs( s->buffer );
				return;
			}
		}
		
		if( s->cdata_sub.valid ) {
			if( time == -1 ) return;

			if( s->cdata_sub.time == -1 || s->cdata_sub.time <= time ) {

				if( _is_raw_text(fmt) ) {
					// CASE 1: Feed raw demuxed packet straight to engine
					if (s->sub_engine) {
						int duration = 0;
						uint8_t *payload = s->sub_buffer.data;
						int payload_size = s->cdata_sub.size;

						if (payload_size >= (int)sizeof(int)) {
							duration = *(int*)payload;
							payload += sizeof(int);
							payload_size -= sizeof(int);
						}
						sub_engine_feed((SUB_ENGINE*)s->sub_engine, payload, payload_size, s->cdata_sub.time, duration);
					}
					s->cdata_sub.valid = 0;

				} else if( _is_ffdec_text(fmt) ) {
					// CASE 2: Decode binary packet to plain text, then feed engine
					VIDEO_FRAME *f = s->subtitle_frame;
					s->sub_dec->decode( s->sub_dec, s->sub_buffer.data, s->cdata_sub.size, s->cdata_sub.time, &f );
					s->cdata_sub.valid = 0;
					if( f && f->data[0] && s->sub_engine ) {
						int text_len = strlen((char*)f->data[0]);
						if( text_len > 0 ) {
							sub_engine_feed((SUB_ENGINE*)s->sub_engine, f->data[0], text_len, f->time, f->duration);
						}
					}

				} else if( _is_ffdec_bitmap(fmt) ) {
					// CASE 3: Decode bitmap packet, upload to engine as OpenGL texture
					VIDEO_FRAME *f = s->subtitle_frame;
					s->sub_dec->decode( s->sub_dec, s->sub_buffer.data, s->cdata_sub.size, s->cdata_sub.time, &f );
					s->cdata_sub.valid = 0;
					if( f ) _feed_bitmap_to_engine( s, f );
				}
			}
		}
	}
}

// *****************************************************************************
//
//    _get_next_ext_sub
//
//    Unified external subtitle pipeline — mirrors the internal embedded path.
//
//    TEXT (SRT/VTT/SMI/SUB/MPL2/ASS/SSA):
//      All cues are bulk-fed into the C engine ONCE at track open via
//      stream_sub_ext_feed_engine(). After that this function does nothing
//      for text tracks — the engine owns the timeline and renders on its
//      own render thread exactly like internal embedded tracks.
//
//    BITMAP (external VobSub IDX/SUB):
//      Still uses sub_dec + frame-by-frame gfx lookup, same as before, but
//      feeds _feed_bitmap_to_engine() instead of Java.
//
// *****************************************************************************
static void _get_next_ext_sub( STREAM *s, int time )
{
	if( s->seek ) return;

	int fmt = s->subtitle->format;

	// --- INIT BLOCK: runs once per track open ---
	if( !s->sub_dec && !s->subtitle_frame ) {

		if( _is_ffdec_bitmap(fmt) ) {
			// EXTERNAL BITMAP: open sub_dec for VobSub decode
			s->sub_dec = stream_get_new_dec_sub( fmt );
			if( s->sub_dec && stream_open_sub_dec( s ) ) {
				stream_drop_subtitles( s );
				return;
			}
		} else if( _is_ext_text(fmt) ) {
			// EXTERNAL TEXT (SRT/VTT/SMI/SUB/MPL2/ASS/SSA):
			// Open the engine track first, then bulk-feed the entire cue list.
			// engine_fmt comes from SUB_PRIV->engine_fmt[track] set at parse time.
			if( s->sub_engine ) {
				int engine_fmt = stream_sub_ext_get_engine_fmt( s );
				if( engine_fmt < 0 ) engine_fmt = SUB_FMT_SRT; // safe default

				SUB_EMBEDDED_FONT embedded_fonts[ATTACHED_FONT_MAX];
				int embedded_fonts_count = _bridge_embedded_fonts( s, embedded_fonts, ATTACHED_FONT_MAX );
				uint64_t track_gen = 0;
				sub_engine_open_track(
					(SUB_ENGINE*)s->sub_engine,
					engine_fmt,
					s->video ? s->video->width  : 0,
					s->video ? s->video->height : 0,
					NULL, 0, // no codec_private for external files
					embedded_fonts, embedded_fonts_count,
					&track_gen);

				// Pin this track's generation token to its uni_sub now, on
				// this same (selecting) thread, BEFORE stream_sub_ext_feed_engine()
				// below can enqueue its streaming feed job onto the
				// background parse-worker pool -- see
				// stream_sub_ext_set_track_generation()'s doc comment in
				// stream_sub_ext.c.
				stream_sub_ext_set_track_generation( s, track_gen );

				// Bulk-feed the full cue list now; clear the refeed flag BEFORE the call so that if a streaming feed gets interrupted and sets it back to 1, that survives for the refeed branch below to pick up.
				_needs_refeed_set( s, 0 );
				stream_sub_ext_feed_engine( s );
			}
			// No sub_dec for external text — engine owns the timeline.
		}

		alloc_sub_frame( s );

		if( !s->subtitle_frame ) {
			serprintf("cannot allocate subtitle frame!\r\n");
			stream_close_sub_dec( s );
			stream_drop_subtitles( s );
			return;
		}
	} else if( _is_ext_text(fmt) && _needs_refeed_get( s ) && s->sub_engine ) {
		// Track was already open (a seek flushed the engine's events, or a prior streaming feed was interrupted); re-run the bulk-feed without reopening the track. Uses stream_sub_ext_force_streaming_refeed() rather than stream_sub_ext_feed_engine() directly since for streaming (SRT/VTT) tracks SUBT_PARSE_DONE would otherwise be treated as "already fed" -- see its comment in stream_sub_ext.c.
DBG serprintf("_get_next_ext_sub: re-feeding external text track after seek/interrupt\r\n");
		_needs_refeed_set( s, 0 );
		stream_sub_ext_force_streaming_refeed( s );
	}

	if( time == -1 ) return;

	// --- PER-FRAME BLOCK ---
	// Text tracks: nothing to do — the engine render thread drives display.
	// Bitmap tracks: decode the current gfx cue and feed the engine.
	if( _is_ffdec_bitmap(fmt) ) {
		if( !s->sub_dec ) return;

		VIDEO_FRAME f_;
		VIDEO_FRAME *f = &f_;
		UCHAR data[SUBTITLE_CHUNK];
		f->data[0] = data;
		f->size = sizeof( data );

		// Use the retained gfx lookup (linear scan, bitmap-only)
		if( stream_sub_ext_get_gfx_data( s, &f, time ) ) return;

		if( f && f->valid ) {
			VIDEO_FRAME *f2 = s->subtitle_frame;
			s->sub_dec->decode( s->sub_dec, f->data[0], f->valid, f->time, &f2 );
			if( f2 ) {
				f2->time = f->time;
				_feed_bitmap_to_engine( s, f2 );
			}
		}
	}
	// Text: no per-frame work needed.
}

// ************************************************************
//
//	_sub_decode
//
// ************************************************************
void _sub_decode( STREAM *s )
{
	if( s->subtitle_changed ) {
		s->subtitle_changed = 0;
		// tell the user
		if( s->message_cb ) {
			s->message_cb( s, STREAM_SUB_PROPS_CHANGED );
		}
	}
	if( s->subtitle->valid && !s->paused ) {
		int time = s->video_time;
		if( time != -1 ) {
			// apply correction
			time -= s->subtitle_offset;
			if( time < 0 )
				time = 0;
		}
		if( s->subtitle->ext ) {
			_get_next_ext_sub( s, time );
		} else {
			_get_next_int_sub( s, time );
		}
	}
}

// ************************************************************
//
//	stream_sub_dec_thread
//
// ************************************************************
void *stream_sub_dec_thread( void *data )
{
	STREAM *s = (STREAM *)data;
DBGS serprintf("PID[%5d] stream_sub_dec_thread::Starting\r\n", getpid() );	
	
	while( thread_state_get( &s->sub_tstate ) != THREAD_EXIT ) {
		thread_state_ack( &s->sub_tstate );
		if( thread_state_get( &s->sub_tstate ) == THREAD_RUNNING ) {
			_sub_decode( s );
		}
		stream_yield_RT();
	}
DBGS serprintf("PID[%5d] stream_sub_dec_thread::Exiting\r\n", getpid() );	
 	return NULL;
}

// _stream_check_subtitles_sync -- unchanged pause/idle handling from the old stream_check_subtitles() body; now called from the discovery worker below instead of directly on the JNI caller's thread.
static int _stream_check_subtitles_sync( STREAM *s )
{
	if( !s->open ) {
serprintf("ScS: not open!\r\n");
		return 1;
	}

	// stream_sub_ext_update() does one incremental scan and picks the cheapest outcome itself, replacing the old has_new()+close()+check() double-scan. Append/rebuild still pause/idle as before since we can't be sure a live append is safe to do while stream_sub_dec_thread is running; only the no-op path skips pausing entirely.
	if( !s->subtitle_priv ) {
		// First-time open: nothing to diff against, no pause needed. Must still signal subtitle_changed explicitly -- stream_check_subtitles() now returns before this scan even runs, so nothing else announces the first menu population.
		stream_sub_ext_check( s );
		s->subtitle_changed = 1;
		return 0;
	}

	char prev_extsub[MAX_NAME_LEN + 1];
	int has_prev_extsub = 0;
	int prev_sub;
	uint64_t start_gen;
	// Locked: av.subs/subtitle must describe the same track, and start_gen
	// is the TOCTOU guard for the reselect below -- see subtitle_table_lock
	// in stream.h.
	pthread_mutex_lock( &s->subtitle_table_lock );
	prev_sub = s->av.subs;
	start_gen = s->subtitle_select_generation;
	if (s->subtitle && s->subtitle->valid && s->subtitle->ext) {
		strncpy(prev_extsub, s->subtitle->path, MAX_NAME_LEN);
		has_prev_extsub = 1;
	}
	pthread_mutex_unlock( &s->subtitle_table_lock );

	int was_paused = stream_pause( s );
	thread_state_set( &s->engine_tstate, THREAD_IDLE );
	thread_state_set( &s->sub_tstate,    THREAD_IDLE );

	int result = stream_sub_ext_update( s );

	if( result == 0 ) {
DBGS serprintf("stream_check_subtitles, no change in ext subtitles\r\n");
		thread_state_set( &s->engine_tstate, THREAD_RUNNING );
		thread_state_set( &s->sub_tstate,    THREAD_RUNNING );
		stream_un_pause( s, was_paused );
		return 0;
	}

	if( result > 0 ) {
		// Live append: new tracks are already in s->av.sub[]/subs_max and subtitle_changed is set; the playing track's index didn't move and there's no old decoder to tear down, so skip straight to resuming.
DBGS serprintf("stream_check_subtitles, %d new ext subtitle track(s) added\r\n", result);
		thread_state_set( &s->engine_tstate, THREAD_RUNNING );
		thread_state_set( &s->sub_tstate,    THREAD_RUNNING );
		stream_un_pause( s, was_paused );
		return 0;
	}

	// result == -1: full rebuild already happened inside stream_sub_ext_update(); tear down the old decoder and reselect by path as before.
DBGS serprintf("stream_check_subtitles, ext subtitles rebuilt\r\n");

	stream_close_sub_dec( s );
	frame_free( s->subtitle_frame );
	s->subtitle_frame = NULL;

	if (has_prev_extsub) {
		int i;

		prev_sub = 0;
		for (i = 0; i < s->av.subs_max; ++i) {
			if (strcmp(s->av.sub[i].path, prev_extsub) == 0) {
				prev_sub = i;
				break;
			}
		}
	}
	// If the generation moved, a manual switch happened during the rescan
	// and already set a fresher selection -- skip this write rather than
	// silently reverting it. See subtitle_table_lock in stream.h.
	pthread_mutex_lock( &s->subtitle_table_lock );
	if( s->subtitle_select_generation == start_gen ) {
		s->av.subs = prev_sub;
		if (s->av.subs >= s->av.subs_max)
			s->av.subs = 0;
		s->subtitle = s->av.sub + s->av.subs;
		s->subtitle_select_generation++;
	} else {
DBGS serprintf("stream_check_subtitles, subtitle selection changed during rescan -- not reverting to pre-scan track %d\r\n", prev_sub);
	}
	pthread_mutex_unlock( &s->subtitle_table_lock );

	s->subtitle_changed = 1;

	thread_state_set( &s->engine_tstate, THREAD_RUNNING );
	thread_state_set( &s->sub_tstate,    THREAD_RUNNING );

	stream_un_pause( s, was_paused );
	return 0;
}

// ---------------------------------------------------------------------------
// Discovery worker: moves the (possibly slow) filesystem scan + fopen()/detect() off whatever thread called stream_check_subtitles() (a JNI entry point with no thread hop of its own) -- same problem the parse worker in stream_sub_ext.c solves for format->parse(). Single-slot mailbox (not a queue): a second request just overwrites the pending one. Process-wide static, not per-STREAM, and deliberately never stopped at stream close -- see stream_sub_ext_wait_for_discovery() below.
typedef struct SUB_DISCOVERY_WORKER {
	pthread_t       thread;
	int             thread_started;
	THREAD_STATE    tstate;	// private; RUNNING while alive, never IDLE -- must not join any blocking idle-rendezvous
	pthread_mutex_t mutex;
	pthread_cond_t  cond;		// signaled on new request and on scan completion; both waiters re-check their own predicate on wake
	int             pending;	// 1 = a scan request is waiting to be picked up
	int             busy;		// 1 = a scan is actually running right now
	STREAM         *target;	// which STREAM the pending/running scan is for
} SUB_DISCOVERY_WORKER;

static SUB_DISCOVERY_WORKER *_discovery_worker = NULL;

static void *_discovery_worker_thread( void *arg )
{
	SUB_DISCOVERY_WORKER *w = (SUB_DISCOVERY_WORKER *)arg;

	while( thread_state_get( &w->tstate ) != THREAD_EXIT ) {
		pthread_mutex_lock( &w->mutex );
		while( !w->pending && thread_state_get( &w->tstate ) != THREAD_EXIT ) {
			pthread_cond_wait( &w->cond, &w->mutex );
		}
		if( thread_state_get( &w->tstate ) == THREAD_EXIT ) {
			pthread_mutex_unlock( &w->mutex );
			break;
		}
		STREAM *target = w->target;
		w->pending = 0;
		w->busy    = 1;
		pthread_mutex_unlock( &w->mutex );

		// The actual (possibly slow) directory scan + per-candidate
		// fopen()+detect(), plus this function's own pause/idle/reselect
		// handling -- safely off-thread now, unchanged internally.
		_stream_check_subtitles_sync( target );

		pthread_mutex_lock( &w->mutex );
		w->busy = 0;
		pthread_cond_broadcast( &w->cond ); // wakes stream_sub_ext_wait_for_discovery()
		pthread_mutex_unlock( &w->mutex );
	}
	return NULL;
}

static SUB_DISCOVERY_WORKER *_discovery_worker_ensure_started( void )
{
	if( _discovery_worker ) return _discovery_worker;

	SUB_DISCOVERY_WORKER *w = acalloc( 1, sizeof( SUB_DISCOVERY_WORKER ) );
	if( !w ) return NULL;

	pthread_mutex_init( &w->mutex, NULL );
	pthread_cond_init( &w->cond, NULL );
	thread_state_init( &w->tstate, THREAD_RUNNING, "subdiscover" );

	if( thread_create( &w->thread, _discovery_worker_thread, w, 0, "ext subtitle discovery" ) != 0 ) {
serprintf( "SUB_DISCOVERY_WORKER: failed to create thread\n" );
		pthread_mutex_destroy( &w->mutex );
		pthread_cond_destroy( &w->cond );
		afree( w );
		return NULL;
	}
	w->thread_started = 1;
	_discovery_worker = w;
	return w;
}

// stream_check_subtitles -- public/JNI entry point. Non-blocking: queues a scan on the discovery worker and returns immediately; falls back to a synchronous scan only if the worker couldn't be created (OOM).
int stream_check_subtitles( STREAM *s )
{
	if( !s ) return 1;

	SUB_DISCOVERY_WORKER *w = _discovery_worker_ensure_started();
	if( !w ) {
serprintf( "stream_check_subtitles: no discovery worker, scanning synchronously\n" );
		return _stream_check_subtitles_sync( s );
	}

	pthread_mutex_lock( &w->mutex );
	w->target  = s;
	w->pending = 1;
	pthread_cond_signal( &w->cond );
	pthread_mutex_unlock( &w->mutex );
	return 0;
}

// Called from stream_sub_ext_close() before it touches s->subtitle_priv, so closing a STREAM doesn't race the (process-wide) discovery worker still scanning for it. Bounded wait: returns immediately unless the worker is mid-scan or has a pending request for this s.
void stream_sub_ext_wait_for_discovery( STREAM *s )
{
	SUB_DISCOVERY_WORKER *w = _discovery_worker;
	if( !w ) return;

	// Self-deadlock guard: _discovery_worker_thread() runs
	// _stream_check_subtitles_sync(target) with w->busy=1 and w->target=target
	// for the whole call -- and that call's own full-rebuild path
	// (stream_sub_ext_update() -> stream_sub_ext_close()) reaches this exact
	// function, for that same `s == target`, from further down the SAME call
	// stack. Without this check, that nested call would see
	// `w->target == s && w->busy` and block waiting for w->busy to clear --
	// which can't happen until this very call frame returns. A thread can
	// never legitimately be waiting on a scan that is its own in-progress
	// call, so skip the wait whenever we're already running ON the discovery
	// worker's thread.
	if( w->thread_started && pthread_equal( pthread_self(), w->thread ) )
		return;

	pthread_mutex_lock( &w->mutex );
	while( w->target == s && ( w->pending || w->busy ) ) {
		pthread_cond_wait( &w->cond, &w->mutex );
	}
	pthread_mutex_unlock( &w->mutex );
}

// *****************************************************************************
//
//	stream_set_subtitle_stream
//
// *****************************************************************************
int stream_set_subtitle_stream( STREAM *s, int sub_stream )
{	
serprintf("stream_set_subtitle_stream( %d )\r\n", sub_stream );
 
	if( !s->open ) {
serprintf("SsS: not open!\r\n");
		return 1;
	}
	
	// Locked: read through the live subtitle table (stream.h's
	// subtitle_table_lock), racing the discovery worker's rebuild. Beyond
	// the valid/bounds gate, also capture the REQUESTED track's own
	// identity (ext + path) here -- this is what a discovery rebuild can
	// invalidate during the teardown below, and what gets re-resolved
	// right before the commit instead of trusting this snapshot blindly.
	// The `sub_stream == s->av.subs` check just below is left unlocked --
	// its return is commented out, so a stale read there only affects
	// a log line.
	int not_valid, out_of_range;
	int req_ext = 0;
	char req_path[MAX_NAME_LEN + 1] = { 0 };
	pthread_mutex_lock( &s->subtitle_table_lock );
	not_valid   = !s->subtitle->valid;
	out_of_range = ( sub_stream >= s->av.subs_max );
	if( !out_of_range ) {
		req_ext = s->av.sub[sub_stream].ext;
		if( req_ext )
			strncpy( req_path, s->av.sub[sub_stream].path, MAX_NAME_LEN );
	}
	pthread_mutex_unlock( &s->subtitle_table_lock );

	if( not_valid ) {
serprintf("SsS: not sub!\r\n");
		return 1;
	}

	if( out_of_range ) {
serprintf("SsS: sub_stream > av.subs_max\n");	
		return 1;
	}
	if( sub_stream == s->av.subs ) {
serprintf("SsS: sub_stream already set\n");	
//		return 0;
	}

	int was_paused = stream_pause( s );

	// idle threads to make sure they are at a known state
	thread_state_set( &s->engine_tstate, THREAD_IDLE );
	thread_state_set( &s->sub_tstate,    THREAD_IDLE );
	
	// close old subtitle decoder
	stream_close_sub_dec( s );

	// free subtitle frame
	frame_free( s->subtitle_frame );
	s->subtitle_frame = NULL;

	// Close the previous track's engine backend now, rather than leaving it
	// active until the new track's first packet arrives. Both s->engine_tstate
	// and s->sub_tstate are already idled above, so nothing can be mid-feed/
	// mid-render on this engine right now -- same safe window already used by
	// stream_close_sub_dec()/frame_free() just above. Without this, whatever
	// track was previously showing (SSA/SRT text or a GFX bitmap) stays fully
	// rendered until the new track's first packet opens a fresh one, which can
	// be a noticeable delay for a slow-starting or different-format track.
	if( s->sub_engine )
		sub_engine_close_track( (SUB_ENGINE*)s->sub_engine );

	// Locked -- re-resolve, don't just re-check, before committing.
	// Everything above ran unlocked and can take a while (pause, idle
	// rendezvous, decoder/engine teardown); a full discovery rebuild can
	// run entirely inside that window and shrink av.subs_max or replace
	// whatever's at sub_stream with a different file. A bare bounds
	// re-check isn't enough to catch the second case, so for an external
	// track we re-resolve by path -- same continuity logic
	// _stream_check_subtitles_sync() already uses for ITS prev-track
	// lookup -- rather than trusting the index captured above. Internal
	// tracks skip this: their indices are never touched by an external
	// rebuild, so the bound alone is sufficient. If the track can no
	// longer be found, the switch fails here rather than committing a
	// stale/wrong index and bumping the generation over it -- which would
	// also wrongly tell _stream_check_subtitles_sync()'s reselect to defer
	// to a selection that was never actually valid.
	int resolved = sub_stream;
	int found = 1;
	pthread_mutex_lock( &s->subtitle_table_lock );
	if( sub_stream >= s->av.subs_max ) {
		found = 0;
	} else if( req_ext ) {
		found = 0;
		if( s->av.sub[sub_stream].ext && !strncmp( s->av.sub[sub_stream].path, req_path, MAX_NAME_LEN ) ) {
			found = 1;	// still at the same index, common case
		} else {
			int i;
			for( i = 0; i < s->av.subs_max; i++ ) {
				if( s->av.sub[i].ext && !strncmp( s->av.sub[i].path, req_path, MAX_NAME_LEN ) ) {
					resolved = i;
					found = 1;
					break;
				}
			}
		}
	}
	if( found ) {
		s->av.subs  = resolved;
		s->subtitle = s->av.sub + s->av.subs;
		s->subtitle_select_generation++;
	}
	pthread_mutex_unlock( &s->subtitle_table_lock );

	// run threads again
	thread_state_set( &s->engine_tstate, THREAD_RUNNING );
	thread_state_set( &s->sub_tstate,    THREAD_RUNNING );

	stream_un_pause( s, was_paused );

	if( !found ) {
DBGS serprintf("stream_set_subtitle_stream, requested track %d no longer present after a concurrent rescan -- switch aborted, previous selection left in place\r\n", sub_stream );
		return 1;
	}
	// External tracks are fed to the engine independent of demuxer position and get picked up on the next sub_tstate tick, so reseeking for them just costs a demux/decode restart. Only internal/embedded tracks need the demuxer repositioned, since their cues arrive as demuxed packets.
	if( !s->subtitle->ext ) {
		// FIXME: there should be a better way without seek jumping
		int current_time = stream_get_current_time( s, NULL );
		if( current_time > 0 && thread_state_get( &s->parser_tstate ) != THREAD_EXIT && s->parser->seekable && s->parser->seekable( s ) ) {
			// reseek to current time to get internal subtitle decoder to reinitialize
			stream_seek_time( s, current_time - 1, STREAM_SEEK_BACKWARD, 0 );
		}
	}
	return 0;
}

// *****************************************************************************
//
//	stream_get_current_subtitle
//
// *****************************************************************************
VIDEO_FRAME *stream_get_current_subtitle( STREAM *s )
{
	return s ? s->subtitle_frame : NULL;
}

// *****************************************************************************
//
//	stream_set_subtitle_offset
//
// *****************************************************************************
void stream_set_subtitle_offset( STREAM *s, int offset )
{
	if( s ) { 
		s->subtitle_offset = offset;
	}
}

// *****************************************************************************
//
//	stream_set_subtitle_ratio
//
// *****************************************************************************
void stream_set_subtitle_ratio( STREAM *s, int n, int d )
{
	if( s ) {
		s->subtitle_ratio_n = n;
		s->subtitle_ratio_d = d;
	}
}

// ************************************************************
//
//	stream_set_subtitle_url
//
// ************************************************************
void stream_set_subtitle_url( STREAM *s, const char **url_list )
{
DBGS serprintf("stream_set_subtitle_url\n");		
	if( s && url_list ) {
		int i;
		for( i = 0; i < SUB_TRACK_MAX && url_list[i]; i++ ) {
DBGS serprintf("sub_url: %s\n", url_list[i] );
			if ( s->sub_url[i + 1] != NULL )
				afree( s->sub_url[i + 1] );
			s->sub_url[i + 1] = astrdup( url_list[i] );		
		}
	}
}
#endif

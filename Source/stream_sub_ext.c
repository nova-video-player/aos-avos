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
#include "stream.h"
#include "stream_subtitle.h"
#include "astdlib.h"
#include "debug.h"
#include "subtitle_format.h"
#include "util.h"
#include "file.h"
#include "browse.h"
#include "sub_engine.h"

#include "athread.h"

#include <ctype.h>		// for isspace
#include <unistd.h>
#include <string.h>

#define DBGS if(Debug[DBG_STREAM])
#define DBG  if(Debug[DBG_SUB])
#define DBG2 if(Debug[DBG_SUB] > 1)
#define DBG3 if(Debug[DBG_SUB] > 2)

#ifdef CONFIG_STREAM
#ifdef CONFIG_SUBTITLES

// Async subtitle parse worker: a bounded thread pool per STREAM's SUB_PRIV, spun up lazily and torn down in stream_sub_ext_close(). Deliberately not stream_sub_dec_thread -- that thread's THREAD_STATE uses a hard blocking thread_state_set() rendezvous, so a slow parse on it would freeze every other thread waiting on that rendezvous, which is exactly the freeze this worker exists to avoid. Also now runs SRT/VTT's streaming feed() (see _do_streaming_feed()), not just format->parse(); the two job kinds never touch the same shared state so mixing them in one queue is safe.

#define PARSE_QUEUE_MAX (SUB_TRACK_MAX)

// Bounded pool size, not core count: this is a mobile device sharing CPU with active decode threads, not a batch box. 3 is enough for real concurrency on the realistic case (a couple of slow PGS tracks plus an SSA track).
#define SUB_PARSE_POOL_SIZE 3

typedef struct SUB_PARSE_WORKER {
	pthread_t       threads[SUB_PARSE_POOL_SIZE];
	int             threads_started;	// count of successfully created pool threads; guards how many _worker_stop() joins
	THREAD_STATE    tstate;		// shared by the whole pool; RUNNING while alive, EXIT to tear down. Deliberately never IDLE -- see the file-level comment above
	pthread_mutex_t queue_mutex;
	pthread_cond_t  queue_cond;		// signaled on enqueue, stop, and job completion (a completion clears an inflight_title[] slot, unblocking a collided job) so the pool never busy-polls
	uni_sub        *queue[PARSE_QUEUE_MAX];
	int             queue_head;
	int             queue_count;
	STREAM         *stream;		// back-pointer, needed for _adjust_timing_for_track()'s s->video access after a MicroDVD track finishes parsing

	// Per-title collision avoidance: inflight_title[i] is the title pool thread i is processing, or NULL. Two jobs sharing a title (different lang_index values of the same SMI/IDX file) must never run IN_PROGRESS at once, since subtitle_do_parse()'s multi-language handling mutates shared state on the title. Guarded by queue_mutex.
	// values of the same SMI/IDX file) must never be IN_PROGRESS at once.
	// Unrelated titles (the overwhelmingly common case: different files
	// entirely) never collide and run fully concurrently. Guarded by
	// queue_mutex -- see _worker_dequeue() below, which claims a job and
	// its title atomically under that same lock.
	void           *inflight_title[SUB_PARSE_POOL_SIZE];
} SUB_PARSE_WORKER;

typedef struct SUB_PRIV {
	subtitle_files *files;
	converted_subs *subs;
	int stream;
	int prev_max;
	// engine_fmt: SUB_FMT_SSA for ASS/SSA tracks, SUB_FMT_SRT for all other
	// text tracks, SUB_FMT_GFX for bitmap tracks. Set per-track in
	// stream_sub_ext_check() and read by stream_subtitle.c at track open.
	int engine_fmt[SUB_TRACK_MAX];

	SUB_PARSE_WORKER *worker;	// NULL until the first track is queued;
					// see _worker_ensure_started() below
} SUB_PRIV;

static void _adjust_timing_for_track( STREAM *s, uni_sub *sub );

// Forward declarations -- both defined further down (they're the same
// _cue_to_engine/_sub_feed_should_stop every fmt->feed() caller has always
// used), needed here for _do_streaming_feed() below, which
// _parse_worker_thread() calls.
static void _cue_to_engine( void *ctx, const char *text, int start_ms, int end_ms );
static int  _sub_feed_should_stop( void *poll_ctx );

// poll_ctx for a streaming (SRT/VTT) feed job. Carries the uni_sub being fed (not just STREAM*) so _sub_feed_should_stop() can also detect a mid-feed track switch and bail out, rather than mixing a stale track's cues into the newly-selected track's engine. `stale` distinguishes that outcome from a genuine pause/seek/close.
typedef struct _STREAMING_POLL_CTX {
	STREAM  *s;
	uni_sub *job;
	int      stale;
} _STREAMING_POLL_CTX;

// Runs SRT/VTT's streaming feed() on the parse-worker thread instead of stream_sub_dec_thread (see file header comment), reusing the same _cue_to_engine()/_sub_feed_should_stop() every feed() caller uses. Returns 1 if the whole file was fed or a genuine pause/seek/close interrupted it (subtitle_ext_needs_refeed is set for the latter); 2 if `subs` was no longer the actively selected track, so the caller must NOT mark the job DONE; 0 only on a genuine setup failure (maps to SUBT_PARSE_FAILED).
static int _do_streaming_feed( STREAM *s, uni_sub *subs )
{
	if( !s || !subs || !subs->spex ) return 0;

	SUBTITLE_FORMAT *fmt = subtitle_get_format_for_sub( subs );
	if( !fmt || !fmt->feed ) return 0;

	// Skip if the user already switched away while this job sat in the queue -- nothing fed yet, nothing to flush.
	SUB_PRIV *p = s->subtitle_priv;
	int still_current = s->subtitle && p && p->subs &&
	                     s->subtitle->stream >= 0 && s->subtitle->stream < p->subs->cnt &&
	                     p->subs->converted[ s->subtitle->stream ] == subs;
	if( !still_current ) return 2;

	// Cleared before the call, not after: _sub_feed_should_stop() may set it back to 1 during the call, and that must survive.
	s->subtitle_ext_needs_refeed = 0;

	_STREAMING_POLL_CTX poll_ctx = { s, subs, 0 };
	fmt->feed( subs->spex, _cue_to_engine, s, _sub_feed_should_stop, &poll_ctx );

	return poll_ctx.stale ? 2 : 1;
}

// Finds and removes the first pending job (starting from queue_head, so _worker_prioritize()'s promotions are respected) whose title isn't already IN_PROGRESS on another pool thread. Returns NULL if the queue is empty or every pending job's title collides with something in flight -- only reachable when multiple queued jobs are all language variants of the same multi-language file. Caller must hold queue_mutex and record the returned job's title into inflight_title[my_slot] before releasing it, so claiming the job and its title happen as one atomic step.
static uni_sub *_worker_dequeue( SUB_PARSE_WORKER *w, int my_slot )
{
	int i;
	for( i = 0; i < w->queue_count; i++ ) {
		int idx = (w->queue_head + i) % PARSE_QUEUE_MAX;
		uni_sub *candidate = w->queue[idx];

		int collides = 0;
		int k;
		for( k = 0; k < SUB_PARSE_POOL_SIZE; k++ ) {
			if( k == my_slot ) continue;
			if( w->inflight_title[k] && w->inflight_title[k] == (void*)candidate->spex ) {
				collides = 1;
				break;
			}
		}
		if( collides ) continue;

		int j;
		for( j = i; j < w->queue_count - 1; j++ ) {
			int dst = (w->queue_head + j) % PARSE_QUEUE_MAX;
			int src = (w->queue_head + j + 1) % PARSE_QUEUE_MAX;
			w->queue[dst] = w->queue[src];
		}
		w->queue_count--;
		return candidate;
	}
	return NULL;
}

// Bundles what _parse_worker_thread() needs beyond SUB_PARSE_WORKER* --
// pthread_create() only passes one void*, and each pool thread needs to
// know its OWN slot index (into inflight_title[]) in addition to the
// worker it belongs to. Heap-allocated per thread by _worker_ensure_started()
// below, freed by the thread itself immediately after reading it out.
typedef struct {
	SUB_PARSE_WORKER *w;
	int                slot;
} SUB_PARSE_WORKER_THREAD_ARG;

// One instance runs per pool thread. Each pulls a job off the shared queue via _worker_dequeue() (blocking only on queue_cond, never on THREAD_STATE's rendezvous), services it synchronously -- this is where a slow fopen()/fread() actually happens, safely -- then loops. Branches on is_streaming: SRT/VTT have no format->parse() (.parse is NULL), so they need _do_streaming_feed() instead, the feed-into-engine work that used to run inline on stream_sub_dec_thread.
static void *_parse_worker_thread( void *arg )
{
	SUB_PARSE_WORKER_THREAD_ARG *targ = (SUB_PARSE_WORKER_THREAD_ARG *)arg;
	SUB_PARSE_WORKER *w    = targ->w;
	int                slot = targ->slot;
	afree( targ );

	while( thread_state_get( &w->tstate ) != THREAD_EXIT ) {
		pthread_mutex_lock( &w->queue_mutex );

		uni_sub *job;
		for( ;; ) {
			if( thread_state_get( &w->tstate ) == THREAD_EXIT ) {
				pthread_mutex_unlock( &w->queue_mutex );
				return NULL;
			}
			job = _worker_dequeue( w, slot );
			if( job ) break;
			// Either the queue is genuinely empty, or every pending job
			// collides with another thread's in-flight title -- either
			// way, wait. Woken by: a fresh enqueue, stop, OR another
			// thread finishing a job (which clears an inflight_title[]
			// slot and could un-collide something we skipped) -- see the
			// broadcast at the bottom of this loop.
			pthread_cond_wait( &w->queue_cond, &w->queue_mutex );
		}
		w->inflight_title[slot] = (void*)job->spex; // claimed atomically
		                                             // with the dequeue
		                                             // above, still under
		                                             // queue_mutex
		pthread_mutex_unlock( &w->queue_mutex );

		pthread_mutex_lock( &job->parse_mutex );
		job->parse_state = SUBT_PARSE_IN_PROGRESS;
		pthread_mutex_unlock( &job->parse_mutex );

		int was_ok;
		int stale = 0; // job->is_streaming only -- see _do_streaming_feed()'s
		                // return-2 case
		if( job->is_streaming ) {
			was_ok = _do_streaming_feed( w->stream, job ); // patch 6
			if( was_ok == 2 ) {
				stale = 1;
				was_ok = 0;
			}
		} else {
			was_ok = subtitle_do_parse( job ); // the actual (possibly slow) file I/O

			if( was_ok ) {
				// MicroDVD frame->ms conversion must happen right after THIS
				// track's own parse, on THIS thread -- not on whatever thread
				// later polls parse_state==DONE, since that thread has no
				// reason to redo it and _adjust_timing_for_track() is not
				// idempotent-safe to call twice on the same already-converted
				// timestamps. Streaming tracks never carry frame_multiplier
				// (that's a MicroDVD-only concept, and MicroDVD isn't
				// is_streaming), so this is skipped for the branch above --
				// not just redundant, genuinely inapplicable.
				_adjust_timing_for_track( w->stream, job );
			}
		}

		pthread_mutex_lock( &job->parse_mutex );
		if( stale ) {
			// The user switched away from this track before/while this job
			// ran -- nothing was left in the engine for it (see
			// _do_streaming_feed()'s comment). NOT_QUEUED, not DONE/FAILED,
			// so a later genuine re-selection of this same track re-queues
			// and properly re-feeds it instead of _poll_parse_state()
			// wrongly reporting "already fed" for a track whose cues were
			// never actually left in the engine.
			job->parse_state = SUBT_PARSE_NOT_QUEUED;
		} else {
			job->parse_state = was_ok ? SUBT_PARSE_DONE : SUBT_PARSE_FAILED;
		}
		pthread_mutex_unlock( &job->parse_mutex );

		// Wake the sub-decode thread's per-frame poll so a freshly-completed non-streaming track is picked up promptly. Scoped to non-streaming only: streaming jobs already fed the engine directly on this thread, and this flag means "redo the feed" -- setting it after a clean streaming completion would loop forever.
		if( w->stream && !job->is_streaming && !stale ) {
			w->stream->subtitle_ext_needs_refeed = 1;
		}

		// Release this slot's title claim and wake any sibling thread that might have skipped a now-un-collided job.
		pthread_mutex_lock( &w->queue_mutex );
		w->inflight_title[slot] = NULL;
		pthread_cond_broadcast( &w->queue_cond );
		pthread_mutex_unlock( &w->queue_mutex );
	}

	return NULL;
}

// Lazily creates SUB_PRIV's worker pool on first use (idempotent). Creates up to SUB_PARSE_POOL_SIZE threads; partial failure is tolerated (whatever subset started forms a smaller working pool) -- only zero threads created counts as "no worker".
static SUB_PARSE_WORKER *_worker_ensure_started( STREAM *s, SUB_PRIV *p )
{
	if( p->worker ) return p->worker;

	SUB_PARSE_WORKER *w = acalloc( 1, sizeof( SUB_PARSE_WORKER ) );
	if( !w ) return NULL;

	w->stream = s;
	pthread_mutex_init( &w->queue_mutex, NULL );
	pthread_cond_init( &w->queue_cond, NULL );
	thread_state_init( &w->tstate, THREAD_RUNNING, "subparse" );

	int i;
	for( i = 0; i < SUB_PARSE_POOL_SIZE; i++ ) {
		SUB_PARSE_WORKER_THREAD_ARG *targ = acalloc( 1, sizeof( SUB_PARSE_WORKER_THREAD_ARG ) );
		if( !targ ) break; // OOM -- stop trying to grow the pool further,
		                    // keep whatever's already running
		targ->w    = w;
		targ->slot = i;

		if( thread_create( &w->threads[i], _parse_worker_thread, targ, 0, "ext subtitle parser" ) != 0 ) {
serprintf( "SUB_PRIV: failed to create subtitle parse worker thread %d/%d\n", i, SUB_PARSE_POOL_SIZE );
			afree( targ );
			break;
		}
		w->threads_started++;
	}

	if( w->threads_started == 0 ) {
serprintf( "SUB_PRIV: failed to create any subtitle parse worker threads\n" );
		pthread_mutex_destroy( &w->queue_mutex );
		pthread_cond_destroy( &w->queue_cond );
		afree( w );
		return NULL;
	}

	p->worker = w;
	return w;
}

// Registered with subtitle_formats.c via subtitle_set_parse_enqueue_fn() so subtitle_ensure_parsed_async() can hand a freshly-QUEUED track to this worker without subtitle_formats.c knowing about STREAM/SUB_PRIV. Since subtitle_parse_enqueue_fn has no STREAM* parameter, this recovers the right worker via a process-wide "most recently active worker" pointer -- correct under this codebase's single-active-stream assumption; a truly concurrent multi-stream player would need a STREAM back-pointer threaded through subt_orig instead.
static SUB_PARSE_WORKER *_last_active_worker = NULL;

// Companion to _last_active_worker: covers the no-active-worker fallback, which still needs a STREAM* to run a streaming track's feed against even though there's no w->stream to read. Set unconditionally alongside _last_active_worker, even on worker-creation failure.
static STREAM *_last_active_stream = NULL;

// Shared "no active worker"/"queue full" fallback for _worker_enqueue()/_worker_prioritize(). subtitle_do_parse() alone is wrong for streaming (SRT/VTT) tracks since format->parse() is NULL for them -- it would mark them FAILED without ever feeding. `s` may be NULL if no worker was ever started for any stream, in which case a streaming track genuinely can't be fed and the fallback is a real FAILED.
static void _synchronous_parse_fallback( STREAM *s, uni_sub *sub )
{
	if( sub->is_streaming ) {
		int was_ok = s ? _do_streaming_feed( s, sub ) : 0;
		pthread_mutex_lock( &sub->parse_mutex );
		if( was_ok == 2 ) sub->parse_state = SUBT_PARSE_NOT_QUEUED; // stale -- see _do_streaming_feed()'s comment
		else               sub->parse_state = was_ok ? SUBT_PARSE_DONE : SUBT_PARSE_FAILED;
		pthread_mutex_unlock( &sub->parse_mutex );
	} else {
		subtitle_do_parse( sub );
	}
}

static void _worker_enqueue( uni_sub *sub )
{
	SUB_PARSE_WORKER *w = _last_active_worker;
	if( !w ) {
		// No worker registered/active (e.g. called before any
		// stream_sub_ext_check() ever ran, or after close()) -- fall back
		// to a direct synchronous parse/feed right here rather than
		// dropping the job. subtitle_ensure_parsed_async() already
		// transitioned this track to QUEUED before calling us, so we own
		// running it. _last_active_stream (not w->stream -- there is no w
		// here) is what makes the streaming case work; see its own
		// comment and _synchronous_parse_fallback()'s.
DBG serprintf("_worker_enqueue: no active worker, parsing synchronously\n");
		_synchronous_parse_fallback( _last_active_stream, sub );
		return;
	}

	pthread_mutex_lock( &w->queue_mutex );
	if( w->queue_count >= PARSE_QUEUE_MAX ) {
		// Queue genuinely full (would need SUB_TRACK_MAX+1 distinct
		// tracks queued at once, i.e. every possible track slot already
		// pending -- practically unreachable, but handle it rather than
		// silently drop or overflow): parse/feed synchronously right here
		// as a fallback, same reasoning as the no-worker case above.
		pthread_mutex_unlock( &w->queue_mutex );
serprintf("_worker_enqueue: queue full, parsing synchronously\n");
		_synchronous_parse_fallback( w->stream, sub );
		return;
	}
	int tail = (w->queue_head + w->queue_count) % PARSE_QUEUE_MAX;
	w->queue[tail] = sub;
	w->queue_count++;
	pthread_cond_signal( &w->queue_cond );
	pthread_mutex_unlock( &w->queue_mutex );
}

// Registered with subtitle_formats.c via subtitle_set_parse_priority_fn(), called for a track that was just actively selected (vs. passively warmed up by _queue_all_tracks(), which still uses the regular tail-insert enqueue). Two cases: if `sub` isn't queued yet, head-insert it (subtitle_ensure_parsed_async_priority() just flipped it to QUEUED); if it's already somewhere in the queue -- the common case, since every track is eagerly warmed up before the user picks one -- find it via linear scan (bounded by PARSE_QUEUE_MAX) and shift it to the front.
static void _worker_prioritize( uni_sub *sub )
{
	SUB_PARSE_WORKER *w = _last_active_worker;
	if( !w ) {
		// No active worker -- same fallback as _worker_enqueue(), including
		// using _last_active_stream since there's no w->stream here.
DBG serprintf("_worker_prioritize: no active worker, parsing synchronously\n");
		_synchronous_parse_fallback( _last_active_stream, sub );
		return;
	}

	pthread_mutex_lock( &w->queue_mutex );

	int i;
	for( i = 0; i < w->queue_count; i++ ) {
		int idx = (w->queue_head + i) % PARSE_QUEUE_MAX;
		if( w->queue[idx] != sub ) continue;

		// Found it at logical position i (0 == already at the head, nothing
		// to do). Shift every entry ahead of it back by one slot, then
		// place `sub` at the head -- equivalent to a remove-then-head-
		// insert, without needing a second pass or a temp buffer.
		int j;
		for( j = i; j > 0; j-- ) {
			int dst = (w->queue_head + j) % PARSE_QUEUE_MAX;
			int src = (w->queue_head + j - 1) % PARSE_QUEUE_MAX;
			w->queue[dst] = w->queue[src];
		}
		w->queue[w->queue_head] = sub;

		pthread_mutex_unlock( &w->queue_mutex );
		return;
	}

	// Not currently in the queue -- a fresh NOT_QUEUED->QUEUED transition; head-insert it, same queue-full fallback as _worker_enqueue().
	if( w->queue_count >= PARSE_QUEUE_MAX ) {
		pthread_mutex_unlock( &w->queue_mutex );
serprintf("_worker_prioritize: queue full, parsing synchronously\n");
		_synchronous_parse_fallback( w->stream, sub );
		return;
	}
	w->queue_head = (w->queue_head - 1 + PARSE_QUEUE_MAX) % PARSE_QUEUE_MAX;
	w->queue[w->queue_head] = sub;
	w->queue_count++;
	pthread_cond_signal( &w->queue_cond );
	pthread_mutex_unlock( &w->queue_mutex );
}

// Signals every pool thread to exit and joins each. Called from stream_sub_ext_close(). Jobs still queued are simply abandoned -- no "drain the queue first" wait, since that would reintroduce the blocking-on-parse this worker exists to avoid. In-flight jobs finishing after tstate flips to EXIT is fine: each only touches its own `job` (about to be freed regardless) and w->stream (null-checked in _parse_worker_thread against a racing close()).
static void _worker_stop( SUB_PARSE_WORKER *w )
{
	if( !w ) return;

	if( _last_active_worker == w ) {
		_last_active_worker = NULL;
	}

	if( w->threads_started > 0 ) {
		pthread_mutex_lock( &w->queue_mutex );
		thread_state_init( &w->tstate, THREAD_EXIT, "subparse" ); // re-init is safe: no other thread touches this tstate except the pool threads' own lock-free thread_state_get() polls
		// broadcast, not signal: ALL pool threads are waiting on this same
		// queue_cond, and every one of them must wake up to see tstate ==
		// EXIT and return -- a signal would only guarantee waking one.
		pthread_cond_broadcast( &w->queue_cond );
		pthread_mutex_unlock( &w->queue_mutex );

		int i;
		for( i = 0; i < w->threads_started; i++ ) {
			apthread_join( w->threads[i], NULL );
		}
	}

	pthread_mutex_destroy( &w->queue_mutex );
	pthread_cond_destroy( &w->queue_cond );
	afree( w );
}

// The discovery worker (stream_subtitle.c) is process-wide, not owned by this stream's SUB_PRIV; called from stream_sub_ext_close() so it can confirm that worker isn't mid-scan (or about to start one) for `s` before anything below frees what it would write into.

static int _get_time_from_frame(VIDEO_PROPERTIES *video, int frame)
{
	if( !video->valid) {
		return -2;
	}
	return (UINT32)( 1000ull * (UINT64)frame * (UINT64)video->scale / (UINT64)video->rate);
}

// Multiplies one already-parsed track's start/end by framerate; only MicroDVD (.sub, frame-based) needs this. Called once, on the parse-worker thread, right after that track's subtitle_do_parse() completes -- not idempotent, must not be called twice for the same track.
static void _adjust_timing_for_track( STREAM *s, uni_sub *sub )
{
	if( !s || !sub || !sub->frame_multiplier ) return;

	sub_line *line = sub->first;
	while( line ) {
		line->start = _get_time_from_frame( s->video, line->start );
		line->end   = _get_time_from_frame( s->video, line->end   );
		line = line->next;
	}
}

// Non-blocking check shared by feed_engine/get_gfx_data below: returns sub's SUBT_PARSE_* state, kicking off async parsing if not started, or jumping it ahead of the queue if already queued (subtitle_ensure_parsed_async_priority()). Represents the track being actively wanted right now, vs. _queue_all_tracks()'s passive tail-insert warm-up. Never blocks.
static int _poll_parse_state( uni_sub *sub )
{
	return subtitle_ensure_parsed_async_priority( sub );
}

static int scale_time( STREAM *s, int time )
{
	int ratio_n = s->subtitle_ratio_n;
	int ratio_d = s->subtitle_ratio_d;

	if( ratio_n && ratio_d ) {
		return time * (UINT64)ratio_n / (UINT64)ratio_d;
	}

	return time;
}

static subtitle_files *get_subtitle_files( STREAM *s )
{
	const char *name =  s->src.name[0] == '\0' ? cut_path( s->src.url ) : s->src.name;

	if (!name)
		return NULL;
	// make the current path the 1st entry in the url list
	if( s->sub_url[0] ) {
		afree( s->sub_url[0] );
		s->sub_url[0] = NULL;
	}
	s->sub_url[0] = astrdup( s->src.url );
	return subtitle_check_files( (const char**)s->sub_url, name );
}

// Same file-discovery step as get_subtitle_files(), but folds in whatever
// was found on a previous scan (see subtitle_check_files_incremental()) so
// already-known files skip detect()/info() entirely. `prev` is consumed --
// see subtitle_check_files_incremental()'s contract.
static subtitle_files *get_subtitle_files_incremental( STREAM *s, subtitle_files *prev )
{
	const char *name =  s->src.name[0] == '\0' ? cut_path( s->src.url ) : s->src.name;

	if (!name) {
		if( prev ) subtitle_free_files( prev ); // consume even on early return
		return NULL;
	}
	if( s->sub_url[0] ) {
		afree( s->sub_url[0] );
		s->sub_url[0] = NULL;
	}
	s->sub_url[0] = astrdup( s->src.url );
	return subtitle_check_files_incremental( (const char**)s->sub_url, name, prev );
}



// Builds SUB_PROPERTIES menu entries for p->subs->converted[start_idx..cnt), appending to s->av.sub[]/advancing s->av.subs_max. Shared by the full rebuild (start_idx==0) and the incremental append (start_idx==old cnt) so the vobsub/is_pgs/is_ssa -> engine_fmt classification lives in one place. Looks up each track's source path via uni_sub->spex->filename directly rather than walking p->files->files in lockstep, since that assumption breaks once tracks can be appended out of original order.
static void _add_menu_entries( STREAM *s, SUB_PRIV *p, int start_idx )
{
	int i;
	for( i = start_idx; i < p->subs->cnt; i++ ) {
		if( !p->subs->converted[i] ) continue;
		if( s->av.subs_max >= SUB_TRACK_MAX ) break;

		SUB_PROPERTIES *sub = s->av.sub + s->av.subs_max;
		uni_sub *conv = p->subs->converted[i];

		// Determine SUB_FORMAT_* first (the only thing the vobsub/is_pgs/is_ssa
		// detector booleans decide). engine_fmt is then ALWAYS derived from
		// sub->format via the single canonical sub_fmt_from_format() mapping --
		// see sub_format.h -- rather than assigned independently per-branch, so
		// this can't drift from the internal-track / ffdec-bitmap call sites.
		if ( conv->vobsub ) {
			sub->format        = SUB_FORMAT_DVD_GFX;
			sub->gfx           = 1;
		} else if ( conv->is_pgs ) {
			sub->format        = SUB_FORMAT_PGS;
			sub->gfx           = 1;
		} else if ( conv->is_ssa ) {
			sub->format        = SUB_FORMAT_SSA;
			sub->gfx           = 0;
		} else {
			sub->format        = SUB_FORMAT_EXT;
			sub->gfx           = 0;
			// conv->is_streaming (SRT/VTT) marks that feed() will be used instead
			// of walking the uni_sub list in stream_sub_ext_feed_engine() -- it
			// does not change the engine format, both paths are SUB_FMT_SRT.
		}
		p->engine_fmt[s->av.subs_max] = sub_fmt_from_format( sub->format );
		sub->ext            = 1;
		sub->stream         = i;
		sub->valid          = 1;
		if( conv->has_palette ) {
DBGS serprintf("has palette!\n");
			sub->extraDataSize = sizeof( conv->palette );
			memcpy( sub->extraData, conv->palette, sub->extraDataSize );
		}
		s->av.subs_max ++;

		strnZcpy( sub->name, conv->identifier, AV_NAME_LEN );
		if( conv->spex && conv->spex->filename ) {
			strnZcpy( sub->path, conv->spex->filename, MAX_NAME_LEN );
		}
	}
}

// Kicks off async parsing for every unstarted track in p->subs->converted[start_idx..cnt), right after the menu is (re)built -- so all discovered files start parsing on the background worker before the user picks a track. Non-blocking: subtitle_ensure_parsed_async() just enqueues and returns, so this doesn't hold up video start.
static void _queue_all_tracks( converted_subs *subs, int start_idx )
{
	if( !subs ) return;
	int i;
	for( i = start_idx; i < subs->cnt; i++ ) {
		if( !subs->converted[i] ) continue;
		// Streaming (SRT/VTT) tracks go through this same worker but not eagerly here: s->sub_engine only holds one open backend at a time, so a streaming track's feed job is only enqueued at actual selection time, via stream_sub_ext_feed_engine()'s _poll_parse_state() call.
		if( subs->converted[i]->is_streaming ) continue;
		subtitle_ensure_parsed_async( subs->converted[i] );
	}
}

// Core of stream_sub_ext_check(), factored out so a caller with a fresh subtitle_files* already in hand (stream_sub_ext_update()) can pass it directly instead of forcing a second scan. `files` is consumed either way.
static int _stream_sub_ext_check_core( STREAM *s, subtitle_files *files )
{
	if( !files )
		return 1;

	if( !s->subtitle_priv ) {
		s->subtitle_priv = amalloc( sizeof( SUB_PRIV ) );
		if( !s->subtitle_priv ) {
			subtitle_free_files( files );
			return 1;
		}
	}
	SUB_PRIV *p = s->subtitle_priv;
	memset( p, 0, sizeof( SUB_PRIV ) );
	p->prev_max = s->av.subs_max;
	p->files = files;

	// now every file that contains valid subtitles (according to name of file)
	// has been found. Convert every file to general format.
	if ( !p->files ) {
		DBG serprintf( "Failed to find subtitles\n" );
		goto NULL_SUBTITLES;
	}

	// Build track stubs (classification + language metadata) WITHOUT
	// parsing any file's cue data yet -- see subtitle_get_converted() in
	// subtitle_formats.c.
	p->subs = subtitle_get_converted( p->files, s->flags & STREAM_SUBTITLES_CLEAN_TAGS );
	if(!p->subs){
		goto NULL_SUBTITLES;
	}

	_add_menu_entries( s, p, 0 );

	// Start the background parse worker for this stream and make it the
	// active target for subtitle_ensure_parsed_async()'s enqueue callback
	// (see _worker_enqueue()'s comment for why a single "active worker"
	// pointer is sufficient here). Then immediately queue every discovered
	// track -- see _queue_all_tracks() -- so parsing for ALL of them
	// starts now, off this thread, in parallel with video startup.
	//
	// _last_active_stream is set unconditionally, even if worker creation
	// below fails -- see its own comment (near _worker_enqueue()) for why:
	// the no-active-worker fallback in _worker_enqueue()/_worker_prioritize()
	// needs a STREAM* precisely in the case where there IS no worker to
	// read one from via w->stream.
	_last_active_stream = s;
	if( _worker_ensure_started( s, p ) ) {
		_last_active_worker = p->worker;
		subtitle_set_parse_enqueue_fn( _worker_enqueue );
		subtitle_set_parse_priority_fn( _worker_prioritize ); // patch 7
		_queue_all_tracks( p->subs, 0 );
	}

	p->stream = -1;

	return 0;

NULL_SUBTITLES:
	stream_sub_ext_close( s );
	return 1;
}

// Public entry point -- always scans fresh and does a full destructive rebuild. Kept for callers that want the old, simple, always-correct behavior (e.g. first-time track open, with nothing to diff against). stream_check_subtitles() prefers stream_sub_ext_update() below to avoid the redundant scan this entry point can't skip.
int stream_sub_ext_check( STREAM *s )
{
	if( !s )
		return 1;

DBGS serprintf("stream_sub_ext_check: [%s]\r\n", s->sub_url[0] ? s->sub_url[0] : "(null)" );

	return _stream_sub_ext_check_core( s, get_subtitle_files( s ) );
}

// Single-scan replacement for the old has_new()->close()->check() sequence, which always threw away one full scan just to answer "did anything change" and then paid for a second full scan plus an unconditional pause/teardown/rebuild on any change. This does exactly one incremental scan (known files skip detect()) and picks the cheapest valid path: no subtitle_priv yet -> full check; unchanged -> no-op; pure addition -> live non-destructive append, no pause; anything removed/renamed -> full destructive rebuild, reusing the scan already in hand. Returns 0 if nothing changed, >0 = tracks added, or -1 if a full rebuild ran (track indices may have changed).
int stream_sub_ext_update( STREAM *s )
{
	if( !s ) return 0;

	SUB_PRIV *p = s->subtitle_priv;

	if( !p || !p->files ) {
		// First-time open, or a previous check failed to produce state --
		// nothing to diff against, no scan to reuse. Fall back to the
		// simple always-scan-fresh path.
		return _stream_sub_ext_check_core( s, get_subtitle_files( s ) ) == 0 ? -1 : 0;
	}

	// Snapshot the CURRENT file paths as plain strings before the call
	// below consumes p->files -- subtitle_check_files_incremental() reuses
	// and mutates (unlinks/relinks) the subt_orig nodes it's handed, so
	// diffing against p->files's list structure "after" the call (even via
	// an earlier shallow copy of the subtitle_files struct, which shares
	// the same mutated subt_orig chain) would be comparing against
	// already-rewritten pointers, not the pre-call state. Plain strings
	// have no such lifetime hazard.
	int old_count = p->files->count;
	char **old_names = old_count > 0 ? acalloc( old_count, sizeof(char*) ) : NULL;
	if( old_names ) {
		int idx = 0;
		subt_orig *o = p->files->files;
		while( o && idx < old_count ) {
			old_names[idx++] = o->filename ? astrdup( o->filename ) : NULL;
			o = o->next;
		}
	}

	subtitle_files *merged = get_subtitle_files_incremental( s, p->files );
	p->files = NULL;

	int rc;
	if( !merged ) {
		// Nothing found at all now -- treat as "everything vanished",
		// same as the old has_new() would have flagged a change and
		// check() would then have found nothing and gone NULL_SUBTITLES.
		stream_sub_ext_close( s );
		rc = -1;
		goto done;
	}

	{
		// Pure addition iff every old name is still present in merged.
		int all_present = 1;
		int i;
		for( i = 0; i < old_count && all_present; i++ ) {
			if( !old_names[i] ) continue;
			int found = 0;
			subt_orig *n = merged->files;
			while( n ) {
				if( n->filename && !strcmp( n->filename, old_names[i] ) ) { found = 1; break; }
				n = n->next;
			}
			if( !found ) all_present = 0;
		}

		if( all_present && merged->count == old_count ) {
			// Identical file set -- nothing changed. Hand the (functionally
			// equivalent, all-reused) merged list back as p->files so the
			// next call has fresh state to diff against, without touching
			// the menu, engine, or any track index.
			p->files = merged;
			rc = 0;
		} else if( all_present ) {
			// Every old file still present, and there's more now -- a pure
			// addition. Safe to append live, no teardown.
			p->files = merged;
			int old_cnt = p->subs->cnt;
			int added = subtitle_append_new_converted( merged, p->subs, s->flags & STREAM_SUBTITLES_CLEAN_TAGS );
			if( added > 0 ) {
				_add_menu_entries( s, p, old_cnt );
				// Queue just the newly-appended tracks -- the pre-existing
				// ones ([0, old_cnt)) were already queued (or already
				// finished) by an earlier call, no need to touch them again.
				_queue_all_tracks( p->subs, old_cnt );
				s->subtitle_changed = 1;
			}
			rc = added;
		} else {
			// Something existing disappeared or changed identity --
			// fall back to the full destructive rebuild, same as the old
			// close()+check() path, but reusing the scan already paid for
			// here instead of scanning a third time.
			stream_sub_ext_close( s );
			_stream_sub_ext_check_core( s, merged );
			rc = -1;
		}
	}

done:
	if( old_names ) {
		int i;
		for( i = 0; i < old_count; i++ ) {
			if( old_names[i] ) afree( old_names[i] );
		}
		afree( old_names );
	}
	return rc;
}

// *************************
//
// stream_sub_ext_close
//
// *************************
void stream_sub_ext_close( STREAM *s )
{
DBGS serprintf("stream_sub_ext_close\r\n" );

	// The discovery worker (stream_subtitle.c) is process-wide; make sure it isn't mid-scan (or about to start one) for `s` before anything below frees what it would write into. Bounded, one-time wait.
	stream_sub_ext_wait_for_discovery( s );

	SUB_PRIV *p = s->subtitle_priv;
	if( p ) {
		// Stop the parse worker first, before freeing the uni_sub entries it might still be touching -- subtitle_do_parse() writes into a uni_sub's fields with no awareness that close() might run concurrently; freeing under an in-flight parse would be a use-after-free. _worker_stop() joins every pool thread, so this can block up to the slowest in-flight job's remaining time (they already run concurrently, so this isn't their sum); queued-but-not-started jobs are simply abandoned. A one-time, bounded wait at teardown, not a recurring stall -- thread cancellation was considered and rejected since killing a thread mid-fopen()/malloc() risks leaks/corruption, same reasoning as every other thread teardown in this codebase.
		_worker_stop( p->worker );
		p->worker = NULL;

		// _last_active_stream (patch 7's synchronous-fallback gap fix,
		// see its own comment near _worker_enqueue()) must be cleared
		// alongside _last_active_worker -- otherwise it would keep
		// pointing at this STREAM after this function's caller frees it,
		// and a later no-active-worker fallback (for some future,
		// different STREAM opened before a new worker exists yet) could
		// hand _do_streaming_feed() a dangling pointer.
		if( _last_active_stream == s ) {
			_last_active_stream = NULL;
		}

		int i;
		for (i = p->prev_max; i < s->av.subs_max; ++i) {
			SUB_PROPERTIES *sub = s->av.sub + i;
			memset(sub, 0, sizeof(SUB_PROPERTIES));
		}
		s->av.subs_max = p->prev_max;
		if( p->files )
			subtitle_free_files( p->files );
		if( p->subs )
			subtitle_free_converted( p->subs );
		afree( s->subtitle_priv );
		s->subtitle_priv = NULL;
	}
}

// _cue_to_engine — sub_cue_cb implementation.
// Fired by feed_SRT / feed_VTT for each parsed cue.
// Converts the cue text to an ASS Dialogue event and feeds it to the engine.
// time parameters from the parser are already in milliseconds.
static void _cue_to_engine( void *ctx, const char *text, int start_ms, int end_ms )
{
	STREAM *s = (STREAM *)ctx;
	if( !s || !s->sub_engine || !text || !text[0] ) return;
	int duration = end_ms - start_ms;
	if( duration < 0 ) duration = 0;
	sub_engine_feed( (SUB_ENGINE*)s->sub_engine,
	                 (const uint8_t*)text, strlen(text),
	                 scale_time( s, start_ms ),
	                 scale_time( s, duration ) );
}

// sub_feed_poll_cb implementation behind feed_SRT()/feed_VTT()'s periodic checkpoint. Checked first: track-switch staleness -- bails and flushes if the user selected a different track while this job was queued/mid-feed (safe unconditionally, since the FIFO worker guarantees nothing else is writing to the engine at this point). Then the normal pause/seek/close check via thread_state_asked(), which also flushes and marks the track for a full refeed (feed_SRT/feed_VTT always restart from the top of their in-memory buffer, so "refeed" means redo the whole file, not resume). Otherwise calls stream_yield_RT() on every non-stopping checkpoint to relieve eng->lock contention with the render thread.
static int _sub_feed_should_stop( void *poll_ctx )
{
	_STREAMING_POLL_CTX *pc = (_STREAMING_POLL_CTX *)poll_ctx;
	if( !pc || !pc->s ) return 0;
	STREAM *s = pc->s;

	SUB_PRIV *p = s->subtitle_priv;
	int still_current = s->subtitle && p && p->subs &&
	                     s->subtitle->stream >= 0 && s->subtitle->stream < p->subs->cnt &&
	                     p->subs->converted[ s->subtitle->stream ] == pc->job;
	if( !still_current ) {
		if( s->sub_engine ) sub_engine_flush( (SUB_ENGINE*)s->sub_engine );
		pc->stale = 1;
		return 1;
	}

	if( thread_state_asked( &s->sub_tstate ) != THREAD_RUNNING ) {
		if( s->sub_engine ) sub_engine_flush( (SUB_ENGINE*)s->sub_engine );
		s->subtitle_ext_needs_refeed = 1;
		return 1;
	}

	stream_yield_RT();
	return 0;
}

// Called at track open and on refeed, from _get_next_ext_sub(). Gets the parsed/fed subtitle track into the C engine so Libass owns the full timeline, same as internal tracks. SRT/VTT go through the same SUBT_PARSE_* readiness check as every other format via _poll_parse_state(); for a streaming track SUBT_PARSE_DONE means the worker already fed its cues, so this just confirms readiness and returns. Returns the engine format (SUB_FMT_SSA/SUB_FMT_SRT/SUB_FMT_GFX), or -1 on error.
int stream_sub_ext_feed_engine( STREAM *s )
{
	SUB_PRIV *p = s->subtitle_priv;
	if( !p || !p->subs ) return -1;

	int stream = s->subtitle->stream;
	if( stream < 0 || stream >= p->subs->cnt ) return -1;

	uni_sub *subs = p->subs->converted[stream];
	if( !subs ) return -1;

	int engine_fmt = p->engine_fmt[s->av.subs];

	// Non-blocking readiness check, uniform across every format now
	// (patch 6). _poll_parse_state() kicks off the background worker if
	// this is the first time anything asked for this track, and returns
	// immediately either way -- it NEVER blocks this call on file I/O or
	// on a streaming feed.
	//
	// If the track isn't SUBT_PARSE_DONE yet, we return here having fed
	// nothing: the engine track is still opened (see the INIT block in
	// _get_next_ext_sub(), stream_subtitle.c, which calls
	// sub_engine_open_track() before this function), it's just empty for
	// now. video/audio keep playing. The worker sets
	// s->subtitle_ext_needs_refeed = 1 on completion (see
	// _parse_worker_thread() above), and _get_next_ext_sub()'s existing
	// per-frame check of that flag re-invokes this exact function once
	// the parse/feed is actually done -- no new plumbing needed on the
	// stream_subtitle.c side for that hand-off.
	int parse_state = _poll_parse_state( subs );
	if( parse_state != SUBT_PARSE_DONE ) {
DBG serprintf("sub_ext_feed_engine: stream %d not ready yet (state %d) -- deferring\r\n",
              stream, parse_state);
		return engine_fmt; // engine track stays open but empty; will
		                    // be re-fed once parsing/feeding completes
	}

DBG serprintf("sub_ext_feed_engine: stream %d  engine_fmt %d  is_ssa %d  is_streaming %d  parse_state %d\r\n",
              stream, engine_fmt, subs->is_ssa, subs->is_streaming, parse_state);

	if( engine_fmt == SUB_FMT_SSA && subs->is_ssa ) {
		// ASS/SSA: feed raw file buffer directly — Libass handles everything
		if( subs->raw_data && subs->raw_size > 0 && s->sub_engine ) {
			sub_engine_feed_raw( (SUB_ENGINE*)s->sub_engine,
			                     (const uint8_t*)subs->raw_data,
			                     subs->raw_size );
		}

	} else if( engine_fmt == SUB_FMT_SRT ) {
		if( subs->is_streaming ) {
			// Patch 6: already fed by the parse worker as part of reaching
			// SUBT_PARSE_DONE above (see _do_streaming_feed()). Nothing
			// left to do here.
		} else {
			// Fallback list-walk path: SMI / MicroDVD SUB / MPL2, which
			// don't implement feed() and still produce a uni_sub list via
			// their own format->parse().
			sub_line *node = subs->first;
			while( node ) {
				char merged[LINE_LEN * 2 + 4];
				if( node->top && node->bottom ) {
					snprintf( merged, sizeof(merged), "%s\\N%s", node->top, node->bottom );
				} else {
					snprintf( merged, sizeof(merged), "%s", node->top ? node->top : "" );
				}
				if( merged[0] && s->sub_engine ) {
					int duration = scale_time( s, node->end ) - scale_time( s, node->start );
					sub_engine_feed( (SUB_ENGINE*)s->sub_engine,
					                 (uint8_t*)merged, strlen(merged),
					                 scale_time( s, node->start ),
				                 duration );
				}
				node = node->next;
			}
		}
	}
	// SUB_FMT_GFX (VobSub bitmap) is handled by the existing
	// _is_ffdec_bitmap path in stream_subtitle.c — not bulk-fed here.

	return engine_fmt;
}

// For streaming (SRT/VTT) tracks: SUBT_PARSE_DONE normally means "the worker fed this track's cues into the engine", but that's invalidated when something clears the engine's events without a re-feed -- a seek's flush, or _sub_feed_should_stop() bailing mid-feed. DONE is otherwise forward-only by design (correct for format->parse() results, which a flush doesn't invalidate), so streaming tracks need this to reset DONE back to NOT_QUEUED first. _get_next_ext_sub()'s refeed branch calls this instead of stream_sub_ext_feed_engine() directly whenever subtitle_ext_needs_refeed fires; a no-op for non-streaming tracks.
int stream_sub_ext_force_streaming_refeed( STREAM *s )
{
	SUB_PRIV *p = s->subtitle_priv;
	if( p && p->subs ) {
		int stream = s->subtitle->stream;
		if( stream >= 0 && stream < p->subs->cnt ) {
			uni_sub *subs = p->subs->converted[stream];
			if( subs && subs->is_streaming ) {
				pthread_mutex_lock( &subs->parse_mutex );
				if( subs->parse_state == SUBT_PARSE_DONE ) {
					subs->parse_state = SUBT_PARSE_NOT_QUEUED;
				}
				pthread_mutex_unlock( &subs->parse_mutex );
			}
		}
	}
	return stream_sub_ext_feed_engine( s );
}

// stream_sub_ext_get_gfx_data — retained for the external VobSub bitmap
// path only. Called from stream_subtitle.c _get_next_ext_sub when
// _is_ffdec_bitmap() is true.
int stream_sub_ext_get_gfx_data( STREAM *s, VIDEO_FRAME **pframe, int time )
{
	int rst_time = TS_TO_RST_TIME(time, int);
	SUB_PRIV *p = s->subtitle_priv;
	if( !p ) return 1;

	int stream = s->subtitle->stream;
	if( stream != p->stream ) {
		p->stream = stream;
DBG serprintf("sub_ext_gfx: stream now %d\r\n", p->stream);
	}

	uni_sub *subs = p->subs->converted[stream];
	if( !subs ) return 1;

	// Parse-on-first-use, ASYNC and NON-BLOCKING, same as
	// stream_sub_ext_feed_engine() above -- for VobSub/PGS,
	// parse_IDX()/parse_SUP() is what populates subs->first (the
	// pos-index list get_gfx_IDX/get_gfx_SUP walk) plus vobsub_fd/sup_fd.
	// Unlike feed_engine (called once at track open), THIS function is
	// called once per video frame, so it doesn't need any re-feed
	// signaling to pick up a track that finishes parsing later -- it just
	// naturally sees SUBT_PARSE_DONE on a later call once the background
	// worker gets there, returning "no subtitle for this frame" (return 1,
	// same as a legitimately empty gap between cues) on every frame until
	// then. Video/audio playback is completely unaffected either way.
	int parse_state = _poll_parse_state( subs );
	if( parse_state != SUBT_PARSE_DONE || !subs->first ) return 1;

	// Walk to find the cue that covers rst_time
	sub_line *node = subs->first;
	while( node ) {
		int start = scale_time( s, node->start );
		int end   = scale_time( s, node->end );
		if( end > rst_time && start <= rst_time ) {
			VIDEO_FRAME *frame = *pframe;
			frame->valid = frame->size;
			subtitle_get_gfx( subs, node->pos, frame->data[0], &frame->valid );
			frame->time     = RST_TO_TS_TIME(start, int);
			frame->duration = RST_TO_TS_DELTA(end - start, int);
			return 0;
		}
		node = node->next;
	}
	return 1;
}
// *************************
//
// stream_sub_ext_get_engine_fmt
//
// Returns the engine format (SUB_FMT_SSA / SUB_FMT_SRT / SUB_FMT_GFX)
// for the currently active subtitle track. Called from stream_subtitle.c
// _get_next_ext_sub() at track open time.
// *************************
int stream_sub_ext_get_engine_fmt( STREAM *s )
{
	SUB_PRIV *p = s->subtitle_priv;
	if( !p ) return -1;
	int track_idx = s->av.subs;
	if( track_idx < 0 || track_idx >= SUB_TRACK_MAX ) return -1;
	return p->engine_fmt[track_idx];
}

#endif	// CONFIG_SUBTITLES
#endif  // CONFIG_STREAM

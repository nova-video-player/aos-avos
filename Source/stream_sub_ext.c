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

// -----------------------------------------------------------------------------
// s->subtitle_ext_needs_refeed is written from the parse-worker pool threads
// (this file), the seek/video thread (stream_video.c), and read+cleared on
// whichever thread drives _get_next_ext_sub() (stream_subtitle.c) -- three
// threads, one plain int, previously with no lock or atomic and therefore no
// defined cross-core visibility/ordering (a genuine data race under the C
// memory model, even though a torn read/write isn't realistically possible
// for a naturally-aligned int on any target this runs on). The flag is a
// single idempotent latch (worst case of a "lost" update is one harmless
// extra refeed poll next frame, never a correctness bug), so a full mutex
// would be overkill here -- acquire/release atomics give it the ordering it
// actually needs. Duplicated (not shared via a header, to avoid touching
// stream.h) in stream_subtitle.c and stream_video.c, the other two sites
// that touch this field.
static inline void _needs_refeed_set( STREAM *s, int val )
{
	__atomic_store_n( &s->subtitle_ext_needs_refeed, val, __ATOMIC_RELEASE );
}
static inline int _needs_refeed_get( STREAM *s )
{
	return __atomic_load_n( &s->subtitle_ext_needs_refeed, __ATOMIC_ACQUIRE );
}

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

// -----------------------------------------------------------------------------
// Per-stream owner lifetime guard, used by _owner_acquire()/_owner_release()
// below. Replaces the old process-wide "most recently active stream/worker"
// globals: subtitle_parse_enqueue_fn/priority_fn callbacks now resolve their
// target via sub->owner_ctx (a STREAM*, stamped by _queue_all_tracks())
// straight back to THIS stream's SUB_PRIV, instead of a shared guess that
// was only correct under a single-active-stream assumption. What's left to
// guard is that resolution -- STREAM* -> s->subtitle_priv -> ->worker --
// against stream_sub_ext_close() freeing `subtitle_priv`/`worker`
// concurrently.
//
// This guard (subtitle_owner_lock/cond/refs/closing) lives on STREAM
// itself, NOT inside SUB_PRIV -- see the doc comment on those fields in
// stream.h for why: a lock a caller can only reach BY FIRST dereferencing
// the very pointer it's meant to protect can't protect that dereference.
// Scoped to one stream (not the whole process), so unrelated streams never
// contend with each other.
// -----------------------------------------------------------------------------

static void _adjust_timing_for_track( STREAM *s, uni_sub *sub );

// Forward declarations -- both defined further down (they're the same
// _cue_to_engine_gen/_sub_feed_should_stop every fmt->feed() caller has always
// used), needed here for _do_streaming_feed() below, which
// _parse_worker_thread() calls.
static int  _sub_feed_should_stop( void *poll_ctx );

// Generation-checked cue-feed helper, used by both worker-thread feed paths
// (_cue_to_engine_and_cache() below, and _feed_from_cache()) that can run for
// an extended, checkpointed duration while a track switch races them --
// see sub_engine_get_track_generation()'s doc comment in sub_engine.h.
static void _cue_to_engine_gen( STREAM *s, uint64_t token, const char *text, int start_ms, int end_ms );

// poll_ctx for a streaming (SRT/VTT) feed job. Carries the uni_sub being fed (not just STREAM*) so _sub_feed_should_stop() can also detect a mid-feed track switch and bail out, rather than mixing a stale track's cues into the newly-selected track's engine. `stale` distinguishes that outcome from a genuine pause/seek/close.
typedef struct _STREAMING_POLL_CTX {
	STREAM  *s;
	uni_sub *job;
	int      stale;
	uint64_t track_gen; // engine track-generation token this pass is pinned
	                     // to; fixed at track-selection time -- see
	                     // uni_sub::track_gen's doc comment in
	                     // subtitle_format.h -- not re-derived per pass
} _STREAMING_POLL_CTX;

// -----------------------------------------------------------------------------
// SRT/VTT re-feed caching.
//
// feed_SRT()/feed_VTT() are a single-pass streaming parse: every call re-opens
// the file, re-reads it whole, and re-parses every cue from scratch, feeding
// each straight into the engine as it goes -- no cue list is kept around
// afterwards (see subtitle_srt.c's file header comment for why: it was
// designed to avoid holding a second, persistent copy of the file in memory).
// That's fine for the FIRST feed of a track. But _sub_feed_should_stop()
// aborts and flushes on every pause/seek/close (see its own comment below),
// and "abort" for this parser means "restart from the top next time" -- so
// on a long file, pausing and unpausing repeatedly re-does the full
// fopen()+fread()+parse() every single time, purely to reproduce cues the
// engine already had a moment ago.
//
// Fix: the first time a track's feed pass runs all the way to true EOF
// without being interrupted, also capture the cues into a sub_line list
// (mirroring exactly what parse() builds for every non-streaming format --
// SMI/SUB/MPL2/IDX -- into uni_sub->first/last, which is otherwise unused
// for is_streaming tracks and already generically freed by
// subtitle_free_converted() same as any other format). Every subsequent
// _do_streaming_feed() call for that track then finds subs->first already
// populated and takes the cheap path: walk the cached list straight into the
// engine (_feed_from_cache() below) instead of touching the file again --
// turning a repeated pause/seek's refeed cost from O(file size) back down to
// O(cue count), same as every other external text format already gets.
//
// A pass that gets interrupted before EOF only has a partial list -- caching
// that would silently truncate every future refeed, so it's discarded
// instead (_free_sub_line_list()) and the next call falls through to the
// slow path again, same as today, until one pass finally completes.
// -----------------------------------------------------------------------------

#define SRT_CACHE_POLL_INTERVAL 200	// matches SRT_POLL_INTERVAL/VTT_POLL_INTERVAL in subtitle_srt.c/subtitle_vtt.c

static void _free_sub_line_list( sub_line *node )
{
	while( node ) {
		sub_line *next = node->next;
		if( node->top )    afree( node->top );
		if( node->bottom ) afree( node->bottom );
		afree( node );
		node = next;
	}
}

// ctx for _cue_to_engine_and_cache(): feeds the engine exactly like
// _cue_to_engine_gen() while also appending each cue to an in-memory list, so a
// first, currently-in-progress feed pass can be captured as a cache without
// changing feed_SRT()/feed_VTT()'s signature or making a second pass.
typedef struct _CACHE_BUILD_CTX {
	STREAM   *s;
	sub_line *first;
	sub_line *last;
	int       alloc_failed; // set if any node/text allocation failed mid-pass;
	                         // a cache built while this is set must never be
	                         // committed -- see the commit site in
	                         // _do_streaming_feed() below.
	uint64_t  track_gen; // see _STREAMING_POLL_CTX::track_gen above
} _CACHE_BUILD_CTX;

static void _cue_to_engine_and_cache( void *ctx, const char *text, int start_ms, int end_ms )
{
	_CACHE_BUILD_CTX *cc = (_CACHE_BUILD_CTX *)ctx;

	_cue_to_engine_gen( cc->s, cc->track_gen, text, start_ms, end_ms );

	if( !text || !text[0] ) return; // nothing worth caching -- same filter _cue_to_engine_gen() applies

	sub_line *node = acalloc( 1, sizeof( sub_line ) );
	if( !node ) {
		// OOM building the cache: the engine already got this cue above via
		// _cue_to_engine_gen(), so playback is unaffected, but the cache we're
		// building is now missing a cue and must not be committed as if it
		// were complete -- see the commit site below.
		cc->alloc_failed = 1;
		return;
	}
	node->top   = astrdup( text );
	if( !node->top ) {
		// astrdup() failure: same reasoning as above -- this node would
		// silently carry a NULL ->top forever (skipped by _feed_from_cache()'s
		// `if (node->top)` check), permanently dropping this cue from every
		// future replay. Discard the node and flag the pass as incomplete.
		afree( node );
		cc->alloc_failed = 1;
		return;
	}
	node->start = start_ms;
	node->end   = end_ms;
	if( !cc->first ) {
		cc->first = cc->last = node;
	} else {
		cc->last->next = node;
		node->prev      = cc->last;
		cc->last        = node;
	}
}

// Cheap refeed path: subs->first/last already holds a complete cached cue
// list from an earlier uninterrupted feed pass -- walk it straight into the
// engine instead of re-reading and re-parsing the file. Mirrors the existing
// SMI/SUB/MPL2 list-walk path in stream_sub_ext_feed_engine(), plus the same
// periodic yield/stop-check feed_SRT()/feed_VTT() do, so a very large cached
// track can't hog eng->lock during a refeed either. Return contract matches
// _do_streaming_feed(): 1 = ran to completion (or was cleanly interrupted --
// the cache itself is untouched either way, so there's nothing to discard),
// 2 = track went stale mid-walk, caller must not mark the job DONE.
//
// `token` is the engine track-generation captured once by the caller before
// this walk started (see sub_engine_get_track_generation()) -- used for
// every cue/flush below so a track switch mid-walk can't land a leftover
// cue in the new backend or flush cues the new track's own feed just added.
static int _feed_from_cache( STREAM *s, uni_sub *subs, uint64_t token )
{
	_needs_refeed_set( s, 0 );

	int n = 0;
	sub_line *node = subs->first;
	while( node ) {
		SUB_PRIV *p = s->subtitle_priv;
		int still_current = s->subtitle && p && p->subs &&
		                     s->subtitle->stream >= 0 && s->subtitle->stream < p->subs->cnt &&
		                     p->subs->converted[ s->subtitle->stream ] == subs;
		if( !still_current ) {
			if( s->sub_engine ) sub_engine_flush_gen( (SUB_ENGINE*)s->sub_engine, token );
			return 2;
		}

		if( (++n % SRT_CACHE_POLL_INTERVAL) == 0 ) {
			if( thread_state_asked( &s->sub_tstate ) != THREAD_RUNNING ) {
				if( s->sub_engine ) sub_engine_flush_gen( (SUB_ENGINE*)s->sub_engine, token );
				_needs_refeed_set( s, 1 );
				return 1;
			}
			stream_yield_RT();
		}

		// _cue_to_engine_and_cache() only ever populates ->top (the cue text
		// already has any multi-line \N embedded by feed_SRT()/feed_VTT()
		// before it reaches the callback) -- ->bottom stays NULL for every
		// cached node, so unlike the SMI/SUB/MPL2 fallback walk in
		// stream_sub_ext_feed_engine() there's no separate half to merge.
		if( node->top ) {
			_cue_to_engine_gen( s, token, node->top, node->start, node->end );
		}

		node = node->next;
	}
	return 1;
}

// Runs SRT/VTT's streaming feed() on the parse-worker thread instead of stream_sub_dec_thread (see file header comment), reusing the same _cue_to_engine_gen()/_sub_feed_should_stop() every feed() caller uses. Returns 1 if the whole file was fed or a genuine pause/seek/close interrupted it (subtitle_ext_needs_refeed is set for the latter); 2 if `subs` was no longer the actively selected track, so the caller must NOT mark the job DONE; 0 only on a genuine setup failure (maps to SUBT_PARSE_FAILED).
static int _do_streaming_feed( STREAM *s, uni_sub *subs )
{
	if( !s || !subs || !subs->spex ) return 0;

	// Skip if the user already switched away while this job sat in the queue -- nothing fed yet, nothing to flush.
	SUB_PRIV *p = s->subtitle_priv;
	int still_current = s->subtitle && p && p->subs &&
	                     s->subtitle->stream >= 0 && s->subtitle->stream < p->subs->cnt &&
	                     p->subs->converted[ s->subtitle->stream ] == subs;
	if( !still_current ) return 2;

	// Fixed at track-SELECTION time, not discovered here: subs->track_gen
	// was stamped by stream_sub_ext_set_track_generation() synchronously,
	// on the selecting thread, right after the sub_engine_open_track()
	// call that made this track active -- BEFORE this job could possibly
	// be enqueued onto the worker thread this function now runs on (see
	// uni_sub::track_gen's doc comment in subtitle_format.h). This used to
	// be a fresh, separate sub_engine_get_track_generation() call made
	// right here instead: a real TOCTOU against the still_current check
	// just above -- a track switch landing in the gap between that check
	// and this (former) call could hand this pass a NEWER generation than
	// the track it just confirmed itself still selected for, letting a
	// stale worker's cues/flush pass the engine's own token check and land
	// on the wrong (new) backend as if they belonged to it. Reading a value
	// fixed before this job ever entered the queue closes that gap
	// entirely -- there is no later point at which it could observe a
	// different generation than the one this track actually opened with.
	//
	// Locked: subs->track_gen is written by stream_sub_ext_set_track_generation()
	// on the selecting/driver thread and read here on the worker thread, so
	// it's a genuine cross-thread field, not just cross-call -- a plain,
	// unguarded uint64_t read/write pair here is a real data race (and on a
	// 32-bit target, not even guaranteed to read back a value either side
	// ever actually wrote: a concurrent write could tear it). This is
	// exactly the same "SELECTED track's uni_sub can be re-stamped by a
	// quick switch-away-and-back while a job for it still sits queued"
	// scenario the comment above already describes -- re-selecting the
	// SAME track reuses the SAME cached uni_sub, so the write and this read
	// really can land on the same object concurrently. sub->parse_mutex
	// already exists to guard exactly this kind of mutable per-job state on
	// uni_sub (see its doc comment in subtitle_format.h); reusing it here
	// avoids adding a second lock (or a platform-specific atomic type) just
	// for this one field. The snapshot taken under the lock is then used
	// as a plain, unguarded local for the rest of this (possibly long)
	// checkpointed pass -- correct, since nothing else writes to a LOCAL
	// variable.
	pthread_mutex_lock( &subs->parse_mutex );
	uint64_t token = subs->track_gen;
	pthread_mutex_unlock( &subs->parse_mutex );

	// Fast path -- see the caching comment above. Skips the file entirely.
	if( subs->first ) {
		return _feed_from_cache( s, subs, token );
	}

	SUBTITLE_FORMAT *fmt = subtitle_get_format_for_sub( subs );
	if( !fmt || !fmt->feed ) return 0;

	// Cleared before the call, not after: _sub_feed_should_stop() may set it back to 1 during the call, and that must survive.
	_needs_refeed_set( s, 0 );

	_STREAMING_POLL_CTX poll_ctx  = { s, subs, 0, token };
	_CACHE_BUILD_CTX     cache_ctx = { s, NULL, NULL, 0, token };
	fmt->feed( subs->spex, _cue_to_engine_and_cache, &cache_ctx, _sub_feed_should_stop, &poll_ctx );

	if( poll_ctx.stale ) {
		// Track switched away mid-feed -- nothing was left in the engine
		// for it (see _sub_feed_should_stop()'s comment), and the partial
		// list we were accumulating never validly ran against the
		// currently-selected track either; discard it so a genuine future
		// re-selection of this track starts the cache fresh.
		_free_sub_line_list( cache_ctx.first );
		return 2;
	}

	if( _needs_refeed_get( s ) ) {
		// Genuinely interrupted by pause/seek/close partway through --
		// _sub_feed_should_stop() set the flag back to 1 (see its comment).
		// The list built so far only covers cues up to the interruption
		// point; caching a truncated list would silently drop the tail of
		// the file on every future refeed, so discard it. The next
		// _do_streaming_feed() call for this track falls through to this
		// same slow path and tries again from scratch.
		_free_sub_line_list( cache_ctx.first );
	} else if( cache_ctx.alloc_failed ) {
		// Ran to true EOF, but at least one cue along the way failed to
		// allocate -- the engine still got every cue via _cue_to_engine_gen(),
		// but the list itself is missing one, so it is NOT a complete,
		// trustworthy cache. Committing it anyway would make that cue
		// silently vanish from every future refeed of this track. Discard
		// and let the next call retry the slow path, same as a genuine
		// interruption above.
		_free_sub_line_list( cache_ctx.first );
	} else if( cache_ctx.first ) {
		// Ran to true EOF, uninterrupted, no allocation failures -- the
		// cache is complete and trustworthy. Commit it so every future
		// refeed of this track (pause/seek, or a later re-selection) takes
		// the fast path above.
		subs->first = cache_ctx.first;
		subs->last  = cache_ctx.last;
	}

	return 1;
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
			_needs_refeed_set( w->stream, 1 );
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

// -----------------------------------------------------------------------------
// Owner resolution for subtitle_parse_enqueue_fn/subtitle_parse_priority_fn.
//
// subtitle_ensure_parsed_async()/_priority() call these with only a `uni_sub*`
// -- subtitle_formats.c deliberately has no STREAM*/SUB_PRIV* parameter here,
// to stay decoupled from stream_sub_ext.c's types. This used to be solved
// with a process-wide "most recently active stream/worker" pointer pair,
// which was only correct under a single-active-stream assumption: with two
// STREAMs alive at once, a job belonging to one could be resolved against
// the other's worker.
//
// Fix: every uni_sub now carries owner_ctx (stamped by _queue_all_tracks(),
// see its comment), an opaque STREAM* that IS this job's actual owner. These
// helpers resolve straight from that STREAM* to its own SUB_PRIV/worker --
// no cross-stream guessing possible. What's left to guard is the same
// lifetime hazard as before (stream_sub_ext_close() freeing SUB_PRIV out
// from under a concurrent resolve) -- guarded now by s->subtitle_owner_lock
// (a STREAM field, not a SUB_PRIV one -- see its doc comment in stream.h),
// scoped to the owning stream so unrelated streams never contend with each
// other.
// -----------------------------------------------------------------------------

// Snapshot `s`'s worker (may be NULL) and mark this stream's subtitle_priv
// "in use". Tri-state: _worker_enqueue()/_worker_prioritize() need to react
// differently depending on why an acquire didn't hand back a usable worker
// (see their call sites).
//
// _OWNER_ACQUIRED: *w_out set. _OWNER_CLOSED: owner exists but has no live
// subtitle_priv right now -- caller must cancel the job, not touch any
// other stream state, and start no new work. _OWNER_NONE: owner_ctx was
// never stamped at all (`s` itself is NULL) -- nothing has ever owned this
// job, so a direct synchronous parse/feed is the only way it completes.
//
// EVERY non-_OWNER_NONE result takes a reference (subtitle_owner_refs++) --
// including _OWNER_CLOSED. That's deliberate, not an oversight: a caller
// that gets _OWNER_CLOSED still needs to safely touch `sub` afterwards (at
// minimum, lock sub->parse_mutex to cancel it), and stream_sub_ext_close()'s
// own drain-wait is what keeps `sub`/subtitle_priv alive long enough for
// that -- but ONLY if this acquire is counted in subtitle_owner_refs like
// any other. A version of this that returned "closing, no reference" would
// let close() sail straight through an empty drain-wait and free `sub` out
// from under the caller's very next touch of it -- a real gap that existed
// here before this comment was written; every _OWNER_CLOSED caller MUST
// pair it with exactly one _owner_release(s) call, same as _OWNER_ACQUIRED,
// once it's done touching `sub`.
//
// _OWNER_CLOSED also covers `s->subtitle_priv == NULL` with
// subtitle_owner_closing already back to 0 (a fully-finished prior close,
// not just an in-progress one) -- NOT folded into _OWNER_NONE. An `s` that
// once had a subtitle_priv and now doesn't is a stream whose subtitle state
// has been closed, and no new work should start against it, exactly like
// the mid-close case; the only case that should ever fall back to a
// synchronous parse is `s` being NULL, i.e. this job never had an owner to
// begin with.
//
// Locking s->subtitle_owner_lock BEFORE even looking at s->subtitle_priv is
// what makes this safe against a concurrent stream_sub_ext_close(): that
// lock's lifetime is the STREAM's own, so unlike a lock that lived inside
// SUB_PRIV, there's no window where close() could have already destroyed
// the very lock this function is about to take -- it belongs to `s`, which
// is guaranteed valid for the whole of this call, not to `*s->subtitle_priv`,
// which is exactly what's being read/invalidated here.
typedef enum { _OWNER_ACQUIRED, _OWNER_CLOSED, _OWNER_NONE } _owner_status;

static _owner_status _owner_acquire( STREAM *s, SUB_PARSE_WORKER **w_out )
{
	if( !s ) return _OWNER_NONE;

	pthread_mutex_lock( &s->subtitle_owner_lock );
	if( s->subtitle_owner_closing || !s->subtitle_priv ) {
		s->subtitle_owner_refs++; // see the long comment above for why this
		                          // branch takes a reference too
		pthread_mutex_unlock( &s->subtitle_owner_lock );
		return _OWNER_CLOSED;
	}
	*w_out = ((SUB_PRIV *)s->subtitle_priv)->worker; // subtitle_priv is `void*`
	                                                  // in stream.h (SUB_PRIV
	                                                  // is private to this
	                                                  // file) -- needs the cast
	s->subtitle_owner_refs++;
	pthread_mutex_unlock( &s->subtitle_owner_lock );
	return _OWNER_ACQUIRED;
}

static void _owner_release( STREAM *s )
{
	if( !s ) return;
	pthread_mutex_lock( &s->subtitle_owner_lock );
	s->subtitle_owner_refs--;
	if( s->subtitle_owner_refs == 0 ) {
		pthread_cond_broadcast( &s->subtitle_owner_cond );
	}
	pthread_mutex_unlock( &s->subtitle_owner_lock );
}

// Shared "no active worker"/"queue full" fallback for _worker_enqueue()/_worker_prioritize(). subtitle_do_parse() alone is wrong for streaming (SRT/VTT) tracks since format->parse() is NULL for them -- it would mark them FAILED without ever feeding. `s` may be NULL if sub->owner_ctx was never stamped, in which case a streaming track genuinely can't be fed and the fallback is a real FAILED.
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

// Drops a job that must NOT be run rather than parsing/feeding it -- used
// instead of _synchronous_parse_fallback() when _owner_acquire() returns
// _OWNER_CLOSED (see that enum's comment): there is no safe "parse against
// `s`" to fall back to there, since `s`'s subtitle state is either being
// freed concurrently or already gone. Marks a definite terminal state so
// anything polling SUBT_PARSE_* (_poll_parse_state() and friends) sees
// FAILED rather than hanging in QUEUED forever. Safe to call here
// specifically because the caller is holding the subtitle_owner_refs
// reference _owner_acquire() took for the _OWNER_CLOSED case -- see its
// comment -- which is what keeps `sub` itself alive for this call.
static void _cancel_job( uni_sub *sub )
{
	pthread_mutex_lock( &sub->parse_mutex );
	sub->parse_state = SUBT_PARSE_FAILED;
	pthread_mutex_unlock( &sub->parse_mutex );
}

static void _worker_enqueue( uni_sub *sub )
{
	STREAM *owner = (STREAM *)sub->owner_ctx;
	SUB_PARSE_WORKER *w = NULL;

	_owner_status st = _owner_acquire( owner, &w );
	if( st == _OWNER_CLOSED ) {
		// Owner exists but has no live subtitle state right now (mid-close
		// or already fully closed -- see _owner_acquire()'s doc comment) --
		// there is no safe "parse synchronously against `owner`" to fall
		// back to here, since that's exactly the state being freed
		// concurrently (or already gone). Cancel the job instead of racing
		// the teardown or starting new work against a closed stream.
		// _owner_acquire() took a reference for this branch specifically so
		// `sub` is still safe to touch here -- release it once we're done.
DBG serprintf("_worker_enqueue: owner has no live subtitle state, dropping job\n");
		_cancel_job( sub );
		_owner_release( owner );
		return;
	}
	if( st == _OWNER_NONE ) {
		// owner_ctx was never stamped (called before any
		// _queue_all_tracks() ever ran for this sub -- shouldn't happen
		// in practice, since stamping always precedes handing a sub to
		// subtitle_ensure_parsed_async()) -- there's no SUB_PRIV that ever
		// existed to resolve a worker against, so fall back to a direct
		// synchronous parse/feed right here rather than dropping the job.
		// subtitle_ensure_parsed_async() already transitioned this track
		// to QUEUED before calling us, so we own running it. owner is NULL
		// in this branch (that's what _OWNER_NONE means); no reference was
		// taken, so nothing to release.
DBG serprintf("_worker_enqueue: no owning stream, parsing synchronously\n");
		_synchronous_parse_fallback( owner, sub );
		return;
	}

	if( !w ) {
		// Owner resolved, but its worker hasn't been created yet (e.g.
		// _queue_all_tracks() ran before _worker_ensure_started() -- see
		// call order in _stream_sub_ext_check_core()) -- same synchronous
		// fallback, still against the correct owning stream this time.
DBG serprintf("_worker_enqueue: owner has no worker yet, parsing synchronously\n");
		_synchronous_parse_fallback( owner, sub );
		_owner_release( owner );
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
		_owner_release( owner );
		return;
	}
	int tail = (w->queue_head + w->queue_count) % PARSE_QUEUE_MAX;
	w->queue[tail] = sub;
	w->queue_count++;
	pthread_cond_signal( &w->queue_cond );
	pthread_mutex_unlock( &w->queue_mutex );
	_owner_release( owner );
}

// Registered with subtitle_formats.c via subtitle_set_parse_priority_fn(), called for a track that was just actively selected (vs. passively warmed up by _queue_all_tracks(), which still uses the regular tail-insert enqueue). Two cases: if `sub` isn't queued yet, head-insert it (subtitle_ensure_parsed_async_priority() just flipped it to QUEUED); if it's already somewhere in the queue -- the common case, since every track is eagerly warmed up before the user picks one -- find it via linear scan (bounded by PARSE_QUEUE_MAX) and shift it to the front.
static void _worker_prioritize( uni_sub *sub )
{
	STREAM *owner = (STREAM *)sub->owner_ctx;
	SUB_PARSE_WORKER *w = NULL;

	_owner_status st = _owner_acquire( owner, &w );
	if( st == _OWNER_CLOSED ) {
		// See the identical branch in _worker_enqueue() above -- including
		// why this releases the reference _owner_acquire() took for us.
DBG serprintf("_worker_prioritize: owner has no live subtitle state, dropping job\n");
		_cancel_job( sub );
		_owner_release( owner );
		return;
	}
	if( st == _OWNER_NONE ) {
DBG serprintf("_worker_prioritize: no owning stream, parsing synchronously\n");
		_synchronous_parse_fallback( owner, sub );
		return;
	}

	if( !w ) {
DBG serprintf("_worker_prioritize: owner has no worker yet, parsing synchronously\n");
		_synchronous_parse_fallback( owner, sub );
		_owner_release( owner );
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
		_owner_release( owner );
		return;
	}

	// Not currently in the queue -- a fresh NOT_QUEUED->QUEUED transition; head-insert it, same queue-full fallback as _worker_enqueue().
	if( w->queue_count >= PARSE_QUEUE_MAX ) {
		pthread_mutex_unlock( &w->queue_mutex );
serprintf("_worker_prioritize: queue full, parsing synchronously\n");
		_synchronous_parse_fallback( w->stream, sub );
		_owner_release( owner );
		return;
	}
	w->queue_head = (w->queue_head - 1 + PARSE_QUEUE_MAX) % PARSE_QUEUE_MAX;
	w->queue[w->queue_head] = sub;
	w->queue_count++;
	pthread_cond_signal( &w->queue_cond );
	pthread_mutex_unlock( &w->queue_mutex );
	_owner_release( owner );
}

// Signals every pool thread to exit and joins each. Called from stream_sub_ext_close(), AFTER the caller has already unpublished and drained s->subtitle_owner_lock's refs (see that function) -- so by the time we get here, no _worker_enqueue()/_worker_prioritize() call can still be holding a reference that resolves to this `w`, and it's safe to tear it down unconditionally. Jobs still queued are simply abandoned -- no "drain the queue first" wait, since that would reintroduce the blocking-on-parse this worker exists to avoid. In-flight jobs finishing after tstate flips to EXIT is fine: each only touches its own `job` (about to be freed regardless) and w->stream (null-checked in _parse_worker_thread against a racing close()).
static void _worker_stop( SUB_PARSE_WORKER *w )
{
	if( !w ) return;

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
	// Locked for the whole loop, not per-entry: valid/av.subs_max are set
	// before name/path within one entry, so a reader needs the whole batch
	// atomic. See subtitle_table_lock in stream.h.
	pthread_mutex_lock( &s->subtitle_table_lock );
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
	pthread_mutex_unlock( &s->subtitle_table_lock );
}

// Kicks off async parsing for every unstarted track in p->subs->converted[start_idx..cnt), right after the menu is (re)built -- so all discovered files start parsing on the background worker before the user picks a track. Non-blocking: subtitle_ensure_parsed_async() just enqueues and returns, so this doesn't hold up video start.
//
// Also stamps sub->owner_ctx = s on every track in the range, BEFORE handing
// any of them to subtitle_ensure_parsed_async() -- that's what lets
// _worker_enqueue()/_worker_prioritize() (registered as the enqueue/priority
// fn) recover the job's actual owning STREAM later, instead of guessing via
// a shared "most recently active stream" global. Streaming (SRT/VTT) tracks
// are stamped too even though they're skipped here (see below): they still
// reach subtitle_ensure_parsed_async_priority() later, at selection time,
// via stream_sub_ext_feed_engine(), and need owner_ctx set by then.
static void _queue_all_tracks( STREAM *s, converted_subs *subs, int start_idx )
{
	if( !subs ) return;
	int i;
	for( i = start_idx; i < subs->cnt; i++ ) {
		if( !subs->converted[i] ) continue;
		subs->converted[i]->owner_ctx = s;
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

	if( s->subtitle_priv ) {
		// A live SUB_PRIV already exists -- this is stream_sub_ext_check()
		// re-invoked directly, without an intervening stream_sub_ext_close()
		// (the normal path, via stream_sub_ext_update(), always closes
		// first). Tear it down through the exact same full lifecycle
		// stream_sub_ext_close() uses everywhere else -- waiting out the
		// discovery worker, draining any in-flight _owner_acquire()
		// callers, stopping the parse-worker pool, and freeing
		// p->files/p->subs -- rather than a hand-rolled shortcut here. A
		// bare memset() used to stand in for all of that: it leaked the
		// worker pool (its threads kept right on running against a
		// queue/subs list this function was about to replace out from
		// under them, with no _worker_stop() ever called), leaked
		// p->files/p->subs outright, and clobbered subtitle_owner_refs/
		// owner_closing while a concurrent _owner_acquire() could still be
		// live against them.
		stream_sub_ext_close( s );
	}

	s->subtitle_priv = amalloc( sizeof( SUB_PRIV ) );
	if( !s->subtitle_priv ) {
		subtitle_free_files( files );
		return 1;
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

	// Start the background parse worker for this stream, register the
	// (stream-agnostic, process-wide) enqueue/priority callbacks -- idempotent,
	// safe to call again if some other stream already registered them, since
	// _worker_enqueue()/_worker_prioritize() resolve their actual target
	// per-job via sub->owner_ctx rather than any state captured here -- then
	// immediately queue every discovered track for this stream -- see
	// _queue_all_tracks() -- so parsing for ALL of them starts now, off this
	// thread, in parallel with video startup. _queue_all_tracks() stamps
	// owner_ctx = s on each track BEFORE handing it to
	// subtitle_ensure_parsed_async(), so by the time any callback runs for
	// one of these jobs, it already carries everything it needs to resolve
	// straight back to this stream's own SUB_PRIV/worker -- no process-wide
	// "which stream is active" state needed at all.
	if( _worker_ensure_started( s, p ) ) {
		subtitle_set_parse_enqueue_fn( _worker_enqueue );
		subtitle_set_parse_priority_fn( _worker_prioritize ); // patch 7
	}
	_queue_all_tracks( s, p->subs, 0 );

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
				_queue_all_tracks( s, p->subs, old_cnt );
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
		// Unpublish this SUB_PRIV first, then block until every reference
		// taken BEFORE this unpublish (via _owner_acquire(), e.g. from a
		// racing _worker_enqueue()/_worker_prioritize()/
		// _synchronous_parse_fallback() call for one of this stream's own
		// tracks) has been released, before touching p->worker again.
		// Setting subtitle_owner_closing alone only stops NEW acquires --
		// it does nothing for a thread that already snapshotted p->worker
		// a moment earlier and hasn't called _owner_release() yet. Without
		// this wait, that thread could still be dereferencing p->worker
		// (queue state, w->stream, etc.) after _worker_stop() below frees
		// it. Bounded: nothing new can start an acquire against THIS
		// stream's subtitle state once subtitle_owner_closing is set, so
		// this only waits out whatever calls (for this stream's own
		// tracks) were already in flight at this instant -- and since
		// owner_ctx always points back to its own stream, an unrelated
		// stream's in-flight fallback call can never be holding a
		// reference here to begin with.
		//
		// subtitle_owner_lock/cond/refs/closing live on `s` itself, not on
		// `p` -- see their doc comment in stream.h -- so unlike before,
		// nothing here needs to (or may) init/destroy them: they were set
		// up once for this STREAM's whole lifetime back in stream_init().
		pthread_mutex_lock( &s->subtitle_owner_lock );
		s->subtitle_owner_closing = 1;
		while( s->subtitle_owner_refs > 0 ) {
			pthread_cond_wait( &s->subtitle_owner_cond, &s->subtitle_owner_lock );
		}
		pthread_mutex_unlock( &s->subtitle_owner_lock );

		// Stop the parse worker, before freeing the uni_sub entries it might still be touching -- subtitle_do_parse() writes into a uni_sub's fields with no awareness that close() might run concurrently; freeing under an in-flight parse would be a use-after-free. _worker_stop() joins every pool thread, so this can block up to the slowest in-flight job's remaining time (they already run concurrently, so this isn't their sum); queued-but-not-started jobs are simply abandoned. A one-time, bounded wait at teardown, not a recurring stall -- thread cancellation was considered and rejected since killing a thread mid-fopen()/malloc() risks leaks/corruption, same reasoning as every other thread teardown in this codebase. Safe to call now unconditionally: the drain above already guarantees no _worker_enqueue()/_worker_prioritize() call can still be resolving against this worker.
		_worker_stop( p->worker );
		p->worker = NULL;

		int i;
		// Locked (subtitle_table_lock, stream.h); scoped to just this
		// loop, not held across _worker_stop()'s join above.
		pthread_mutex_lock( &s->subtitle_table_lock );
		for (i = p->prev_max; i < s->av.subs_max; ++i) {
			SUB_PROPERTIES *sub = s->av.sub + i;
			memset(sub, 0, sizeof(SUB_PROPERTIES));
		}
		s->av.subs_max = p->prev_max;
		pthread_mutex_unlock( &s->subtitle_table_lock );
		if( p->files )
			subtitle_free_files( p->files );
		if( p->subs )
			subtitle_free_converted( p->subs );
		afree( s->subtitle_priv );
		s->subtitle_priv = NULL;

		// Teardown is fully complete -- reset the latch so a FUTURE
		// SUB_PRIV on this same STREAM (the next video, or a fresh
		// stream_sub_ext_check()) can be acquired against again.
		// subtitle_owner_closing is a per-SUB_PRIV-lifecycle latch even
		// though it now lives on the (reused, longer-lived) STREAM, so it
		// must not stay set past the teardown it was raised for, or every
		// _owner_acquire() for this stream would fail forever after the
		// very first close().
		pthread_mutex_lock( &s->subtitle_owner_lock );
		s->subtitle_owner_closing = 0;
		pthread_mutex_unlock( &s->subtitle_owner_lock );
	}
}

// _cue_to_engine_gen — sub_cue_cb-shaped cue feeder, gated on the engine's
// track-generation token (see sub_engine_get_track_generation()'s doc
// comment in sub_engine.h). Used by both worker-thread feed paths
// (_cue_to_engine_and_cache() and _feed_from_cache()), which can run for an
// extended, checkpointed duration while a track switch races them. The
// synchronous SMI/SUB/MPL2 fallback walk in stream_sub_ext_feed_engine()
// calls sub_engine_feed() directly instead -- it's a single quick pass on
// the same thread that just opened the track, not a long-lived background
// worker, so it isn't exposed to the same overlap.
static void _cue_to_engine_gen( STREAM *s, uint64_t token, const char *text, int start_ms, int end_ms )
{
	if( !s || !s->sub_engine || !text || !text[0] ) return;
	int duration = end_ms - start_ms;
	if( duration < 0 ) duration = 0;
	sub_engine_feed_gen( (SUB_ENGINE*)s->sub_engine, token,
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
		if( s->sub_engine ) sub_engine_flush_gen( (SUB_ENGINE*)s->sub_engine, pc->track_gen );
		pc->stale = 1;
		return 1;
	}

	if( thread_state_asked( &s->sub_tstate ) != THREAD_RUNNING ) {
		if( s->sub_engine ) sub_engine_flush_gen( (SUB_ENGINE*)s->sub_engine, pc->track_gen );
		_needs_refeed_set( s, 1 );
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
    if( !s || !s->subtitle || !pframe || !*pframe ) return 1;
	int rst_time = TS_TO_RST_TIME(time, int);
	SUB_PRIV *p = s->subtitle_priv;
	if( !p || !p->subs ) return 1;

	int stream = s->subtitle->stream;
	if( stream < 0 || stream >= p->subs->cnt ) return 1;
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

// *************************
//
// stream_sub_ext_set_track_generation
//
// Stamps the engine track-generation token sub_engine_open_track() just
// returned (its `out_generation` out-param -- see sub_engine.h) onto the
// currently-selected external track's uni_sub -- see uni_sub::track_gen's
// doc comment in subtitle_format.h. Called from stream_subtitle.c's
// _get_next_ext_sub(), synchronously, on the same (selecting) thread, right
// after the open_track() call that made this track active and BEFORE
// stream_sub_ext_feed_engine() (called immediately after, same thread) can
// enqueue this track's streaming feed job onto the background parse-worker
// pool -- so by the time that job could possibly run on the worker thread,
// this write has already happened-before it via the job queue's own
// mutex/cond, same as everything else handed to a queued job.
//
// Deliberately indexed the same way _do_streaming_feed() and friends
// resolve "the currently selected external track" (s->subtitle->stream into
// p->subs->converted[]), not stream_sub_ext_get_engine_fmt()'s s->av.subs
// into p->engine_fmt[] -- those are two different indices into two
// different arrays; see the comment on SUB_PRIV's engine_fmt field.
//
// No-op if there's no live SUB_PRIV, or the current track index doesn't
// resolve to a real, converted uni_sub right now -- callers don't need to
// pre-check either; this is exactly the same "still current?" question
// _do_streaming_feed() itself asks before ever reading track_gen back.
//
// The write itself is under subs->parse_mutex, guarding against a racing
// worker-thread read in _do_streaming_feed() -- see that read's own comment
// (subtitle_format.h) for why this specific field needs it: re-selecting
// the SAME streaming track reuses the SAME cached uni_sub, so a quick
// switch-away-and-back can call this function again for a job that's still
// sitting queued (or running) with the OLD token.
//
// *************************
void stream_sub_ext_set_track_generation( STREAM *s, uint64_t token )
{
	if( !s || !s->subtitle ) return;
	SUB_PRIV *p = s->subtitle_priv;
	if( !p || !p->subs ) return;
	int stream = s->subtitle->stream;
	if( stream < 0 || stream >= p->subs->cnt ) return;
	uni_sub *subs = p->subs->converted[stream];
	if( !subs ) return;
	pthread_mutex_lock( &subs->parse_mutex );
	subs->track_gen = token;
	pthread_mutex_unlock( &subs->parse_mutex );
}

#endif	// CONFIG_SUBTITLES
#endif  // CONFIG_STREAM

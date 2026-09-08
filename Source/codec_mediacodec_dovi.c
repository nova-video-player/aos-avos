/*
 * Copyright (C) 2026 The Nova Video Player Project
 *
 * Dolby Vision tone-map mode, hardware path (S5).
 *
 * MediaCodec decodes the HEVC base layer into an AImageReader-backed
 * surface; each output frame is exported as an AHardwareBuffer descriptor
 * and rendered by dovi_gl through the mpv hwdec_aimagereader import chain
 * (AHardwareBuffer -> eglGetNativeClientBufferANDROID -> EGLImage ->
 * GL_TEXTURE_EXTERNAL_OES -> libplacebo).
 *
 * The Dolby Vision RPU (HEVC NAL type 62) is extracted from every input
 * access unit before MediaCodec sees it (the DV NALs are stripped) and is
 * paired with the decoded frame by timestamp; the renderer parses it with
 * libdovi (pl_hdr_metadata_from_dovi_rpu) so the dynamic L1 trim metadata
 * drives the HDR10 tone-map.
 *
 * Scope: profiles whose base layer is HDR10-compatible (profile 8.x, and
 * profile 7 without an enhancement layer). FEL content (profile 7 with
 * EL) needs YUV-domain reshaping/NLQ composition, which the OES import
 * cannot provide (the GPU sampler has already converted to RGB); such
 * files fail open() here and fall back to the software decoder, which
 * implements the full mpv-parity pipeline.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "debug.h"
#include "av.h"
#include "stream.h"
#include "stream_dec_video.h"
#include "stream_config.h"

#if defined( CONFIG_ANDROID ) && defined( CONFIG_DOVI_TONEMAP )

#include <unistd.h>
#include <dlfcn.h>
#include <android/api-level.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <libavutil/mem.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavcodec/avcodec.h>

#include "dovi_nal.h"
#include "dovi_rpu_meta.h"
#include "dovi_gl.h"
#include "frame_q.h"

/* NdkMediaImageReader.h is not exposed at APP_PLATFORM 21; the API is
 * dlopen/dlsym'd at runtime (API 26+), so only the opaque types and the
 * format constant are needed here. */
typedef struct AImageReader AImageReader;
typedef struct AImage AImage;
#ifndef AIMAGE_FORMAT_PRIVATE
#define AIMAGE_FORMAT_PRIVATE 0x22
#endif

/* MediaCodec color formats for the byte-buffer (copy) EL/BL decode path —
 * mirrors the values FFmpeg's mediacodecdec_common.c handles */
#define DVHW_COLOR_FormatYUV420Planar            19
#define DVHW_COLOR_FormatYUV420SemiPlanar         21
#define DVHW_COLOR_FormatYUVP010                   0x36
#define DVHW_COLOR_QCOM_FormatYUV420SemiPlanar32m 0x7FA30C04

/* libavos.c: user preference (0 = passthrough) */
extern int libavos_get_dolby_vision_mode(void);
/* stream_sink_video_dovi.c */
extern STREAM_SINK_VIDEO *stream_sink_video_dovi_new(void *surface);

#define TAG "DOVI_HW"

#define DVHW_PENDING_MAX 64
#define DVHW_INPUT_TIMEOUT_US 0	/* never block the engine on a full
				 * codec input queue: a full input queue means the codec's
				 * outputs are not being drained fast enough - blocking here
				 * just caps the whole pipeline at ~1/timeout fps (measured
				 * 7fps on 48fps FEL with 100ms); drop the AU like mpv's
				 * mediacodec wrapper does under backpressure and let the
				 * engine pace itself against the sink pool */
#define DVHW_EL_INPUT_TIMEOUT_US 0	/* EL feed must never block: its input
					 * queue fills while the BL paces the engine; outputs are
					 * drained non-blocking every call */
#define DVHW_EL_CATCHUP 4		/* EL reorder depth: the EL decoder emits
					 * an AU a few AUs after it is fed, so the
					 * fed-vs-drained budget may run this many
					 * AUs ahead of the 1:1 BL pace */

/* AImage entry points (API 26+), resolved at runtime so libavos.so still
 * loads on older devices. Only AImage_delete stays live (dvhw_frame_release
 * frees stale recycled-frame images from the pre-buffer-mode era of the
 * pool); the former AImageReader surface pipeline was removed with the
 * OES path - the BL now decodes in MediaCodec buffer mode like mpv's
 * mediacodec-copy. */
typedef void (*PFN_AImage_delete)(AImage *image);

static struct {
	void *lib_mediandk;
	PFN_AImage_delete                  imageDelete;
	int loaded;
} dvhw_api;

static int dvhw_api_load(void)
{
	if (dvhw_api.loaded)
		return 0;

	dvhw_api.lib_mediandk = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);
	if (!dvhw_api.lib_mediandk) {
		serprintf(TAG ": cannot dlopen libmediandk.so\n");
		return 1;
	}

	dvhw_api.imageDelete = (PFN_AImage_delete)
		dlsym(dvhw_api.lib_mediandk, "AImage_delete");

	if (!dvhw_api.imageDelete) {
		serprintf(TAG ": AImage entry points incomplete\n");
		dlclose(dvhw_api.lib_mediandk);
		dvhw_api.lib_mediandk = NULL;
		return 1;
	}
	dvhw_api.loaded = 1;
	return 0;
}

#define DVHW_IN_Q_MAX 10
/* BL park high-water: AU feed stops when this many decoded BLs are parked
 * waiting for their EL partners (mpv f_enhancement_pair QUEUE_MAX analogue,
 * sized to the EL reorder lag DVHW_EL_CATCHUP + 2). Doubles as the
 * queue-pressure emit threshold in dvhw_fel_emit - it must stay at/below
 * the feed gate so a lost-EL head can always drain. Below the bl_q[]
 * array cap so an intra-iteration output burst still has defense room. */
#define DVHW_BL_PARK_MAX 6
/* BL decode credit: park + codec in-flight (fed - out) must stay below
 * this for the feed to continue. The park-only gate measured a steady
 * 3-10/s park_drop: a burst-feed can push ~10 AUs into the codec input
 * queue instantly, and the codec then EMITS that burst into an already-
 * near-full park. Counting the in-flight window in the budget absorbs
 * bursts by pausing feed credits instead (the classic bounded-in-flight
 * model; mpv: the VO pool bounds what the decoder may hold). Must exceed
 * the codec's reorder depth (B-pyramid ~4-5 AUs) or the codec banks and
 * feed stalls - 10 covers it with margin; the 12-slot bl_q array keeps
 * defense room above it. */
#define DVHW_BL_CREDIT_MAX 32
/* ^ was 10: the 4K c2 codec holds ~24 AUs in flight inside its B-pyramid
 * reorder pipeline at ALL times (measured el_fed-el_out=24 steady), so
 * park+in_flight < 10 was permanently violated and the feed dribbled
 * (44/s with 19 inline-copy fallbacks/s). 32 = 24 reorder + 8 slack. */
#define DVHW_BL_Q_ARRAY 12	/* the bl_q[] array bound - park-overflow
				 * defense drops at THIS, not CREDIT_MAX: with
				 * CREDIT_MAX=32 a >=CREDIT_MAX test is dead code and
				 * the collector could write past bl_q[11] (UB) */

typedef struct {
	uint8_t *data;
	int      size;
	uint8_t *rpu;
	int      rpu_size;
	int      user_id;
	int64_t  pts_us;		/* AU pts in µs - MediaCodec timestamp key */
} dvhw_au_t;

/* --- decoder framedrop (mpv --framedrop=decoder parity) ------------------
 * Measured: the 4K BL codec sustains ~44/s vs 48fps content - a fixed
 * ~4/s deficit. Video can never run faster than 1x, so the deficit grows
 * unboundedly (unsync climbing, sink late-drops 13-17/s). mpv's answer:
 * when video is behind, SKIP non-reference frames at the decoder INPUT
 * (HEVC _N slice types - disposable, nothing references them), so the
 * content position advances faster than realtime until it catches up.
 * Dropped BL pts go into a skip ring; dvhw_el_feed discards the matching
 * EL packets (exact-pts pairing must stay 1:1). */
#define DVHW_EL_SKIP_RING 16
typedef struct {
	int64_t pts_us[DVHW_EL_SKIP_RING];
	int head, count;
} dvhw_skip_ring_t;

// RPU blobs waiting for their decoded frame, keyed by MediaCodec timestamp
// user_ID rides along (async layer): the frame slot the engine gets back
// must carry the AU's user_ID (subtitle sync bookkeeping)
typedef struct {
	int	used;
	int64_t	pts_us;
	uint8_t	*rpu;
	int	rpu_size;
	int	user_id;
} dvhw_pending;

/* --- recycling frame-buffer pool -------------------------------------------
 * Measured: av_frame_get_buffer per output frame cost ~14ms each (12MB 4K
 * P010: mmap + ~3200 first-touch page faults), 500-580ms/s of the decode
 * thread - the direct wall between emit 40/s and content 48/s. The pool
 * recycles previously-touched pages instead: buffers ride the AVFrame's
 * refcount (av_buffer_create with a free callback that RETURNS the memory
 * to the stash), so lifetime is always correct - frames the sink still
 * holds keep their memory, and dead frames' memory comes back for reuse
 * automatically. FFmpeg's own decoders do exactly this (avcodec's internal
 * frame pools).
 *
 * Layout: [hdr (32B) | frame data ...] - hdr sits at a FIXED offset below
 * the AVBuffer data pointer, so the free callback recovers the exact birth
 * size and the OWNING pool. If the owning pool died (geometry change or
 * dvhw_close), the memory frees directly; a stale buffer can never be
 * re-stashed into the wrong geometry. */
#define DVHW_BUFPOOL_MAX 16
#define DVHW_BUFPOOL_HDR 32
typedef struct {
	uint8_t *ptr[DVHW_BUFPOOL_MAX];	/* allocation base (hdr) pointers */
	size_t   size[DVHW_BUFPOOL_MAX];	/* full allocation size incl. hdr */
	int      count;
	int      dead;
	pthread_mutex_t mtx;	/* take (copy worker) races release (frame death on the
				 * sink/decode/flush threads) - lock the stash */
} dvhw_bufpool_t;

/* BL buffer-mode copy job (async copy worker): the decode thread
 * dequeues output buffers and posts them; a WORKER thread does the
 * expensive part (AMediaCodec_getOutputBuffer + 12MB memcpy + release,
 * measured ~21ms serialized per frame - the whole difference between
 * emit 40/s and content 48/s on 4K P010: the binder wait inside
 * getOutputBuffer/release overlaps the NEXT dequeue instead of
 * serializing with it). mpv/FFmpeg parity: their mediacodec-copy paths
 * run the copy off the dequeue critical path too. */
typedef struct {
	ssize_t oidx;
	AMediaCodecBufferInfo info;
	int width, height, stride, slice_height, color_format;
	uint8_t *rpu;			/* BL's RPU (pending_take at post time) - the
				 * worker attaches the DV side data */
	int rpu_size;
	int user_id;			/* AU user_ID for the park (subtitle sync) */
	int64_t pts_us;			/* EMISSION-ORDER GATE: this job's pts - the
				 * emit loop holds bl_q[0] while any in-flight
				 * copy carries an older pts (see
				 * dvhw_copyq_oldest_inflight_ms) */
} dvhw_copyjob_t;
#define DVHW_COPY_Q_MAX 8
/* TWO copy workers: one job = getOutputBuffer + 25MB P010 memcpy + release
 * + RPU parse ~ 33ms measured - a single worker saturates at ~30/s, below
 * the 48fps Charles ceiling, and the ring-saturation backpressure (below)
 * then pins the whole pipeline to the copy rate. Two overlap the binder
 * waits: ~66/s ceiling, headroom over content rate. */
#define DVHW_COPY_WORKERS 2
typedef struct {
	dvhw_copyjob_t job[DVHW_COPY_Q_MAX];
	int head, count;	/* decode thread posts at (head+count)%MAX */
	AVFrame *ready[DVHW_COPY_Q_MAX];
	int ready_user_id[DVHW_COPY_Q_MAX];
	int ready_head, ready_count;
	int run;		/* workers alive */
	int busy[DVHW_COPY_WORKERS];	/* per-worker in-flight state (flush waits) */
	int64_t busy_pts_us[DVHW_COPY_WORKERS];
				/* per-worker CURRENT job pts (INT64_MAX = idle);
				 * the emission-order gate needs the pts of a
				 * frame still inside a worker, not just the ones
				 * posted to the ring */
	int release_only;	/* flush protocol: drain queued jobs, release-only */
	pthread_mutex_t mtx;
	pthread_cond_t cond;
	pthread_t thread[DVHW_COPY_WORKERS];
} dvhw_copyq_t;

static void *dvhw_copy_worker_n(void *ctx);

/* EMISSION-ORDER GATE support: the oldest pts (us domain, but the same
 * media-ms * 1000) still inside the copy pipeline - posted jobs, jobs
 * held by a busy worker, frames sitting on the ready ring - or INT64_MAX
 * when nothing is in flight. The emit loop compares this against
 * bl_q[0]->pts*1000: a smaller value means an OLDER frame is still
 * copying, so emitting the head now would put its newer content on
 * screen first and the old frame after - the backward jump. Takes the
 * copyq directly (PRIV is not yet typedef'd at this point in the file);
 * callers hold NO other locks while taking q->mtx. */
static int64_t dvhw_copyq_oldest_inflight_us(dvhw_copyq_t *q)
{
	int64_t oldest = INT64_MAX;
	pthread_mutex_lock(&q->mtx);
	for (int i = 0; i < q->count; i++) {
		int64_t us = q->job[(q->head + i) % DVHW_COPY_Q_MAX].pts_us;
		if (us < oldest)
			oldest = us;
	}
	for (int i = 0; i < DVHW_COPY_WORKERS; i++) {
		if (q->busy_pts_us[i] < oldest)
			oldest = q->busy_pts_us[i];
	}
	for (int i = 0; i < q->ready_count; i++) {
		AVFrame *f = q->ready[(q->ready_head + i) % DVHW_COPY_Q_MAX];
		if (f && f->pts * 1000 < oldest)
			oldest = f->pts * 1000;
	}
	pthread_mutex_unlock(&q->mtx);
	return oldest;
}
static void dvhw_bufpool_init(dvhw_bufpool_t *bp)
{
	bp->count = 0;
	bp->dead = 0;
	pthread_mutex_init(&bp->mtx, NULL);
}

static void dvhw_bufpool_kill(dvhw_bufpool_t *bp)
{
	int i;
	pthread_mutex_lock(&bp->mtx);
	bp->dead = 1;	/* late unrefs of in-flight buffers free directly */
	for (i = 0; i < bp->count; i++)
		av_freep(&bp->ptr[i]);
	bp->count = 0;
	pthread_mutex_unlock(&bp->mtx);
	pthread_mutex_destroy(&bp->mtx);
}

typedef struct {
	dvhw_bufpool_t *owner;
	size_t          size;	/* total allocation (hdr + payload) */
} dvhw_bufpool_hdr;

/* take a stashed allocation of >= payload size, or malloc fresh */
static uint8_t *dvhw_bufpool_take(dvhw_bufpool_t *bp, size_t payload)
{
	int i, best = -1;
	size_t need = payload + DVHW_BUFPOOL_HDR;
	uint8_t *mem;
	pthread_mutex_lock(&bp->mtx);
	for (i = 0; i < bp->count; i++) {
		if (bp->size[i] >= need && (best < 0 || bp->size[i] < bp->size[best]))
			best = i;
	}
	if (best >= 0) {
		mem = bp->ptr[best];
		bp->size[best] = bp->size[bp->count - 1];
		bp->ptr[best] = bp->ptr[bp->count - 1];
		bp->count--;
		pthread_mutex_unlock(&bp->mtx);
		return mem;
	}
	pthread_mutex_unlock(&bp->mtx);
	return (uint8_t *) av_malloc(need);
}

/* AVBuffer free-callback: last AVFrame ref died - return to the stash */
static void dvhw_bufpool_release(void *opaque, uint8_t *data)
{
	dvhw_bufpool_hdr *hdr = (dvhw_bufpool_hdr *) (data - DVHW_BUFPOOL_HDR);
	dvhw_bufpool_t *bp = hdr->owner;
	pthread_mutex_lock(&bp->mtx);
	if (bp->dead) {
		pthread_mutex_unlock(&bp->mtx);
		av_free(hdr);	/* hdr IS the allocation base */
		return;
	}
	if (bp->count < DVHW_BUFPOOL_MAX) {
		bp->ptr[bp->count] = (uint8_t *) hdr;
		bp->size[bp->count] = hdr->size;
		bp->count++;
		pthread_mutex_unlock(&bp->mtx);
		return;
	}
	pthread_mutex_unlock(&bp->mtx);
	av_free(hdr);	/* stash full: normal free */
}

typedef struct {
	AMediaCodec	*codec;
	int	width, height;
	int	nal_length_size;
	dvhw_pending	pending[DVHW_PENDING_MAX];
	int	pending_count;
	/* --- FEL: hardware BL+EL byte-buffer decode (mpv mediacodec-copy shape).
	 * When the file has an enhancement layer, BOTH layers decode through
	 * MediaCodec in buffer mode (no surface): libplacebo's FEL composition
	 * requires the BL in YUV domain with the DV repr (renderer.c gates on
	 * PL_COLOR_SYSTEM_DOLBYVISION + nlq_active), which an OES/RGB import
	 * cannot provide. One YUV420 copy per frame per layer — the same cost
	 * mpv's mediacodec-copy path pays — versus the multi-hundred-ms SW
	 * 4K HEVC decode. */
	int	fel;			/* 1: BL+EL buffer-mode decode active */
	int	buffer_mode;		/* 1: BL byte-buffer output (copy pipeline). Set for
				 * FEL AND for P8.x/P7-BL-only: the YUV-copy path is
				 * the glitch-free one (see dvhw_open). */
	AMediaCodec	*el_codec;
	int	el_width, el_height;
	int	el_stride, el_slice_height;
	int	el_color_format;
	int	el_fed_count;		/* EL AUs fed, throttled against BL AUs consumed */
	int	el_drain_count;		/* EL frames emitted by the decoder (reorder lag tracking) */
	int	bl_fed_count;		/* BL AUs queued to the BL codec (EL feed pacing master) */
	int	el_seen;		/* EL emitted >=1 frame since last OPEN (warm-up gate,
				 * mpv 3b4caf0; carries across seek/flush - a flushed
				 * codec keeps its pipeline primed, only its reorder
				 * window must refill) */
	int	el_exhausted;		/* parser has no more EL packets (EOF tail evidence,
				 * probed in dvhw_el_feed; cleared on flush) */
	AVFrame		*el_q[DVHW_PENDING_MAX];
	int	el_q_count;
	int	el_nal_length_size;
	int	el_params_sent;	/* 1: in-band VPS/SPS/PPS prepended once */
	AVPacket	el_hold_pkt;	/* EL AU held over an input-full dequeue */
	int	el_hold_valid;	/* 1: el_hold_pkt owns a parser packet */
	/* BL buffer-mode decode (FEL only) */
	int	stride, slice_height, color_format;
	/* BL parking depth. mpv f_enhancement_pair holds QUEUE_MAX=8 pending
	 * BLs through the EL warm-up (el_seen==0, no pressure drops); 4 would
	 * overflow the array the moment the warm-up hold (dvhw_fel_emit /
	 * park-side drop both gate on el_seen) actually engages. 12 slots
	 * = DVHW_BL_CREDIT_MAX + defense margin (credit-gated feed can reach
	 * the cap only with an empty codec pipeline; the >= CREDIT_MAX drop
	 * below is the last-resort defense, not a pacing path). */
	AVFrame		*bl_q[12];
	int	bl_q_count;
	int		bl_user_id[12];	/* per parked BL: AU user_ID (subtitle sync) */
	int		bl_out_count;	/* cumulative BL codec outputs (in-flight = fed - out) */
	int		emit_user_id;	/* bl_user_id[0] at emit time (async fill) */
	int		collect_user_id;	/* ready-frame user_id at collect time (park fill) */
	/* 1Hz pipeline diagnostics (reset each stats print) */
	int	stat_bl_out, stat_emit;
	int	stat_bl_zerolen;	/* zero-size codec events released (BL) */
	int	stat_bl_deq;		/* AMediaCodec_dequeueOutputBuffer returns >0 (BL) */
	int	stat_bl_postdrop;	/* copy ring saturated at post: inline-released frame */
	int	stat_bl_readydrop;	/* ready ring full in worker: dropped copy */
	int	stat_dec_drop;		/* decoder framedrop: _N AUs skipped at dec_in (mpv parity) */
	int	stat_el_zerolen;	/* zero-size codec events released (EL) */
	int	stat_park_drop;	/* parked BLs dropped on bl_q overflow */
	int	stat_bl_qfull;	/* AUs dropped because the BL codec input queue was full */
	int	stat_pts_last, stat_pts_dmin, stat_pts_dmax;	/* emitted pts spacing */
	int64_t stat_log_last_us;	/* 1Hz fps-pipe print timer (PER-INSTANCE: the
				 * function-static it replaces was shared across
				 * live codec instances, interleaving their prints) */
	int	stat_log_loops;	/* loops accumulated since the last 1Hz print */
	int64_t stat_fdguard_last_us;	/* 1Hz fdguard print timer (same) */
	int64_t	dq_ring[8];	/* DEQUEUE-ORDER diagnostic ring: last 8 */
	int	dq_ring_w;	/*   dequeued pts, dumped on negative emit */
	int64_t	stat_fed_last_us;	/* last AU pts FED - detects double-feed/duplicate-pts */
	int64_t	stat_fed_dmin_us, stat_fed_dmax_us;	/* fed pts step range */
	int64_t	stat_phase_ns[4];	/* per-1s wall: 0 feed / 1 el / 2 output-poll+copy / 3 emit */
	int64_t	stat_copy_ns;	/* per-1s: dvhw_copy_yuv420 wall */
	int64_t	stat_rpu_ns;	/* per-1s: dovi_rpu_parse_to_avmetadata wall */
	int64_t	stat_getin_ns;	/* per-1s: AMediaCodec_getInputBuffer wall */
	int64_t	stat_qin_ns;	/* per-1s: memcpy+queueInputBuffer wall */

/* --- async layer (sfdec2 contract) --- */
int		run;		/* thread alive */
int		flushing;	/* flush in progress: thread parks, queues rebuild */
int		th_busy;		/* thread holds in-flight state (flush waits) */
int		feed_run;		/* feed worker alive */
int		feed_busy;	/* feed worker holds an in-flight AU (flush waits) */
int		feed_park;	/* flush: feed worker parks after current AU */
pthread_mutex_t	feed_mtx;
pthread_cond_t	feed_cond;
pthread_t	feed_thread;
int		feed_started;
pthread_mutex_t	pending_mtx;	/* pending[] map: push (feed worker) races take (decode thread) */
dvhw_bufpool_t	bufpool;	/* recycled 4K frame-buffer stash (see dvhw_bufpool_*) */
dvhw_copyq_t	copyq;	/* BL copy worker ring (see dvhw_copyjob_t) */
	pthread_mutex_t	th_mtx;
	pthread_cond_t	th_cond;
	struct {
		pthread_mutex_t	mtx;
		pthread_cond_t	cond;
	} th;
	pthread_t	th_thread;
	int		th_started;
	int		th_mtx_inited;	/* th.mtx/th.cond init'd (created before the thread, may outlive it briefly) */
	int		copyq_started;	/* BL copy worker thread alive */
	dvhw_au_t	in_q[DVHW_IN_Q_MAX];
	int		in_q_read, in_q_write, in_q_count;
	dvhw_skip_ring_t	el_skip;	/* dropped-BL pts: EL packets to discard (decoder framedrop) */
	FRAME_Q		slot_q;		/* pool frames handed via put_out, ready to fill */
	FRAME_Q		out_q;		/* filled frames, engine collects via get_out */
	int		slot_count;
} PRIV;

/* async layer (defined later, used by open/close/flush) */
static void *dvhw_async_thread(void *ctx);
static void *dvhw_feed_worker(void *ctx);
static void dvhw_in_q_clear(PRIV *p);
static void dvhw_out_q_clear(PRIV *p);
static int dvhw_dec_in_should_drop(PRIV *p, STREAM_DEC_VIDEO *dec,
                                   VIDEO_FRAME *d,
                                   const uint8_t *clean, int clean_size);

static void dvhw_pending_clear(PRIV *p)
{
	int i;
	pthread_mutex_lock(&p->pending_mtx);
	for (i = 0; i < DVHW_PENDING_MAX; i++) {
		if (p->pending[i].used) {
			av_free(p->pending[i].rpu);
			p->pending[i].used = 0;
			p->pending[i].rpu = NULL;
			p->pending[i].rpu_size = 0;
		}
	}
	p->pending_count = 0;
	pthread_mutex_unlock(&p->pending_mtx);
}

static void dvhw_pending_push(PRIV *p, int64_t pts_us, uint8_t *rpu, int rpu_size,
                            int user_id)
{
	int i, oldest = -1;
	pthread_mutex_lock(&p->pending_mtx);
	for (i = 0; i < DVHW_PENDING_MAX; i++) {
		if (!p->pending[i].used)
			break;
		if (oldest < 0 || p->pending[i].pts_us < p->pending[oldest].pts_us)
			oldest = i;
	}
	if (i == DVHW_PENDING_MAX) {
		// full: drop the oldest (stale) entry
		i = oldest;
		av_free(p->pending[i].rpu);
			p->pending[i].used = 0;
		p->pending_count--;
	}
	p->pending[i].used = 1;
	p->pending[i].pts_us = pts_us;
	p->pending[i].rpu = rpu;
	p->pending[i].rpu_size = rpu_size;
	p->pending[i].user_id = user_id;
	p->pending_count++;
	pthread_mutex_unlock(&p->pending_mtx);
}

// take the RPU matching pts_us (exact match first, else oldest <= pts_us);
// user_id_out gets the AU's subtitle-sync bookkeeping id
static void dvhw_pending_take(PRIV *p, int64_t pts_us, uint8_t **rpu, int *rpu_size,
                               int *user_id_out)
{
	int i, best = -1;
	*rpu = NULL;
	*rpu_size = 0;
	if (user_id_out)
		*user_id_out = 0;
	pthread_mutex_lock(&p->pending_mtx);
	for (i = 0; i < DVHW_PENDING_MAX; i++) {
		if (!p->pending[i].used)
			continue;
		if (p->pending[i].pts_us == pts_us) {
			best = i;
			break;
		}
		if (p->pending[i].pts_us <= pts_us &&
		    (best < 0 || p->pending[i].pts_us > p->pending[best].pts_us))
			best = i;
	}
	if (best < 0) {
		pthread_mutex_unlock(&p->pending_mtx);
		return;
	}
	*rpu = p->pending[best].rpu;
	*rpu_size = p->pending[best].rpu_size;
	if (user_id_out)
		*user_id_out = p->pending[best].user_id;
	p->pending[best].used = 0;
	p->pending[best].rpu = NULL;
	p->pending[best].rpu_size = 0;
	p->pending[best].user_id = 0;
	p->pending_count--;
	pthread_mutex_unlock(&p->pending_mtx);
}

// renderer-side cleanup, invoked by the DV sink after rendering
static void dvhw_frame_release(dovi_hw_frame *f)
{
	if (!f)
		return;
	/* the AHardwareBuffer is owned by the AImage (mpv hwdec_aimagereader
	 * does the same): AImage_delete releases it, an explicit ahbRelease
	 * would underflow the refcount and corrupt the reader's BufferQueue */
	if (f->image && dvhw_api.imageDelete)
		dvhw_api.imageDelete((AImage *) f->image);
	av_free(f->rpu);
	afree(f);
}

static void dvhw_frame_release(dovi_hw_frame *f);

static void dvhw_el_q_clear(PRIV *p);
static void dvhw_bl_q_clear(PRIV *p);

static int dvhw_open(STREAM_DEC_VIDEO *dec, VIDEO_PROPERTIES *video, void *ctx,
                     int *pneed_flush, int *pneed_reorder)
{
	PRIV *p = (PRIV *) dec->priv;
	AMediaFormat *fmt = NULL;
	media_status_t st;
	int i;

	if (libavos_get_dolby_vision_mode() == 0)
		return 1;		// passthrough mode: sfdec2 owns DV
	if (video->format != VIDEO_FORMAT_DOLBY_VISION)
		return 1;
	if (android_get_device_api_level() < 26) {
		serprintf(TAG ": AHardwareBuffer export needs API 26, using software path\n");
		return 1;
	}
	if (video->dv_profile_source != 7 && video->dv_profile_source != 8)
		return 1;
	if (dvhw_api_load())
		return 1;

	// FEL (profile 7 with EL): libplacebo composes the enhancement layer
	// only from a YUV-domain BL with the DV repr (PL_COLOR_SYSTEM_DOLBYVISION
	// + nlq_active, renderer.c sample_el gate). The OES import is RGB, so for
	// FEL we decode BOTH layers through MediaCodec byte-buffer mode (mpv
	// mediacodec-copy shape) and hand YUV AVFrames to the same
	// dovi_gl_render(bl, el) call the software path uses. Without an EL
	// config (P8.x, P7 BL-only) the zero-copy OES surface path stays.
	memset(p, 0, sizeof(*p));
	p->fel = (video->dv_profile_source == 7 &&
	          video->dv_el_extraData && video->dv_el_extraDataSize > 0) ? 1 : 0;
	/* P8.x (and P7 BL-only) use the same BUFFER-MODE pipeline as FEL, minus
	 * the EL codec: MediaCodec hands YUV420 byte buffers, dvhw_copy_yuv420
	 * copies them to AVFrames and the RPU side datas (DOVI_METADATA + raw
	 * RPU) are attached for pl_map_avframe_ex(map_dovi) in dovi_gl_render -
	 * identical to the proven FEL BL path. The former zero-copy OES
	 * surface path (AImageReader + EGLImage import) showed deterministic
	 * one-frame corruption (colored squares) on SMB Cape Fear P8.1 at
	 * fixed content positions - playing fine locally and fine on desktop
	 * mpv - pointing at the Samsung codec2 GPU-render-to-reader buffer
	 * pipeline being fed in a way this device tolerates only when the
	 * queue stays shallow. The copy pipeline costs one 4K YUV420 copy per
	 * frame (~2ms CPU) and reuses the same bl_q/emit machinery FEL uses. */
	p->buffer_mode = 1;
	p->width = video->width;
	p->height = video->height;
	p->nal_length_size = dovi_hvcc_nal_length_size(video->extraData,
	                                               video->extraDataSize);

	if (p->fel) {
		serprintf(TAG ": profile 7 FEL: hardware BL+EL buffer-mode decode\n");
		// BL codec in BUFFER mode: no surface, YUV420 byte buffers out.
		// Same configure as below otherwise.
		p->codec = AMediaCodec_createDecoderByType("video/hevc");
		if (!p->codec) {
			serprintf(TAG ": no video/hevc decoder available\n");
			goto fail;
		}
		fmt = AMediaFormat_new();
		if (!fmt)
			goto fail;
		AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/hevc");
		AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, video->width);
		AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, video->height);
		/* Request 10-bit output (P010) for Main10 BL: c2.qti.hevc.decoder
		 * advertises YUVP010 (0x36) in byte-buffer mode. Without this the
		 * codec may hand back 8-bit NV12, and an 8-bit BL fed through the
		 * 10-bit-authored DV reshaping polynomial renders wrong (green).
		 * Verified against dumpsys media.player color list. */
		AMediaFormat_setInt32(fmt, "color-format", 0x36 /* YUVP010 */);
		if (video->extraDataSize > 0)
			AMediaFormat_setBuffer(fmt, "csd-0",
			                       video->extraData, video->extraDataSize);
		if (AMediaCodec_configure(p->codec, fmt, NULL, NULL, 0) != 0) {
			serprintf(TAG ": AMediaCodec_configure failed (BL buffer mode)\n");
			goto fail;
		}
		if (AMediaCodec_start(p->codec) != 0) {
			serprintf(TAG ": AMediaCodec_start failed (BL)\n");
			goto fail;
		}

		// EL codec: separate 1080p session in buffer mode, EL hvcC config.
		// The EL hvcC comes from dv_el_extraData: the hvcE BlockAddition
		// mapping config (interleaved) or the EL track CodecPrivate
		// (dual-track), same as the software path uses.
		p->el_nal_length_size = dovi_hvcc_nal_length_size(
			video->dv_el_extraData, video->dv_el_extraDataSize);
		p->el_codec = AMediaCodec_createDecoderByType("video/hevc");
		if (!p->el_codec) {
			serprintf(TAG ": no second video/hevc decoder for EL\n");
			goto fail;
		}
		AMediaFormat_delete(fmt);
		fmt = AMediaFormat_new();
		if (!fmt)
			goto fail;
		AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/hevc");
		/* the EL dimensions (1920x1080 for a spatial-substream FEL) come
		 * from the codec's output-format event; seed with the BL size as a
		 * hint (MediaCodec re-negotiates from the hvcC/SPS anyway) */
		p->el_width  = video->width;
		p->el_height = video->height;
		AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, p->el_width);
		AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, p->el_height);
		/* Main10 EL: request P010 (same as the BL) */
		AMediaFormat_setInt32(fmt, "color-format", 0x36 /* YUVP010 */);
		AMediaFormat_setBuffer(fmt, "csd-0",
		                       video->dv_el_extraData, video->dv_el_extraDataSize);
		if (AMediaCodec_configure(p->el_codec, fmt, NULL, NULL, 0) != 0) {
			serprintf(TAG ": AMediaCodec_configure failed (EL)\n");
			goto fail;
		}
		if (AMediaCodec_start(p->el_codec) != 0) {
			serprintf(TAG ": AMediaCodec_start failed (EL)\n");
			goto fail;
		}
		AMediaFormat_delete(fmt);
		fmt = NULL;
	} else {
	// --- P8.x / P7 BL-only: same BL BUFFER-mode configure as FEL, no EL ---
	serprintf(TAG ": profile %d: hardware BL buffer-mode decode (copy pipeline)\n",
		          video->dv_profile_source);
	p->codec = AMediaCodec_createDecoderByType("video/hevc");
	if (!p->codec) {
		serprintf(TAG ": no video/hevc decoder available\n");
		goto fail;
	}
	fmt = AMediaFormat_new();
	if (!fmt)
		goto fail;
	AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/hevc");
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, video->width);
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, video->height);
	/* Request 10-bit output (P010) for Main10: c2.qti.hevc.decoder
	 * advertises YUVP010 (0x36) in byte-buffer mode. Without this the
	 * codec may hand back 8-bit NV12, and an 8-bit BL fed through the
	 * 10-bit-authored DV reshaping polynomial renders wrong (green).
	 * Same key the FEL BL configure uses. */
	AMediaFormat_setInt32(fmt, "color-format", 0x36 /* YUVP010 */);
	if (video->extraDataSize > 0)
		AMediaFormat_setBuffer(fmt, "csd-0",
		                       video->extraData, video->extraDataSize);
	if (AMediaCodec_configure(p->codec, fmt, NULL, NULL, 0) != 0) {
		serprintf(TAG ": AMediaCodec_configure failed (BL buffer mode)\n");
		goto fail;
	}
	AMediaFormat_delete(fmt);
	fmt = NULL;
	if (AMediaCodec_start(p->codec) != 0) {
		serprintf(TAG ": AMediaCodec_start failed (BL)\n");
		goto fail;
	}
	}	// end BL buffer-mode (non-FEL) path

	dec->ctx = ctx;
	dec->video = &dec->_video;
	memcpy(dec->video, video, sizeof(VIDEO_PROPERTIES));
	dec->is_open = 1;
	if (pneed_flush)
		*pneed_flush = 1;
	if (pneed_reorder)
		*pneed_reorder = 0;	// MediaCodec outputs in display order

	/* async layer: queues + decode thread (sfdec2 contract). The engine
	 * switches to _stream_player_async via dec->async and drives us
	 * through dec_in/put_out/get_out - the 12ms decode wall overlaps
	 * the engine loop instead of serializing it. */
	frame_q_init(&p->slot_q, "dovi_slot_q");
	frame_q_init(&p->out_q, "dovi_out_q");
	p->in_q_read = p->in_q_write = p->in_q_count = 0;
	p->el_skip.head = p->el_skip.count = 0;
	p->stat_dec_drop = 0;
	p->slot_count = 0;
	p->run = 1;
	p->flushing = 0;
	p->th_busy = 0;
	dvhw_bufpool_init(&p->bufpool);
	/* BL copy workers: mutex+cond first, ring state zeroed, then start */
	p->copyq.head = p->copyq.count = 0;
	p->copyq.ready_head = p->copyq.ready_count = 0;
	for (i = 0; i < DVHW_COPY_WORKERS; i++) {
		p->copyq.busy[i] = 0;
		p->copyq.busy_pts_us[i] = INT64_MAX;	/* 0 is a VALID pts - */
	}							/* an idle worker must not gate */
	p->copyq.release_only = 0;
	p->copyq.run = 1;
	/* errorcheck mutexes: they diagnose lock-order/double-unlock bugs with
	 * the offending thread+line in logcat instead of corrupting state.
	 * Three-way threaded pipeline (decode/feed/copy) is new code; the
	 * first cut deadlocked (measured startup wedge, all threads in
	 * futex_wait with no owner). Overhead is negligible vs the binder
	 * calls they guard. */
	pthread_mutexattr_t mattr;
	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_ERRORCHECK);
	if (pthread_mutex_init(&p->copyq.mtx, &mattr) ||
	    pthread_cond_init(&p->copyq.cond, NULL)) {
		p->copyq.run = 0;
		goto fail;
	}
	if (pthread_mutex_init(&p->th.mtx, &mattr) ||
	    pthread_cond_init(&p->th.cond, NULL)) {
		p->run = 0;
		goto fail;
	}
	p->th_mtx_inited = 1;
	for (i = 0; i < DVHW_COPY_WORKERS; i++) {
		struct worker_ctx {
			void *dec;
			int slot;
		};
		struct worker_ctx *wc = amalloc(sizeof(*wc));
		if (!wc)
			break;
		wc->dec = dec;
		wc->slot = i;
		if (pthread_create(&p->copyq.thread[i], NULL,
			           dvhw_copy_worker_n, wc)) {
			afree(wc);
			p->copyq.run = 0;
			pthread_mutex_destroy(&p->copyq.mtx);
			pthread_cond_destroy(&p->copyq.cond);
			goto fail;
		}
		p->copyq_started = i + 1;	/* number of workers running */
	}
	/* feed worker: the dedicated queueInputBuffer thread (see
	 * dvhw_feed_worker). pending_mtx guards the RPU map across
	 * push (feed worker) / take (decode thread). */
	p->feed_run = 1;
	p->feed_busy = 0;
	p->feed_park = 0;
	if (pthread_mutex_init(&p->feed_mtx, &mattr) ||
	    pthread_cond_init(&p->feed_cond, NULL) ||
	    pthread_mutex_init(&p->pending_mtx, &mattr)) {
		p->feed_run = 0;
		pthread_mutexattr_destroy(&mattr);
		goto fail;
	}
	pthread_mutexattr_destroy(&mattr);
	if (pthread_create(&p->feed_thread, NULL, dvhw_feed_worker, dec)) {
		p->feed_run = 0;
		pthread_mutex_destroy(&p->feed_mtx);
		pthread_cond_destroy(&p->feed_cond);
		pthread_mutex_destroy(&p->pending_mtx);
		goto fail;
	}
	p->feed_started = 1;
	if (pthread_create(&p->th_thread, NULL, dvhw_async_thread, dec)) {
		p->run = 0;
		pthread_mutex_lock(&p->th.mtx);
		pthread_cond_broadcast(&p->th.cond);
		pthread_mutex_unlock(&p->th.mtx);
		goto fail;
	}
	p->th_started = 1;

	serprintf(TAG ": MediaCodec HEVC decode for DV tone-map, %dx%d nal_length_size %d (async)\n",
	          video->width, video->height, p->nal_length_size);
	return 0;

fail:
	if (fmt)
		AMediaFormat_delete(fmt);
	if (p->codec) {
		AMediaCodec_delete(p->codec);
		p->codec = NULL;
	}
	if (p->el_codec) {
		AMediaCodec_delete(p->el_codec);
		p->el_codec = NULL;
	}
	dvhw_el_q_clear(p);
	dvhw_bl_q_clear(p);
	return 1;
}

// ************************************************************
//
//	FEL byte-buffer helpers: frame queues, YUV420 copy from MediaCodec
//
// ************************************************************
static void dvhw_el_q_clear(PRIV *p)
{
	while (p->el_q_count > 0)
		av_frame_free(&p->el_q[--p->el_q_count]);
	/* held-over EL AU (input-full stash): it belongs to the pre-flush
	 * stream, unref or it leaks/segfaults on stale data after seek */
	if (p->el_hold_valid) {
		av_packet_unref(&p->el_hold_pkt);
		p->el_hold_valid = 0;
	}
	/* seek/flush: EL decoder state restarts, drop the pace accounting
	 * and the warm-up flag (mpv pair_reset) - EXCEPT el_seen: a FLUSH
	 * does not un-prime the codec's decode pipeline (it keeps its
	 * hardware state; only its reorder window must refill, ~CATCHUP
	 * frames), so the codec is still 'warm' for pressure-emit purposes.
	 * Clearing el_seen on every seek let a post-seek EL hiccup park BLs
	 * up to the 12-slot array bound with the pressure-emit disarmed
	 * (park_drop 4 measured on the 12:07 seek-stall) and re-fed 96 AUs
	 * of the warm-up floor (~2s at 44 AU/s) that a warm codec never
	 * needed. Close/re-open still resets it - a NEW codec instance
	 * must re-prove warmth. */
	p->el_fed_count = 0;
	p->el_drain_count = 0;
	p->bl_fed_count = 0;
	p->bl_out_count = 0;	/* or in-flight (fed-out) goes negative and
					 * the credit gate opens unbounded after seek */
}

static void dvhw_bl_q_clear(PRIV *p)
{
	while (p->bl_q_count > 0)
		av_frame_free(&p->bl_q[--p->bl_q_count]);
}
/* copy one MediaCodec YUV420 byte buffer (index already dequeued) into an
 * AVFrame — FFmpeg mediacodec_sw_buffer_copy_yuv420_* parity: stride and
 * slice-height aware, planar and semiplanar layouts. Releases the codec
 * buffer after the copy. pts lands in the avos ms domain. */
static AVFrame *dvhw_copy_yuv420(PRIV *p, AMediaCodec *codec, ssize_t index,
                                 AMediaCodecBufferInfo *info,
                                 int width, int height,
                                 int stride, int slice_height,
                                 int color_format, int crop_right)
{
	AVFrame *f = NULL;
	size_t bsize = 0, total;
	uint8_t *src = AMediaCodec_getOutputBuffer(codec, (size_t) index, &bsize);
	int y, x;

	if (!src)
		goto done;	/* release the dequeued output slot (an early return here
			 * would permanently lose one of the codec's fixed output
			 * buffers and wedge output after N failures) */
	if (stride <= 0)
		stride = width;
	if (slice_height <= 0)
		slice_height = height;
	/* crop_right: MediaCodec slices may be padded to a stride multiple */
	(void) crop_right;

	f = av_frame_alloc();
	if (!f)
		goto done;
	/* P010 (color 0x36): 16-bit samples, 10-bit code MSB-aligned. The
	 * frame tag is set to AV_PIX_FMT_P010LE below (semiplanar, shift 6)
	 * so libplacebo reads code/1023 - the domain the DV RPU curve and
	 * matrices were authored for (see the p010 copy branch). */
	int p010 = (color_format == DVHW_COLOR_FormatYUVP010);
	f->format = p010 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_YUV420P;
	f->width = width;
	f->height = height;
	/* Layout math (MediaCodec stride/slice-height are in BYTES):
	 *  - P010: 2 planes (Y, interleaved UV), both row pitch = stride
	 *    (16-bit samples: stride >= width*2; the copy loops use y*stride
	 *    source rows and width*2/row - NOT a doubled destination pitch).
	 *  - planar (19): 3 planes, Y pitch = stride, U/V pitch = stride/2.
	 *  - NV12 semiplanar: the copy DEINTERLEAVES into 3 planes - the
	 *    destination layout is identical to planar (a separate V plane
	 *    is required; a NULL data[2] would SEGV the deinterleave loop). */
	size_t l0 = (size_t) stride;
	size_t l1, l2, rows1;
	if (p010) {
		l1 = (size_t) stride;
		l2 = 0;
	} else {
		l1 = (size_t) (stride / 2);
		l2 = (size_t) (stride / 2);
	}
	rows1 = (size_t) ((height + 1) / 2);
	/* one contiguous pool allocation: plane pointers carve it up. This
	 * replaces av_frame_get_buffer (measured ~14ms/frame in mmap + ~3200
	 * first-touch page faults at 4K P010; the pool recycles touched pages,
	 * measured wall: emit ceiling 40/s vs content 48/s). */
	total = l0 * (size_t) height + (l1 + l2) * rows1;
	uint8_t *base = dvhw_bufpool_take(&p->bufpool, total);
	dvhw_bufpool_hdr *hdr = (dvhw_bufpool_hdr *) base;
	hdr->owner = &p->bufpool;
	hdr->size = total + DVHW_BUFPOOL_HDR;
	uint8_t *mem = base + DVHW_BUFPOOL_HDR;
	AVBufferRef *buf = av_buffer_create(mem, total, dvhw_bufpool_release, NULL, 0);
	if (!buf) {
		av_free(base);
		av_frame_free(&f);
		f = NULL;
		goto done;
	}
	f->buf[0] = buf;
	f->data[0] = mem;
	f->linesize[0] = (int) l0;
	if (p010) {
		/* semiplanar: plane 1 is interleaved UV at pitch = stride */
		f->data[1] = mem + l0 * (size_t) height;
		f->linesize[1] = (int) l1;
		f->data[2] = NULL;
		f->linesize[2] = 0;
	} else {
		f->data[1] = mem + l0 * (size_t) height;
		f->linesize[1] = (int) l1;
		f->data[2] = l2 ? mem + l0 * (size_t) height + l1 * rows1 : NULL;
		f->linesize[2] = (int) l2;
	}
	f->buf[1] = NULL;
	f->pts = info->presentationTimeUs / 1000;

	if (p010) {
		/* MediaCodec P010: 16-bit samples with the 10-bit code in the MSBs
		 * (code << 6), NV12-style semiplanar layout (Y, then interleaved
		 * UV), stride in BYTES. Keep the layout EXACTLY and tag the frame
		 * AV_PIX_FMT_P010LE: its pixdesc (comp shift 6, depth 10) makes
		 * libplacebo's pl_plane_data_align produce
		 *   bits = { sample_depth 16, color_depth 10, bit_shift 6 }
		 * so pl_color_repr_normalize nets out to 1.0 and the shader reads
		 * code/1023 - the domain the DV reshape pivots/matrix and the
		 * RPU nonlinear matrix were authored for.
		 * (Tagging the deinterleaved planes YUV420P10LE was wrong: that
		 * descriptor says shift 0, so libplacebo scaled by 64, every
		 * signal above 1/64 passed the PQ EOTF pole, went NaN and
		 * rendered black.) */
		const uint8_t *y_src = src + info->offset;
		const uint8_t *uv_src = y_src + (size_t) stride * slice_height;
		f->format = AV_PIX_FMT_P010LE;
#ifdef DVHW_NULLCOPY_TEST
		memcpy(f->data[0], y_src, width * 2);
		memcpy(f->data[1], uv_src, width * 2);
#else
		for (y = 0; y < height; y++)
			memcpy(f->data[0] + y * f->linesize[0],
			       y_src + y * stride, width * 2);
		for (y = 0; y < height / 2; y++)
			memcpy(f->data[1] + y * f->linesize[1],
			       uv_src + y * stride, width * 2);
#endif
	} else if (color_format == DVHW_COLOR_FormatYUV420Planar) {
		/* planar fallback: Y, then U, then V planes */
		const uint8_t *y_src = src + info->offset;
		const uint8_t *u_src = y_src + stride * slice_height;
		const uint8_t *v_src = u_src + (stride / 2) * (slice_height / 2);
		for (y = 0; y < height; y++)
			memcpy(f->data[0] + y * f->linesize[0],
			       y_src + y * stride, width);
		for (y = 0; y < height / 2; y++) {
			memcpy(f->data[1] + y * f->linesize[1],
			       u_src + y * (stride / 2), width / 2);
			memcpy(f->data[2] + y * f->linesize[2],
			       v_src + y * (stride / 2), width / 2);
		}
	} else {
		/* semiplanar: Y plane, then interleaved UV (NV12) */
		const uint8_t *y_src = src + info->offset;
		const uint8_t *uv_src = y_src + stride * slice_height;
		for (y = 0; y < height; y++)
			memcpy(f->data[0] + y * f->linesize[0],
			       y_src + y * stride, width);
		for (y = 0; y < height / 2; y++) {
			const uint8_t *uv = uv_src + y * stride;
			uint8_t *u = f->data[1] + y * f->linesize[1];
			uint8_t *v = f->data[2] + y * f->linesize[2];
			for (x = 0; x < width / 2; x++) {
				u[x] = uv[2 * x];
				v[x] = uv[2 * x + 1];
			}
		}
	}

done:
	if (index >= 0)
		AMediaCodec_releaseOutputBuffer(codec, (size_t) index, false);
	return f;
}

static int dvhw_close(STREAM_DEC_VIDEO *dec)
{
	PRIV *p = (PRIV *) dec->priv;
	int i;

	if (!dec->is_open)
		return 0;

	/* stop the async threads: park them (flush handshake) so none is
	 * inside a codec call, then signal run=0 and join. The buffer pool
	 * is killed AFTER the joins: in-flight frames can still die later
	 * (the sink may hold AVFrames), but the hdr owner pointer handles
	 * that (dvhw_bufpool_release frees directly once dead).
	 *
	 * ORDER: the TH thread (dvhw_async_thread) is stopped FIRST. It is
	 * the only thread that cross-locks the other subsystems' mutexes
	 * (feed_mtx in the emit-wake path, pending_mtx in dvhw_pending_take,
	 * copyq.mtx when collecting worker copies), so it must be parked
	 * and joined BEFORE any of those mutexes are destroyed. The old
	 * order (feed -> copyq -> th) destroyed feed_mtx while the TH
	 * thread could still lock it: FORTIFY abort 'pthread_mutex_lock
	 * called on a destroyed mutex' in dvhw_async_thread at the
	 * emit-wake pthread_mutex_lock(&p->feed_mtx) (measured, tombstone
	 * 08, vc32 GoT run, exit path). */
	if (p->th_started) {
		pthread_mutex_lock(&p->th.mtx);
		p->flushing = 1;
		p->run = 0;
		pthread_cond_broadcast(&p->th.cond);
		while (p->th_busy)
			pthread_cond_wait(&p->th.cond, &p->th.mtx);
		pthread_mutex_unlock(&p->th.mtx);
		pthread_join(p->th_thread, NULL);
		p->th_started = 0;
	}
	if (p->feed_started) {
		pthread_mutex_lock(&p->feed_mtx);
		p->feed_park = 0;
		p->feed_run = 0;
		pthread_cond_broadcast(&p->feed_cond);
		pthread_mutex_unlock(&p->feed_mtx);
		pthread_join(p->feed_thread, NULL);
		p->feed_started = 0;
		pthread_mutex_destroy(&p->feed_mtx);
		pthread_cond_destroy(&p->feed_cond);
		pthread_mutex_destroy(&p->pending_mtx);
	}
	if (p->copyq_started) {
		pthread_mutex_lock(&p->copyq.mtx);
		p->copyq.run = 0;
		p->copyq.release_only = 0;
		pthread_cond_broadcast(&p->copyq.cond);
		pthread_mutex_unlock(&p->copyq.mtx);
		for (i = 0; i < p->copyq_started && i < DVHW_COPY_WORKERS; i++)
			pthread_join(p->copyq.thread[i], NULL);
		p->copyq_started = 0;
		pthread_mutex_destroy(&p->copyq.mtx);
		pthread_cond_destroy(&p->copyq.cond);
	}
	if (p->th_mtx_inited) {
		dvhw_in_q_clear(p);
		dvhw_out_q_clear(p);
		dvhw_bufpool_kill(&p->bufpool);
		pthread_mutex_destroy(&p->th.mtx);
		pthread_cond_destroy(&p->th.cond);
		p->th_mtx_inited = 0;
	}

	if (p->codec) {
		AMediaCodec_stop(p->codec);
		AMediaCodec_delete(p->codec);
		p->codec = NULL;
	}
	if (p->el_codec) {
		AMediaCodec_stop(p->el_codec);
		AMediaCodec_delete(p->el_codec);
		p->el_codec = NULL;
	}
	dvhw_el_q_clear(p);
	dvhw_bl_q_clear(p);
	dvhw_pending_clear(p);
	dec->is_open = 0;
	return 0;
}

static int dvhw_prepare(STREAM_DEC_VIDEO *dec, VIDEO_FRAME **frames, int num_frames)
{
	return 0;
}

static int dvhw_cleanup(STREAM_DEC_VIDEO *dec, VIDEO_FRAME **frames, int num_frames)
{
	int i;
	for (i = 0; i < num_frames; i++) {
		VIDEO_FRAME *f = frames[i];
		if (f && f->dec == dec) {
			/* OES path: AImage-backed dovi_hw_frame */
			if (f->handle[0]) {
				dvhw_frame_release((dovi_hw_frame *) f->handle[0]);
				f->handle[0] = NULL;
			}
			/* FEL path: BL/EL AVFrames in priv/handle[1] (same ownership
			 * model as codec_ffmpeg_video's cleanup) */
			if (f->priv) {
				av_frame_free((AVFrame **) &f->priv);
			}
			if (f->handle[1]) {
				av_frame_free((AVFrame **) &f->handle[1]);
				f->handle[1] = NULL;
			}
		}
	}
	return 0;
}



// av_buffer_create free callback: frees the AVDOVIMetadata struct
static void dvhw_meta_buf_free(void *opaque, uint8_t *data)
{
	(void) data;
	av_free(opaque);
}

/* Convert an hvcC/hvcE extradata to an Annex-B byte stream of its
 * parameter-set NAL arrays (VPS/SPS/PPS), prepended with start codes.
 * Samsung codec2 buffer-mode decoders need the parameter sets in-band
 * (csd-0 alone does not always kick off EL decoding); this mirrors what
 * mkvextract writes when extracting an EL track (proven to decode).
 * Returns an av_malloc'd buffer (caller frees) or NULL. */
static uint8_t *dvhw_hvcc_params_annexb(const uint8_t *hvcc, int size,
                                        int *out_size)
{
	/* hvcC layout: 0..21 fixed, 22 = numOfArrays, then per array:
	 *   1 byte (completeness|NAL_type<<1), 2 bytes numNalus, then per
	 *   NAL: 2 bytes length + payload (no emulation needed on copy) */
	int pos, i, a, n;
	uint8_t *buf = NULL;
	size_t total = 0;
	uint8_t *dst;

	if (!hvcc || size < 23)
		return NULL;
	if (!(hvcc[22] & 0xFF)) { /* numOfArrays == 0 */
		*out_size = 0;
		return NULL;
	}
	pos = 23;
	/* pass 1: size */
	for (a = 0; a < hvcc[22]; a++) {
		if (pos + 3 > size)
			return NULL;
		int num = (hvcc[pos + 1] << 8) | hvcc[pos + 2];
		pos += 3;
		for (n = 0; n < num; n++) {
			if (pos + 2 > size)
				return NULL;
			int nal = (hvcc[pos] << 8) | hvcc[pos + 1];
			if (pos + 2 + nal > size)
				return NULL;	/* declared NAL longer than the buffer:
					 * malformed/corrupt extradata - reject instead of
					 * over-reading in pass 2's memcpy (FFmpeg's hvcC
					 * parser bounds-checks every NAL length) */
			pos += 2 + nal;
			total += 4 + (size_t) nal;
		}
	}
	buf = (uint8_t *) av_malloc(total + AV_INPUT_BUFFER_PADDING_SIZE);
	if (!buf)
		return NULL;
	dst = buf;
	pos = 23;
	for (a = 0; a < hvcc[22]; a++) {
		int num = (hvcc[pos + 1] << 8) | hvcc[pos + 2];
		pos += 3;
		for (n = 0; n < num; n++) {
			int nal = (hvcc[pos] << 8) | hvcc[pos + 1];
			pos += 2;
			*dst++ = 0; *dst++ = 0; *dst++ = 0; *dst++ = 1;
			memcpy(dst, hvcc + pos, (size_t) nal);
			dst += nal;
			pos += nal;
		}
	}
	(void) i;
	*out_size = (int) total;
	return buf;
}

/* Convert a length-prefixed HEVC access unit to Annex-B (start codes).
 * Data already in Annex-B (lsize==0) passes through as a copy. */
static uint8_t *dvhw_au_to_annexb(const uint8_t *data, int size, int lsize,
                                  int *out_size)
{
	int prefix = lsize ? lsize : 4;
	size_t total = 0;
	int pos = 0, nal_size, pass;
	const uint8_t *nal;
	uint8_t *buf = NULL, *dst;

	for (pass = 0; pass < 2; pass++) {
		pos = 0;
		dst = buf;
		while ((nal = dovi_next_nal(data, size, lsize, &pos, &nal_size)) != NULL) {
			if (pass == 0) {
				total += 4 + (size_t) nal_size;
			} else {
				*dst++ = 0; *dst++ = 0; *dst++ = 0; *dst++ = 1;
				memcpy(dst, nal, (size_t) nal_size);
				dst += nal_size;
			}
		}
		if (pass == 0) {
			if (!total) {
				*out_size = 0;
				return NULL;
			}
			buf = (uint8_t *) av_malloc(total + AV_INPUT_BUFFER_PADDING_SIZE);
			if (!buf)
				return NULL;
		}
	}
	(void) prefix;
	*out_size = (int) total;
	return buf;
}

/* take the EL frame whose pts matches (exact match). Unlike the software
 * path there is no stale-drop here: the EL queue may legitimately lag
 * (async MediaCodec), and dvhw_fel_emit holds BL frames until their EL
 * arrives or is proven absent. */
static AVFrame *dvhw_el_take_pair(PRIV *p, int64_t bl_pts_ms)
{
	int i;
	AVFrame *el = NULL;
	for (i = 0; i < p->el_q_count; i++) {
		if (p->el_q[i]->pts == bl_pts_ms) {
			el = p->el_q[i];
			memmove(&p->el_q[i], &p->el_q[i + 1],
			        (p->el_q_count - i - 1) * sizeof(p->el_q[0]));
			p->el_q_count--;
			break;
		}
	}
	return el;
}
static void dvhw_el_feed(STREAM_DEC_VIDEO *dec)
{
	PRIV *p = (PRIV *) dec->priv;
	STREAM *s = (STREAM *) dec->ctx;
	AVPacket el_pkt;

	if (!p->el_codec || !s || !s->parser || !s->parser->get_dovi_el_packet)
		return;
	/* teardown race (async layer): stream_close frees the parser's priv
	 * (s->parser_priv, the ff_p the EL pull derefs) BEFORE our decode
	 * thread is joined - the sync path never saw this because it ran on
	 * the engine thread, fully serialized behind stream_close. s->aborted
	 * is set at the top of the close sequence and the STREAM object
	 * itself outlives dvhw_close, so it is a safe stop signal here.
	 * (measured crash: _get_dovi_el_packet SEGV via dvhw_el_feed from
	 * dvhw_async_thread during teardown) */
	if (s->aborted)
		return;
	/* also bail while the stream is re-opening the parser (seek between
	 * files): parser_priv swaps under us there too */
	if (!s->parser_open)
		return;
	/* mpv f_enhancement_pair parity: the EL decoder is fed at the same
	 * rate the BL AUs are consumed (plus a small window for the EL's
	 * reorder lag). Unthrottled feeding lets the EL decoder race seconds
	 * ahead of the BL output position on locally-buffered files; the
	 * 64-deep el_q then overflows and drops exactly the ELs the upcoming
	 * BLs need, and every frame emits BL-only (measured: elq0 = bl+918ms,
	 * elq=64, 5/8 BL-only) — FEL metadata without the residual renders
	 * green/garbage. The EL must be AHEAD of the BL output, not behind:
	 * the BL emits its Nth frame only after its reorder window is fed, so
	 * pace the EL feed against the BL INPUT count. */
	/* The feed gate paces EL INPUT, but the EL codec's OUTPUT must be
	 * drained unconditionally: MediaCodec buffer-mode wedges when all
	 * output buffers are held and nobody dequeues them (measured: EL
	 * stopped after 36 frames, fed frozen, every BL emitted EL-less).
	 * mpv f_enhancement_pair polls its EL filter pin on every frame
	 * regardless of input pacing. */
	/* WARM-UP: until the EL codec emits its first frame (el_seen), it
	 * holds ~24 AUs in-flight (measured 1187-1163=24 steady, but the
	 * first frame sometimes needs MORE - two separate runs froze at
	 * el_fed=38/el_out=0 waiting: the c2 EL pipeline depth is variable
	 * at startup and bl_fed is itself credit-blocked waiting for ELs,
	 * so a bl_fed-derived budget is circular and deadlocks). The floor:
	 * feed the EL until el_seen OR 96 AUs (2s of content at 48fps)
	 * unconditionally - once the first EL pairs, park drains, BL credits
	 * free, and the tight bl_fed+DVHW_EL_CATCHUP pace takes over. A
	 * deep floor cannot wedge the sink: frames only EMIT when paired,
	 * and the park gate bounds decode; it only risks ~2s of EL-only
	 * pre-decode at startup/seek, discarded by pairing if unneeded.
	 * Seek/flush resets both counters, so seek recovery re-primes too.
	 * FLUSH SHORT-FLOOR: after a SEEK the codec is already warm (its
	 * pipeline spin-up is done - only its reorder window, ~DVHW_EL_CATCHUP
	 * frames, must refill), so the 96-AU floor only re-primes the
	 * DECODE-AHEAD, not codec spin-up: the measured post-seek recovery
	 * window is ~4-5s, of which the floor's 96 AUs at ~44 AU/s feed
	 * rate is ~2s of EL-only pre-decode the pair logic then discards.
	 * el_seen carries across flush (the codec proved warm once), so the
	 * floor only applies to a genuinely cold codec. */
	int el_budget = p->bl_fed_count + DVHW_EL_CATCHUP;
	if (!p->el_seen) {
		int warm_floor = 96;
		if (el_budget < warm_floor)
			el_budget = warm_floor;
	}
	int el_gated = (p->el_fed_count >= el_budget);
	/* parser-side EL exhaustion: observed by the FEED LOOP itself (the
	 * only place with a legitimate pull right) - when the parser queue
	 * returns empty, remember it. dvhw_fel_emit's EOF-tail policy needs
	 * 'no future EL exists' as an input (drain == fed alone is NOT proof
	 * mid-file: the EL feed is catch-up paced, so between AUs the counts
	 * can transiently match while later EL packets are still coming).
	 * Do NOT probe by pulling here: get_dovi_el_packet DEQUUES (ownership
	 * moves to the caller) - a pull-and-unref "probe" destroys one EL
	 * packet per BL decode call and eats half the EL stream (measured
	 * collapse to single-digit fps on 48fps FEL). Cleared on every
	 * successful pull and on flush (seek). */
	/* held-over EL AU from the previous call (the EL codec input was
	 * full then): consume it BEFORE any new pull - the previous code
	 * unref'd it (silent EL loss: its BL partner emitted EL-less every
	 * time the input deque lapsed the 8ms timeout). */
	int hold_pending = p->el_hold_valid;
	AVPacket el_pkt_hold;
	if (hold_pending) {
		el_pkt_hold = p->el_hold_pkt;
		p->el_hold_valid = 0;
	}
	if (el_gated && !hold_pending)
		goto drain_el;
	/* BUDGET RECHECK INSIDE the loop, not only at entry: the loop pulls
	 * one packet per iteration and can cross the budget mid-loop (the
	 * warm-up allowance makes the entry gate permissive until el_seen;
	 * without an in-loop check the loop runs to parser-empty - measured
	 * el_fed creep to 38 with bl_fed=10 frozen). The warm-up exit is
	 * el_seen itself: the +24 allowance exists to prime the EL codec
	 * until its FIRST frame emerges, then the tight
	 * bl_fed+DVHW_EL_CATCHUP pace takes over. */
	while ((hold_pending ||
		        (!el_gated && s->parser->get_dovi_el_packet(s, &el_pkt) == 0))) {
		uint8_t *clean = NULL;
		int clean_size = 0;
		ssize_t idx;
		if (hold_pending) {
			el_pkt = el_pkt_hold;
			hold_pending = 0;
		}
		/* decoder-framedrop pairing: this EL packet's BL was skipped at
		 * dec_in (el_skip ring). Discard it WITHOUT feeding the EL codec
		 * and WITHOUT advancing el_fed_count, so the exact-pts pairing
		 * stays 1:1 (a fed EL whose BL never exists just wedges el_q as
		 * stale). `continue` re-enters the while condition which pulls
		 * the next packet (it may also be a skip). */
		{
			pthread_mutex_lock(&p->th.mtx);
			int sk, found = -1;
			for (sk = 0; sk < p->el_skip.count; ) {
				int64_t spts = p->el_skip.pts_us[
					(p->el_skip.head + sk) % DVHW_EL_SKIP_RING];
				if (spts == (int64_t) el_pkt.pts * 1000) {
					found = sk;
					sk++;
					continue;
				}
				if (spts < (int64_t) el_pkt.pts * 1000) {
					/* stale entry (its BL window passed): drop from ring */
					int mv;
					for (mv = sk; mv < p->el_skip.count - 1; mv++)
						p->el_skip.pts_us[(p->el_skip.head + mv) %
							  DVHW_EL_SKIP_RING] =
							p->el_skip.pts_us[(p->el_skip.head + mv + 1) %
							  DVHW_EL_SKIP_RING];
					p->el_skip.count--;
					continue;	/* re-examine position sk */
				}
				sk++;
			}
			if (found >= 0) {
				/* remove the matched entry, then discard the packet */
				int mv;
				for (mv = found; mv < p->el_skip.count - 1; mv++)
					p->el_skip.pts_us[(p->el_skip.head + mv) %
						  DVHW_EL_SKIP_RING] =
					p->el_skip.pts_us[(p->el_skip.head + mv + 1) %
						  DVHW_EL_SKIP_RING];
				p->el_skip.count--;
			}
			pthread_mutex_unlock(&p->th.mtx);
			if (found >= 0) {
				av_packet_unref(&el_pkt);
				continue;	/* while condition pulls the next packet */
			}
		}
		/* 0-timeout (NOT the BL's 8ms): when the EL codec input is full
		 * this must fail FAST - the packet goes to the hold stash and the
		 * next loop iteration retries. An 8ms block here ran once per
		 * feed-advance (~40/s) and stole ~30% of the decode thread's
		 * wall time (measured: emit ceiling 40/s vs content 48/s).
		 * mpv parity: f_enhancement_pair feeds its EL filter pin
		 * synchronously and never blocks; the hold stash is the async
		 * equivalent of "leave it queued in the parser". */
		idx = AMediaCodec_dequeueInputBuffer(p->el_codec, 0);
		if (idx < 0) {
			/* hold the packet losslessly for the next call - the flush path
			 * (dvhw_el_q_clear / seek) unrefs it */
			p->el_hold_pkt = el_pkt;
			p->el_hold_valid = 1;
			break;	// EL codec busy: retry held packet next call
		}
		/* strip any RPU NAL from the EL access unit (the EL's own RPU is
		 * not needed: the BL RPU carries the composition metadata), then
		 * convert to Annex-B: Samsung codec2 buffer-mode decoders need
		 * start-code framing, and the EL parameter sets must arrive
		 * in-band on the first AU (csd-0 alone does not always start
		 * EL decoding — proven by the EL-track extraction test where
		 * in-band VPS/SPS/PPS decode fine) */
		dovi_strip_dv_nals(el_pkt.data, el_pkt.size,
		                   p->el_nal_length_size, &clean, &clean_size);
		{
			uint8_t *annexb = NULL;
			int annexb_size = 0;
			if (clean && clean_size > 0)
				annexb = dvhw_au_to_annexb(clean, clean_size,
							 p->el_nal_length_size,
							 &annexb_size);
			av_free(clean);
			clean = annexb;
			clean_size = annexb_size;
			if (clean && clean_size > 0 && !p->el_params_sent) {
				/* prepend VPS/SPS/PPS from the EL hvcC to the first AU */
				int ps_size = 0;
				uint8_t *ps = dvhw_hvcc_params_annexb(
					(const uint8_t *) dec->video->dv_el_extraData,
					dec->video->dv_el_extraDataSize, &ps_size);
				if (ps && ps_size > 0) {
					uint8_t *merged = (uint8_t *) av_malloc(
						ps_size + clean_size +
						AV_INPUT_BUFFER_PADDING_SIZE);
					if (merged) {
						memcpy(merged, ps, (size_t) ps_size);
						memcpy(merged + ps_size, clean,
						       (size_t) clean_size);
						av_free(clean);
						clean = merged;
						clean_size += ps_size;
					}
					av_free(ps);
				}
				p->el_params_sent = 1;
			}
		}
		if (!clean || clean_size <= 0) {
			av_free(clean);
			av_packet_unref(&el_pkt);
			continue;
		}
		{
			size_t buf_size = 0;
			uint8_t *buf = AMediaCodec_getInputBuffer(p->el_codec,
			                                        (size_t) idx, &buf_size);
			if (buf && clean_size <= (int) buf_size) {
				memcpy(buf, clean, (size_t) clean_size);
				/* EL pts (parser already converted to the avos ms domain)
				 * queues as the MediaCodec timestamp; output pts returns in
				 * the same domain for pairing */
				AMediaCodec_queueInputBuffer(
					p->el_codec, (size_t) idx, 0,
					(size_t) clean_size,
					(int64_t) el_pkt.pts * 1000, 0);
				p->el_fed_count++;
			}
		}
		av_free(clean);
		av_packet_unref(&el_pkt);
		p->el_exhausted = 0;	/* parser just produced one: not EOF */
		if (p->el_fed_count >= p->bl_fed_count + DVHW_EL_CATCHUP &&
		    (p->el_seen || p->el_fed_count >= el_budget))
			break;
	}
	/* reaching here with !el_gated means the parser queue ran dry mid-
	 * budget. That is only EOF evidence when the demuxer itself is at
	 * EOF: mid-file the EL queue runs transiently empty between AUs
	 * (parse paces 1:1 with BL AUs on interleaved FEL) and a premature
	 * el_exhausted makes dvhw_fel_emit emit BL-only frames whose ELs
	 * are a few packets behind - green/flat FEL frames mid-file
	 * (measured flapping before the gate). s->video_parse_end is the
	 * parser's own EOF flag (set on av_read_frame failure, cleared on
	 * seek), so gate on it: queue-dry + demux-EOF = real EOF tail.
	 * A later successful pull (or a seek flush) clears it. */
	if (!el_gated)
		p->el_exhausted = (s->video_parse_end != 0);

	/* unconditional EL output drain - runs even when input is gated */
drain_el:
	/* pull ALL ready EL frames: EL outputs lag BL by the reorder depth
	 * (B-frames), so draining one-per-input would leave every EL frame
	 * arriving after its BL partner already left (dropped as stale) */
	{
		for (;;) {
			AMediaCodecBufferInfo einfo;
			ssize_t oidx = AMediaCodec_dequeueOutputBuffer(p->el_codec,
			                                         &einfo, 0);
			if (oidx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
				AMediaFormat *ofmt = AMediaCodec_getOutputFormat(p->el_codec);
				int32_t v = 0;
				if (ofmt) {
					if (AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_WIDTH, &v) && v > 0)
						p->el_width = v;
					if (AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_HEIGHT, &v) && v > 0)
						p->el_height = v;
					if (AMediaFormat_getInt32(ofmt, "stride", &v) && v > 0)
						p->el_stride = v;
					if (AMediaFormat_getInt32(ofmt, "slice-height", &v) && v > 0)
						p->el_slice_height = v;
					if (AMediaFormat_getInt32(ofmt, "color-format", &v))
						p->el_color_format = v;
					AMediaFormat_delete(ofmt);
				}
			serprintf(TAG ": EL output format %dx%d stride %d slice %d color %d\n",
			          p->el_width, p->el_height, p->el_stride,
			          p->el_slice_height, p->el_color_format);
			continue;
		}
		if (oidx < 0)
			break;
		if (einfo.size <= 0) {
			/* zero-size Codec2 event buffer: must be released, see the
			 * BL drain loop above (same measured ~50% output wedge) */
			AMediaCodec_releaseOutputBuffer(p->el_codec, (size_t) oidx, false);
			p->stat_el_zerolen++;
			continue;
		}
			AVFrame *el = dvhw_copy_yuv420(p, p->el_codec, oidx, &einfo,
			                               p->el_width, p->el_height,
			                               p->el_stride, p->el_slice_height,
			                               p->el_color_format, 0);
			if (!el)
				break;
			el->color_primaries = AVCOL_PRI_BT2020;
			el->color_trc      = AVCOL_TRC_SMPTE2084;
			el->colorspace     = AVCOL_SPC_BT2020_NCL;
			el->color_range    = AVCOL_RANGE_MPEG;
			p->el_drain_count++;
			p->el_seen = 1;
			if (p->el_q_count >= DVHW_PENDING_MAX) {
				av_frame_free(&p->el_q[0]);
				memmove(&p->el_q[0], &p->el_q[1],
				        (DVHW_PENDING_MAX - 1) * sizeof(p->el_q[0]));
				p->el_q_count--;
			}
			p->el_q[p->el_q_count++] = el;
			continue;
		}
	}
}

/* FEL emission: the BL output queue (bl_q) holds decoded BL frames whose EL
 * has not arrived yet; emit the head when its EL is present, when a later
 * EL proves it absent (pts jumped past), or when the queue is full. Returns
 * the frame to hand out or NULL (nothing ready). mpv f_enhancement_pair
 * pending-queue semantics. */
static AVFrame *dvhw_fel_emit(PRIV *p, AVFrame **el_out)
{
	AVFrame *bl = NULL;
	*el_out = NULL;
	if (!p->bl_q_count)
		return NULL;
	/* No EL decoder (P8.x / P7 BL-only): nothing to pair with, the BL
	 * emits as soon as it decodes. */
	if (!p->fel) {
		bl = p->bl_q[0];
		goto emit;
	}
	bl = p->bl_q[0];
	*el_out = dvhw_el_take_pair(p, bl->pts);
	if (*el_out)
		goto emit;
	/* mpv f_enhancement_pair parity, three affirmative-evidence policies:
	 *
	 * 1. EL older than the oldest BL: its BL partner already left (or
	 *    never existed) - the EL is stale, drop it and retry. Holding it
	 *    only wedges the queue head (mpv: "dropping stale EL").
	 * 2. EL newer than the oldest BL: this BL's EL will never come -
	 *    emit BL-only (mpv give_up: el_newer).
	 * 3. Queue pressure: emit BL-only ONLY after the EL decoder has
	 *    produced at least one frame since the last flush. During EL
	 *    warm-up (startup and after seeks) a full bl_q only means the
	 *    EL decoder is still spinning up - BL-only frames there render
	 *    green (FEL metadata without residual). mpv commit 3b4caf0
	 *    ("don't emit BL-only frames before the EL warms up").
	 */
	while (p->el_q_count && p->el_q[0]->pts < bl->pts) {
		av_frame_free(&p->el_q[0]);
		memmove(&p->el_q[0], &p->el_q[1],
		        (p->el_q_count - 1) * sizeof(p->el_q[0]));
		p->el_q_count--;
	}
	if (p->el_q_count && p->el_q[0]->pts > bl->pts)
		goto emit;	/* proven absent: EL jumped past this BL */
	if (p->bl_q_count >= DVHW_BL_PARK_MAX && p->el_seen)
		goto emit;	/* queue pressure, EL warm. The threshold must stay
			 * at/below the feed gate (DVHW_BL_PARK_MAX): with the
			 * gate alone stopping the feed at a full park, an 8-deep
			 * pressure test could never re-arm - a lost-EL head
			 * locked the pipeline at a full-but-unreleasable park */
	/* EL fully drained AND the parser has no more EL packets (probed
	 * in dvhw_el_feed, cleared on flush): the parked BLs' ELs will never
	 * arrive - emit BL-only, mpv's el_eof give-up (f_enhancement_pair.c:
	 * EOF drains the pending queue as BL-only). Without this the last
	 * 1-3 frames of every FEL file stay parked forever (their ELs are
	 * still inside the EL codec's reorder window when the AU stream
	 * ends) and the file end truncates. drain==fed alone is not proof
	 * mid-file (feed is catch-up paced), hence the el_exhausted gate. */
	if (p->el_seen && p->el_exhausted &&
	    p->el_drain_count >= p->el_fed_count &&
	    p->el_q_count == 0)
		goto emit;
	return NULL;
emit:
	p->stat_emit++;
	if (p->stat_pts_last != -1) {
		int d = (int)(bl->pts - p->stat_pts_last);
		if (d < p->stat_pts_dmin) p->stat_pts_dmin = d;
		if (d > p->stat_pts_dmax) p->stat_pts_dmax = d;
			if (d < 0) {
			/* EMIT-ORDER VIOLATION dump: emitted pts stepped backward.
			 * With the emission-order gate this is an invariant breach -
			 * it fired on every build before the gate (pts stepping
			 * -41..-209ms, i.e. frames shown 1-5 slots out of order, the
			 * visible judder) and is silent with the gate healthy. Dump
			 * the dequeue ring (codec output order), the parked queue,
			 * and the copy pipeline state to localize the breach. */
			serprintf(TAG ": EMIT-ORDER VIOLATION d=%d bl_pts=%d last=%d  dq_ring:",
				  d, (int)bl->pts, p->stat_pts_last);
			for (int i = 0; i < 8; i++) {
				int idx = (p->dq_ring_w + i) & 7;
				serprintf(" %lld", (long long)(p->dq_ring[idx] / 1000));
			}
			serprintf("  bl_q:");
			for (int i = 0; i < p->bl_q_count; i++)
				serprintf(" %d", (int)p->bl_q[i]->pts);
			serprintf("  inflight_us=%lld busy=[%lld %lld] cnt=%d\n",
				  (long long)dvhw_copyq_oldest_inflight_us(&p->copyq),
				  (long long)p->copyq.busy_pts_us[0],
				  (long long)p->copyq.busy_pts_us[1],
				p->copyq.count);
		}
	}
	p->stat_pts_last = (int) bl->pts;
	p->emit_user_id = p->bl_user_id[0];
	memmove(&p->bl_q[0], &p->bl_q[1],
	        (p->bl_q_count - 1) * sizeof(p->bl_q[0]));
	memmove(&p->bl_user_id[0], &p->bl_user_id[1],
	        (p->bl_q_count - 1) * sizeof(p->bl_user_id[0]));
	p->bl_q_count--;
	return bl;
}

/* ---------------------------------------------------------------
 * Async decode layer (sfdec2/codec_lavc_async contract, mpv model)
 *
 * The synchronous decode2 path serialized the whole pipeline: the
 * engine signals done=1, waits (1ms poll) for the decode thread,
 * which runs decode2 (measured 12-13ms wall on 4K FEL: binder
 * round-trips + 12MB BL copy + 3MB EL copy), then done=2. Round trip
 * ~23ms/AU = ~43 AU/s ceiling - below the 48 of this content. Video
 * time then falls behind audio at ~5 frames/s until the sink's
 * late policy eats frames (the "48fps slowed to half" symptom).
 *
 * With dec->async=1 the engine switches to _stream_player_async and
 * calls dec_in/put_out/get_out instead: dec_in copies the AU into
 * an internal queue and returns immediately; an internal thread
 * feeds MediaCodec + drains EL + pairs FEL frames into slots handed
 * via put_out; get_out collects finished frames. The 12ms decode
 * cost now overlaps the engine loop - sustained rate is the codec's
 * own throughput (48fps), not the round trip.
 *
 * DVHW_IN_Q_MAX: engine-side AU backlog (mpv demux slack ~8-10 AUs).
 */
static int dvhw_dec_in(STREAM_DEC_VIDEO *dec, VIDEO_FRAME **data_frame,
                      int *pdecoded, int *ptime)
{
	PRIV *p = (PRIV *) dec->priv;
	VIDEO_FRAME *d = *data_frame;

	if (pdecoded)
		*pdecoded = 0;
	if (ptime)
		*ptime = 0;
	if (!p->codec || !p->run)
		return 1;

	pthread_mutex_lock(&p->th.mtx);
	if (p->flushing) {
		/* engine still holds AU bytes: not consumed, retry after flush */
		pthread_mutex_unlock(&p->th.mtx);
		return 0;
	}
	if (p->in_q_count >= DVHW_IN_Q_MAX - 1) {
		/* internal backlog full (one slot RESERVED for the feed thread's
	 * AU requeue - the codec-input-full retry can otherwise race this
	 * push in the pop->requeue window and drop an AU, measured 2-7/s):
	 * not consumed - the engine's cdata stays, dec_in is retried next
	 * loop (pool backpressure) */
		pthread_mutex_unlock(&p->th.mtx);
		return 0;
	}
	pthread_mutex_unlock(&p->th.mtx);

	/* strip outside the lock: RPU extraction + NAL rewrite are the
	 * expensive part (~1ms) and touch no shared state */
	uint8_t *rpu = NULL, *clean = NULL;
	int rpu_size = 0, clean_size = 0;
	dovi_extract_rpu(d->data[0], d->size, p->nal_length_size, &rpu, &rpu_size);
	if (dovi_strip_dv_nals(d->data[0], d->size, p->nal_length_size,
	                       &clean, &clean_size) || !clean || clean_size <= 0) {
		/* metadata-only AU: consumed, nothing to decode */
		av_free(rpu);
		av_free(clean);
		if (pdecoded)
			*pdecoded = d->size;
		return 0;
	}

	/* mpv --framedrop=decoder: video behind the heard clock by >2 frames
	 * and this AU is a disposable _N frame: skip it entirely. The stream
	 * advances faster than realtime until it catches up (fixes the
	 * unbounded-deficit unsync: codec sustains ~44/s on 48fps content).
	 * Record the pts so dvhw_el_feed discards the paired EL packet too. */
	if (p->run && dvhw_dec_in_should_drop(p, dec, d, clean, clean_size)) {
		av_free(rpu);
		av_free(clean);
		pthread_mutex_lock(&p->th.mtx);
		p->el_skip.pts_us[(p->el_skip.head + p->el_skip.count) %
		                  DVHW_EL_SKIP_RING] = (int64_t) d->time * 1000;
		if (p->el_skip.count < DVHW_EL_SKIP_RING)
			p->el_skip.count++;
		else
			p->el_skip.head = (p->el_skip.head + 1) % DVHW_EL_SKIP_RING;
		p->stat_dec_drop++;
		pthread_mutex_unlock(&p->th.mtx);
		if (pdecoded)
			*pdecoded = d->size;
		return 0;		/* consumed, never decoded */
	}

	dvhw_au_t au = { clean, clean_size, rpu, rpu_size, d->user_ID, (int64_t) d->time * 1000 };
	pthread_mutex_lock(&p->th.mtx);
	if (p->flushing || !p->run || p->in_q_count >= DVHW_IN_Q_MAX - 1) {
		pthread_mutex_unlock(&p->th.mtx);
		av_free(rpu);
		av_free(clean);
		return 0;	/* not consumed (slot reserved: see above) */
	}
	p->in_q[p->in_q_write] = au;
	p->in_q_write = (p->in_q_write + 1) % DVHW_IN_Q_MAX;
	p->in_q_count++;
	pthread_cond_broadcast(&p->th.cond);
	pthread_mutex_unlock(&p->th.mtx);
	/* new AU: wake the feed worker's gate re-check */
	pthread_mutex_lock(&p->feed_mtx);
	pthread_cond_broadcast(&p->feed_cond);
	pthread_mutex_unlock(&p->feed_mtx);

	if (pdecoded)
		*pdecoded = d->size;
	return 0;
}

/* HEVC first-slice NAL type: returns 1 when the AU's first slice NAL is
 * NON-REFERENCE (types 0=TRAIL_N, 2=TSA_N, 4=STSA_N, 6=RADL_N, 8=RASL_N;
 * spec 7.4.2.5 Table 7-1). _N pictures are never referenced - skipping one
 * at the decoder input cannot corrupt any other frame (the property mpv's
 * --framedrop=decoder relies on). Returns 0 for _R slices / IDR / CRA /
 * unparseable AUs (fail-safe: decode rather than corrupt). */
static int dvhw_au_is_nonref(const uint8_t *au, int size, int nal_len_size)
{
	const uint8_t *p = au;
	const uint8_t *end = au + size;
	if (nal_len_size > 4)
		return 0;
	if (nal_len_size < 1) {
		/* Annex-B framing (MKVs with start-code AUs: measured on Charles
		 * - nal_length_size 0, cleaned AU starts 00 00 00 01): scan start
		 * codes to the first slice NAL. */
		while (p + 4 <= end) {
			int sc = 0;
			if (p[0] == 0 && p[1] == 0) {
				if (p[2] == 1) { sc = 3; }
				else if (p[2] == 0 && p + 4 <= end && p[3] == 1) { sc = 4; }
			}
			if (!sc) {
				p++;
				continue;
			}
			p += sc;
			if (p + 2 > end)
				return 0;
			int nal_type = (p[0] >> 1) & 0x3f;
			if (nal_type >= 32)
				continue;		/* VPS/SPS/PPS/SEI: next start code */
			if (nal_type <= 9)
				return (nal_type == 0 || nal_type == 2 ||
				        nal_type == 4 || nal_type == 6 || nal_type == 8);
			return 0;	/* >= IDR_W_RADL: reference picture */
		}
		return 0;
	}
	while (p + nal_len_size + 2 <= end) {
		uint32_t n = 0;
		int i;
		for (i = 0; i < nal_len_size; i++)
			n = (n << 8) | p[i];
		p += nal_len_size;
		if (n == 0 || p + n > end)
			return 0;		/* malformed: fail-safe */
		int nal_type = (p[0] >> 1) & 0x3f;
		if (nal_type >= 32) {
			p += n;		/* VPS/SPS/PPS/SEI: skip to the first slice NAL */
			continue;
		}
		if (nal_type <= 9)
			return (nal_type == 0 || nal_type == 2 ||
			        nal_type == 4 || nal_type == 6 || nal_type == 8);
		return 0;		/* >= IDR_W_RADL: reference picture, decode */
	}
	return 0;
}

/* mpv --framedrop=decoder parity: video running behind the heard-audio
 * clock AND the AU is a disposable _N frame: skip it at the decoder INPUT.
 * The signal is the PRESENTED position (s->video_time, updated from sink
 * frame recycles) vs heard audio - NOT the AU pts vs the sink clock: the
 * AU arriving here is decode-ahead input (~700ms in front of the clock via
 * in_q + codec reorder), so an AU-based check never fires (measured:
 * decdrop stuck at 0 behind a 3.3s deficit). Dropping _N AUs advances the
 * content position faster than realtime until the deficit closes (codec
 * sustains ~44/s on 48fps content). The dropped pts goes to the el_skip
 * ring so the EL packet is discarded too (pairing stays 1:1). */
static int dvhw_dec_in_should_drop(PRIV *p, STREAM_DEC_VIDEO *dec,
                                    VIDEO_FRAME *d,
                                    const uint8_t *clean, int clean_size)
{
	STREAM *s = (STREAM *) dec->ctx;
	(void) d;
	/* 1Hz guard diagnostic - FIRST, before any early return, so the
	 * drop decision inputs are always visible. Timer is PER-INSTANCE
	 * (review finding 13: the function-static it replaces was shared
	 * across live codec instances). */
	{
		struct timespec tsd;
		clock_gettime(CLOCK_MONOTONIC, &tsd);
		int64_t now_us = (int64_t) tsd.tv_sec * 1000000 + tsd.tv_nsec / 1000;
		if (now_us - p->stat_fdguard_last_us >= 1000000) {
			int nt = -1, nonref = -1;
			if (d && clean && clean_size > 0) {
				const uint8_t *pd = clean;
				const uint8_t *ed = clean + clean_size;
				if (pd + p->nal_length_size + 2 <= ed) {
					uint32_t n = 0;
					int i;
					for (i = 0; i < p->nal_length_size; i++)
						n = (n << 8) | pd[i];
					pd += p->nal_length_size;
					if (n > 0 && pd + n <= ed)
						nt = (pd[0] >> 1) & 0x3f;
				}
				nonref = dvhw_au_is_nonref(clean, clean_size,
				                       p->nal_length_size);
			}
			serprintf(TAG ": fdguard au=%d sink=%d vtime=%d atime=%d nal=%d nonref=%d clnsz=%d b0=%02x b1=%02x b2=%02x b3=%02x\n",
			  d ? d->time : -999,
			  (s && s->video_sink && s->video_sink->get_time)
				  ? s->video_sink->get_time(s->video_sink) : -999,
			  s ? s->video_time : -999,
			  s ? s->audio_time : -999,
			  nt, nonref,
			  clean ? clean_size : -1,
			  (clean && clean_size > 0) ? clean[0] : 0,
			  (clean && clean_size > 1) ? clean[1] : 0,
			  (clean && clean_size > 2) ? clean[2] : 0,
			  (clean && clean_size > 3) ? clean[3] : 0);
			p->stat_fdguard_last_us = now_us;
		}
	}
	if (!s || !s->audio || !s->audio->valid || s->audio_time < 0)
		return 0;
	if (s->video_time < 0)
		return 0;		/* nothing presented yet */
	int frame_ms = 21;		/* default; refined below */
	if (dec->video && dec->video->frame_rate_num > 0 &&
	    dec->video->frame_rate_den > 0)
		frame_ms = (int) ((int64_t) 1000 * dec->video->frame_rate_den /
		                  dec->video->frame_rate_num);
	if (frame_ms <= 0)
		frame_ms = 21;
	/* SIGNAL = AU pts vs the anchored presentation clock. In the
	 * deficit regime the content position trails the heard anchor by
	 * hundreds of ms to seconds (measured: au 30.6s behind the clock
	 * when decode sustains ~40/s on 48fps content) - dropping _N AUs
	 * here advances the stream position faster than realtime until
	 * caught up. (The earlier heard-vs-video_time signal never fired:
	 * video_time tracks PRESENTED pts, which lags the AUs by the whole
	 * pipeline, so both sides carried the same deficit and canceled.) */
	if (!s->video_sink || !s->video_sink->get_time)
		return 0;
	int now = s->video_sink->get_time( s->video_sink );
	if (now < 0)
		return 0;
	int late_by = now - d->time;	/* >0: AU content trails the clock; <0: AU LEADS */
	/* TARGET = dec_in LEADS the clock by the decode pipeline latency
	 * (BL codec B-pyramid reorder ~24 AUs + copy + emit + engine ≈
	 * 400-500ms at 48fps). Emitted frames then reach the sink AT their
	 * blit_time and the sink drain (504ms) stays quiet. Clamping au
	 * AT the clock (late_by<=100) left every emitted frame ~500ms stale
	 * on arrival - the drain skipped half of them forever (measured:
	 * decdrop 10-12/s + skip 14-22/s + pres 18-29/s while the venc sat
	 * 700-930ms/s idle). Decode everything while the lead is ≥350ms;
	 * when the lead shrinks (clock drift, decode dips), drop _N frames
	 * to restore it - mpv's decoder-framedrop exactly. */
	if (late_by <= -350)
		return 0;		/* AU leads the clock by ≥350ms: on schedule */
	int nonref = dvhw_au_is_nonref(clean, clean_size, p->nal_length_size);
	/* (deficit + NAL verdict printed by the fdguard diag at function
	 * top - one shared 1Hz timer, no duplicate block) */
	return nonref;
}

static int dvhw_put_out(STREAM_DEC_VIDEO *dec, VIDEO_FRAME **pin_frame)
{
	PRIV *p = (PRIV *) dec->priv;

	/* engine calls put_out(dec, NULL) when it has no free slot (the
	 * else branch at the put_out call site) - pin_frame itself can be
	 * NULL, that is a no-op handshake, not an error (sfdec2 parity) */
	if (!pin_frame)
		return 0;
	VIDEO_FRAME *f = *pin_frame;

	*pin_frame = NULL;
	if (!f)
		return 0;

	/* The engine recycles slots: a frame it dropped without rendering
	 * (SEEK_DROP / pause-discard) still carries our AVFrame payloads.
	 * render frees them on the normal retire path; on this path nobody
	 * does - free here or they leak one BL+EL per dropped frame. */
	if (f->priv)
		av_frame_free((AVFrame**) &f->priv);
	if (f->handle[1])
		av_frame_free((AVFrame**) &f->handle[1]);
	f->valid = 0;

	pthread_mutex_lock(&p->th.mtx);
	frame_q_put(&p->slot_q, f);
	p->slot_count++;
	pthread_cond_broadcast(&p->th.cond);
	pthread_mutex_unlock(&p->th.mtx);
	/* slot arrived: wake the feed worker's gate re-check */
	pthread_mutex_lock(&p->feed_mtx);
	pthread_cond_broadcast(&p->feed_cond);
	pthread_mutex_unlock(&p->feed_mtx);
	return 0;
}

static int dvhw_get_out(STREAM_DEC_VIDEO *dec, VIDEO_FRAME **pout_frame)
{
	PRIV *p = (PRIV *) dec->priv;

	pthread_mutex_lock(&p->th.mtx);
	*pout_frame = frame_q_get(&p->out_q);
	pthread_mutex_unlock(&p->th.mtx);
	if (*pout_frame)
		(*pout_frame)->valid = 1;
	return 0;
}

/* fill a pool slot with a paired BL(+EL) - the exact field set the
 * synchronous decode2 produced on emit */
static void dvhw_fill_slot(PRIV *p, VIDEO_FRAME *avos_frame,
                           AVFrame *bl, AVFrame *el, int user_id)
{
	(void) p;
	avos_frame->valid = 1;
	avos_frame->error = 0;
	avos_frame->handle[1] = (void*) el;
	avos_frame->priv = (void*) bl;
	avos_frame->width = bl->width;
	avos_frame->height = bl->height;
	avos_frame->pts = bl->pts;
	avos_frame->time = (int) bl->pts;
	avos_frame->user_ID = user_id;
	avos_frame->type = 2;
	avos_frame->interlaced = 0;
	avos_frame->top_field_first = 0;
}

/* BL copy worker: drains the posted job ring, runs the expensive copy
 * (getOutputBuffer + 12MB memcpy + release - serialized ~21ms on the
 * decode thread, the emit-40/s wall), and pushes the finished AVFrame
 * onto the ready ring. The decode thread collects ready frames before
 * its EL feed each iteration. Flush parks BOTH threads (dvhw_flush
 * waits th_busy==0 AND copyq.busy==0), so the worker never touches a
 * rebuilding pipeline. */
static void *dvhw_copy_worker_n(void *ctx)
{
	/* per-worker context: which busy[] flag is ours (dvhw_open creates
	 * one thread per WORKER_CTX instance) */
	struct worker_ctx {
		void *dec;
		int slot;
	};
	struct worker_ctx *wc = (struct worker_ctx *) ctx;
	STREAM_DEC_VIDEO *dec = (STREAM_DEC_VIDEO *) wc->dec;
	int slot = wc->slot;
	afree(wc);
	PRIV *p = (PRIV *) dec->priv;
	dvhw_copyq_t *q = &p->copyq;

	pthread_mutex_lock(&q->mtx);
	while (q->run) {
		while (q->run && q->count == 0)
			pthread_cond_wait(&q->cond, &q->mtx);
		if (!q->run)
			break;

		q->busy[slot] = 1;
		dvhw_copyjob_t job = q->job[q->head];
		q->busy_pts_us[slot] = job.pts_us;
		q->head = (q->head + 1) % DVHW_COPY_Q_MAX;
		q->count--;
		int release_only = q->release_only;
		pthread_mutex_unlock(&q->mtx);

			if (release_only) {
			/* flush protocol: the posted buffer belongs to the
			 * pre-seek stream - release it, produce nothing */
			AMediaCodec_releaseOutputBuffer(p->codec,
				                        (size_t) job.oidx, false);
			av_free(job.rpu);
			pthread_mutex_lock(&q->mtx);
			q->busy[slot] = 0;
			q->busy_pts_us[slot] = INT64_MAX;
			pthread_cond_broadcast(&q->cond);
			continue;
		}
		/* NOTE: busy_pts_us[slot] STAYS = job.pts_us for the whole
		 * copy + RPU parse: the emission-order gate must see the pts
		 * of the frame this worker is holding while it works. Clearing
		 * it here made the worker report idle exactly while carrying
		 * the older frame - the gate went blind and the reorder
		 * inversions continued (measured diag8: gate_hist entries
		 * 'X->MAX' at every violation). It is cleared only when the
		 * frame reaches the ready ring (or is dropped) below. */

		AVFrame *bl = dvhw_copy_yuv420(p, p->codec, job.oidx, &job.info,
		                               job.width, job.height,
		                               job.stride, job.slice_height,
		                               job.color_format, 0);
		/* the RPU moved with the job: attach its DV side data NOW (worker
		 * thread) - the ~4-9ms dovi_rpu_parse_to_avmetadata measured on the
		 * decode thread's collector was the second-largest phase after the
		 * copy itself. bl->pts already carries the pairing key. */
		if (bl && job.rpu && job.rpu_size > 0) {
			AVDOVIMetadata *meta =
				dovi_rpu_parse_to_avmetadata(job.rpu, (size_t) job.rpu_size);
			if (meta) {
				AVBufferRef *mbuf = av_buffer_create((uint8_t*) meta,
					sizeof(AVDOVIMetadata),
					dvhw_meta_buf_free, meta, 0);
				if (mbuf) {
					AVFrameSideData *sd =
						av_frame_new_side_data_from_buf(
							bl, AV_FRAME_DATA_DOVI_METADATA, mbuf);
						if (!sd)
							av_buffer_unref(&mbuf);
				} else {
					av_free(meta);
				}
			}
			{
				AVFrameSideData *sd =
					av_frame_new_side_data(bl,
						AV_FRAME_DATA_DOVI_RPU_BUFFER, job.rpu_size);
				if (sd)
					memcpy(sd->data, job.rpu, job.rpu_size);
			}
		}
		av_free(job.rpu);

		pthread_mutex_lock(&q->mtx);
		q->busy[slot] = 0;
		q->busy_pts_us[slot] = INT64_MAX;
		if (!q->run) {
			/* close raced us: drop the frame (close frees everything) */
			av_frame_free(&bl);
			pthread_cond_broadcast(&q->cond);
			continue;
		}
		if (bl && q->ready_count < DVHW_COPY_Q_MAX) {
			int rslot = (q->ready_head + q->ready_count) % DVHW_COPY_Q_MAX;
			q->ready[rslot] = bl;
			q->ready_user_id[rslot] = job.user_id;
			q->ready_count++;
			/* frame reached the ready ring: NOW the worker is idle for
			 * gate purposes (the ready ring itself is covered by the
			 * oldest-inflight scan) */
			q->busy_pts_us[slot] = INT64_MAX;
		} else {
			av_frame_free(&bl);	/* ready ring full: drop (defense;
						 * the collector keeps it shallow) */
			p->stat_bl_readydrop++;
			/* dropped: worker idle again for gate purposes */
			q->busy_pts_us[slot] = INT64_MAX;
		}
		pthread_cond_broadcast(&q->cond);
	}
	pthread_mutex_unlock(&q->mtx);
	return NULL;
}

/* drop the pending-RPU map entry for a pts whose frame was discarded
 * (ring-saturated inline release) - or its RPU memory leaks */
static void bl_drop_rpu(PRIV *p, int64_t pts_us)
{
	uint8_t *rpu = NULL;
	int rpu_size = 0, uid = 0;
	dvhw_pending_take(p, pts_us, &rpu, &rpu_size, &uid);
	av_free(rpu);
}

/* finish a worker-copied BL frame: colors + park. The RPU/side-data
 * work ran on the copy worker (in the job); this is the decode-thread
 * park step only. user_id rode the ready-frame via collect_user_id. */
static void dvhw_bl_finish(PRIV *p, AVFrame *bl)
{
	p->stat_bl_out++;
	if (!bl)
		return;
	bl->color_primaries = AVCOL_PRI_BT2020;
	bl->color_trc = AVCOL_TRC_SMPTE2084;
	bl->colorspace = AVCOL_SPC_BT2020_NCL;
	bl->color_range = AVCOL_RANGE_MPEG;
	if (p->bl_q_count >= DVHW_BL_Q_ARRAY) {
		/* overflow defense at the ARRAY bound (CREDIT_MAX=32 feeds can
		 * legitimately park >10; bl_q[] has 12 slots - never write past
		 * it). Drop the INCOMING frame, never the head. */
		av_frame_free(&bl);
		p->stat_park_drop++;
		return;
	}
	/* pts-sorted insert: TWO copy workers finish jobs out of completion
	 * order, but bl_q's FIFO pairing invariant (dvhw_fel_emit pairs EXACT
	 * pts against the strictly-ascending EL queue) requires ascending
	 * order. Walk from the tail while our pts is smaller and shift; the
	 * ring is shallow (park ≤ 6 in steady state) so this is O(few). */
	int at = p->bl_q_count;
	while (at > 0 && p->bl_q[at - 1]->pts > bl->pts) {
		p->bl_q[at] = p->bl_q[at - 1];
		p->bl_user_id[at] = p->bl_user_id[at - 1];
		at--;
	}
	p->bl_q[at] = bl;
	p->bl_user_id[at] = p->collect_user_id;
	p->bl_q_count++;
}

/* BL FEED WORKER: the dedicated queueInputBuffer thread. Measured on
 * this device (c2.qti HEVC 4K, buffer mode): the input binder call is
 * decode-progress-throttled - with the codec's ~24-AU B-pyramid window
 * in flight, each queueInputBuffer blocks ~15ms waiting for the HAL
 * input pool; the output dequeue blocks ~10-20ms waiting for frames.
 * On ONE thread these waits SERIALIZE (25-30ms/AU = the 37-40/s
 * equilibrium); on separate threads they OVERLAP (both are waits on
 * the same decoder progress, so max() not sum()). This is the
 * mpv/ExoPlayer architecture: mpv's vd thread and the mediacodec
 * wrapper's async callbacks run concurrently; ExoPlayer's
 * MediaCodecAsyncBufferQueue is a dedicated feed thread. The worker
 * pops gated AUs from in_q (th.mtx guards the ring; gates: park room,
 * credits, free slot) and runs the input binder loop (dequeue input
 * with a real timeout, memcpy, queue, pending_push) until the gates
 * close or in_q empties, then waits on feed_cond - broadcast by the
 * decode thread's collect/emit (park freed, credits freed, slots
 * freed) and by dec_in (new AU). */
static void *dvhw_feed_worker(void *ctx)
{
	STREAM_DEC_VIDEO *dec = (STREAM_DEC_VIDEO *) ctx;
	PRIV *p = (PRIV *) dec->priv;

	/* LOCK ORDER: th.mtx is NEVER taken while holding feed_mtx. The
	 * gate check / AU pop / counter updates take th.mtx alone (feed_mtx
	 * released); feed_busy/feed_park/run live under feed_mtx alone. The
	 * first cut of this worker held feed_mtx across the th.mtx section
	 * while the decode thread's emit broadcast took feed_mtx under
	 * th.mtx - a classic AB-BA deadlock (both threads frozen, measured
	 * startup wedge at loop 75). */
	for (;;) {
		/* park for flush (dvhw_flush sets feed_park under feed_mtx) */
		pthread_mutex_lock(&p->feed_mtx);
		while (p->feed_run && p->feed_park) {
			p->feed_busy = 0;
			pthread_cond_broadcast(&p->feed_cond);
			pthread_cond_wait(&p->feed_cond, &p->feed_mtx);
		}
		int run = p->feed_run;
		if (run)
			p->feed_busy = 1;	/* about to hold an AU - flush must wait */
		pthread_mutex_unlock(&p->feed_mtx);
		if (!run)
			break;

		/* gates under th.mtx (in_q ring + park + credits); feed_mtx NOT held */
		pthread_mutex_lock(&p->th.mtx);
		int gated = (p->in_q_count == 0) ||
		            (p->bl_q_count + (p->bl_fed_count - p->bl_out_count) >=
		                 DVHW_BL_CREDIT_MAX) ||
		            (frame_q_count(&p->slot_q) == 0) ||
		            p->flushing || !p->run;
			dvhw_au_t au = { NULL, 0, NULL, 0, 0, 0 };
		if (!gated) {
			au = p->in_q[p->in_q_read];
			p->in_q_read = (p->in_q_read + 1) % DVHW_IN_Q_MAX;
			p->in_q_count--;
		}
		pthread_mutex_unlock(&p->th.mtx);

		if (gated) {
			/* wait for a gate-open broadcast (dec_in / collect / emit), but
			 * TIME-BOXED: a 4ms timed wait so transient gate states
			 * (codec input full, momentary park) retry at 250Hz even with
			 * no broadcast - measured: the indefinite cond_wait slept a
			 * full frame period per input-full retry and cut feed to ~27/s
			 * (blq_full 5-7/s, emit 23-30/s at 48fps content). */
			struct timespec tw;
			clock_gettime(CLOCK_REALTIME, &tw);
			tw.tv_nsec += 4 * 1000000L;
			if (tw.tv_nsec >= 1000000000L) {
				tw.tv_sec++;
				tw.tv_nsec -= 1000000000L;
			}
			pthread_mutex_lock(&p->feed_mtx);
			p->feed_busy = 0;
			pthread_cond_broadcast(&p->feed_cond);
			pthread_cond_timedwait(&p->feed_cond, &p->feed_mtx, &tw);
			pthread_mutex_unlock(&p->feed_mtx);
			continue;
		}

		ssize_t idx = AMediaCodec_dequeueInputBuffer(p->codec, 8000);
		if (idx < 0) {
			/* codec input full: requeue at the in_q head and wait for the
			 * next gate-open signal (the codec is decoding; retry later) */
			pthread_mutex_lock(&p->th.mtx);
			p->in_q_read = (p->in_q_read - 1 + DVHW_IN_Q_MAX) % DVHW_IN_Q_MAX;
			p->in_q[p->in_q_read] = au;
			p->in_q_count++;
			p->stat_bl_qfull++;
			pthread_mutex_unlock(&p->th.mtx);
			continue;	/* loop immediately - the 8ms timeout paced us */
		}
		size_t buf_size = 0;
		uint8_t *buf = AMediaCodec_getInputBuffer(p->codec,
				(size_t) idx, &buf_size);
		if (buf && au.size <= (int) buf_size) {
			memcpy(buf, au.data, (size_t) au.size);
			if (AMediaCodec_queueInputBuffer(p->codec,
					(size_t) idx, 0, (size_t) au.size,
					(int64_t) au.pts_us, 0) == 0) {
				dvhw_pending_push(p, au.pts_us, au.rpu, au.rpu_size,
					   au.user_id);
				au.rpu = NULL;	/* ownership moved */
				pthread_mutex_lock(&p->th.mtx);
				p->bl_fed_count++;
				if (p->stat_fed_last_us) {
					int64_t fd = au.pts_us - p->stat_fed_last_us;
					if (fd < p->stat_fed_dmin_us)
						p->stat_fed_dmin_us = fd;
					if (fd > p->stat_fed_dmax_us)
						p->stat_fed_dmax_us = fd;
				}
				p->stat_fed_last_us = au.pts_us;
				pthread_mutex_unlock(&p->th.mtx);
			}
		} else {
			/* buffer too small (should not happen): drop the AU */
			p->stat_bl_qfull++;
		}
		av_free(au.rpu);
		av_free(au.data);
		/* not holding an AU anymore: flush may proceed (the busy flag
		 * re-checks at loop top anyway) */
		pthread_mutex_lock(&p->feed_mtx);
		p->feed_busy = 0;
		pthread_mutex_unlock(&p->feed_mtx);
		/* loop immediately: the gates re-check next iteration */
	}
	pthread_mutex_lock(&p->feed_mtx);
	p->feed_busy = 0;
	pthread_cond_broadcast(&p->feed_cond);
	pthread_mutex_unlock(&p->feed_mtx);
	return NULL;
}

static void *dvhw_async_thread(void *ctx)
{
	STREAM_DEC_VIDEO *dec = (STREAM_DEC_VIDEO *) ctx;
	PRIV *p = (PRIV *) dec->priv;

	pthread_mutex_lock(&p->th.mtx);
	while (p->run) {
		/* flush: drop the backlog, codec flush happens in dvhw_flush
		 * under the same lock - park here until it completes. Clear
		 * th_busy FIRST and broadcast: dvhw_flush waits for th_busy==0,
		 * so parking without clearing it deadlocks (flush waits us, we
		 * wait flush). */
		while (p->run && p->flushing) {
			p->th_busy = 0;
			pthread_cond_broadcast(&p->th.cond);
			pthread_cond_wait(&p->th.cond, &p->th.mtx);
		}
		if (!p->run)
			break;

		/* th_busy is held for the WHOLE iteration body: the EL feed and
		 * the BL output poll mutate shared state (el_q, bl_q, pending,
		 * counters) outside any other lock, and dvhw_flush rebuilds that
		 * state - it must find a quiescent thread. Park/idle paths below
		 * clear it before their waits, so flush's th_busy==0 wait always
		 * terminates within one iteration. */
		p->th_busy = 1;

		/* BL input feed runs on the DEDICATED FEED WORKER (dvhw_feed_worker):
		 * the ~15ms decode-progress-throttled queueInputBuffer binder wait
		 * overlaps the output dequeue's wait on a separate thread (measured
		 * serialized sum 25-30ms/AU = 37/s equilibrium vs overlapped max()).
		 * The decode thread only broadcasts gate-open events here (park
		 * freed below by collect, credits freed by emit, slots by the sink
		 * recycle) - the feed worker re-checks its gates each wake. */
		struct timespec ph0, ph1;
		clock_gettime(CLOCK_MONOTONIC, &ph0);

		pthread_mutex_unlock(&p->th.mtx);

		/* collect finished worker copies: RPU pairing + park (the worker
		 * only copies; pending_take/bl_q are decode-thread state). */
		for (;;) {
			pthread_mutex_lock(&p->copyq.mtx);
			AVFrame *bl = NULL;
			if (p->copyq.ready_count > 0) {
				bl = p->copyq.ready[p->copyq.ready_head];
				p->collect_user_id = p->copyq.ready_user_id[p->copyq.ready_head];
				p->copyq.ready_head = (p->copyq.ready_head + 1) % DVHW_COPY_Q_MAX;
				p->copyq.ready_count--;
			}
			pthread_mutex_unlock(&p->copyq.mtx);
			if (!bl)
				break;
			dvhw_bl_finish(p, bl);
		}
		/* park freed by collects: wake the feed worker to re-check gates */
		pthread_mutex_lock(&p->feed_mtx);
		pthread_cond_broadcast(&p->feed_cond);
		pthread_mutex_unlock(&p->feed_mtx);

		clock_gettime(CLOCK_MONOTONIC, &ph1);
		p->stat_phase_ns[0] += (int64_t)(ph1.tv_sec - ph0.tv_sec) * 1000000000L + (ph1.tv_nsec - ph0.tv_nsec);
		clock_gettime(CLOCK_MONOTONIC, &ph0);

		/* EL feed + drain run EVERY iteration, not only with AUs: the EL
		 * decoder needs continuous output drain (buffer-mode wedges when
		 * its output buffers are held), and at EOF (in_q empty,
		 * video_parse_end) the EL catch-up loop must keep pulling the
		 * remaining EL packets so the tail BLs can pair. The old sync
		 * path ran this once per AU; the thread's poll cadence replaces
		 * that. dvhw_el_feed's parser pull is internally gated against
		 * el_fed vs bl_fed, so idle calls are cheap. */
		if (p->fel)
			dvhw_el_feed(dec);

		clock_gettime(CLOCK_MONOTONIC, &ph1);
		p->stat_phase_ns[1] += (int64_t)(ph1.tv_sec - ph0.tv_sec) * 1000000000L + (ph1.tv_nsec - ph0.tv_nsec);
		clock_gettime(CLOCK_MONOTONIC, &ph0);

		/* --- output path: FFmpeg ff_mediacodec_dec_receive shape.
		 * The FIRST dequeue after feeding uses a blocking timeout
		 * (OUTPUT_DEQUEUE_TIMEOUT_US=8000 in FFmpeg) - that IS the loop's
		 * idle sleep: the binder call parks us until a frame is ready.
		 * A 0-timeout poll + nanosleep spin measured 3000-6000
		 * binder/sec and collapsed the 4K BL codec's out/fed ratio
		 * to ~50-60% (burst glitches); blocking dequeue is what every
		 * known-good client (FFmpeg/mpv, ExoPlayer) does. */
		/* ADAPTIVE output timeout: FFmpeg's blocking model (8ms) assumes the
	 * receive call is the thread's ONLY job. Ours is not: the same thread
	 * runs EL feed/pair, ready-frame collect, and emit - with the 4K BL
	 * codec producing at ~40/s, a fixed 8ms block happened ~40×/s and
	 * stole 320-960ms/s from the other phases (measured phase timers
	 * summing to 1.1-1.4s/s: the shared-resource equilibrium - put, emit
	 * and EL output all pinned to the same ~40/s). Block the full 8ms ONLY
	 * when idle (nothing queued, nothing to collect/emit); otherwise poll
	 * with a short timeout so the pending work runs THIS iteration. */
		int64_t out_timeout_us;
		pthread_mutex_lock(&p->copyq.mtx);
		int ready = p->copyq.ready_count;
		pthread_mutex_unlock(&p->copyq.mtx);
		int work_pending = (p->in_q_count > 0) || (ready > 0) ||
		                  (p->bl_q_count > 0) || (p->el_q_count > 0) ||
		                  (frame_q_count(&p->slot_q) > 0 && p->bl_q_count > 0);
		out_timeout_us = work_pending ? 1000 : 8000;
		for (;;) {
			AMediaCodecBufferInfo info;
			ssize_t oidx = AMediaCodec_dequeueOutputBuffer(p->codec,
					&info, out_timeout_us);
			out_timeout_us = 0;	/* subsequent iterations: drain without wait */
			if (oidx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
				AMediaFormat *ofmt = AMediaCodec_getOutputFormat(p->codec);
				int32_t v = 0;
				if (ofmt) {
					if (AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_WIDTH, &v) && v > 0)
						p->width = v;
					if (AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_HEIGHT, &v) && v > 0)
						p->height = v;
					if (AMediaFormat_getInt32(ofmt, "stride", &v) && v > 0)
						p->stride = v;
					if (AMediaFormat_getInt32(ofmt, "slice-height", &v) && v > 0)
						p->slice_height = v;
					if (AMediaFormat_getInt32(ofmt, "color-format", &v))
						p->color_format = v;
					AMediaFormat_delete(ofmt);
				}
				continue;
			}
			if (oidx < 0)
				break;
			if (info.size <= 0) {
				/* Codec2 byte-buffer mode emits zero-size output buffers as
				 * pipeline events; the sync decode2 released them (its
				 * else-branch). Skipping the release wedges one codec output
				 * slot per event - measured: both codecs emitted only ~50%
				 * of fed frames (bl_out 22/s at fed 44/s, el_out 51%),
				 * rate-independent, visible as burst glitches every 0.5-2s.
				 * Release + count. */
				AMediaCodec_releaseOutputBuffer(p->codec, (size_t) oidx, false);
				p->stat_bl_zerolen++;
				continue;
			}
			struct timespec pt0, pt1;
			clock_gettime(CLOCK_MONOTONIC, &pt0);
			/* post the copy job to the worker (async copy: the ~21ms
			 * getOutputBuffer+memcpy+release overlaps the NEXT dequeue
			 * instead of serializing - the emit-40/s wall at 48fps).
			 * Credit accounting is at POST time (bl_out_count++ here):
			 * the AU left the codec, whether the copy lands now, in the
			 * worker, or never (flush). The RPU is taken NOW too - the
			 * pending map is keyed by pts, and the worker's frame must
			 * not race the map on another pts.
			 *
			 * ZERO-LOSS BACKPRESSURE (mpv/ExoPlayer VO-pool semantics):
			 * if the ring is saturated, do NOT destroy the frame - wait
			 * for a worker to finish a job (bounded timed wait), then
			 * post. The frame stays owned by the codec until we take it;
			 * a full ring means the codec output pool is full too, which
			 * back-pressures the decoder naturally. The old
			 * dequeue-then-drop (postdrop 6-17/s measured) punched silent
			 * holes in the pts stream: pts_dmax 63-104ms vs the dense
			 * 21ms cadence, the sink then delivered frames whose
			 * deadlines had drifted past, late/flate climbing - the
			 * ~7-20fps equilibrium. */
			pthread_mutex_lock(&p->copyq.mtx);
			p->stat_bl_deq++;
			/* room is guaranteed by the pre-dequeue gate (in-flight < 4);
			 * the ring bound check below is pure defense (single poster
			 * thread: gate and post cannot interleave). */
			if (p->copyq.count < DVHW_COPY_Q_MAX) {
				dvhw_copyjob_t *job =
					&p->copyq.job[(p->copyq.head + p->copyq.count) % DVHW_COPY_Q_MAX];
				job->oidx = oidx;
				job->info = info;
				job->width = p->width;
				job->height = p->height;
				job->stride = p->stride;
				job->slice_height = p->slice_height;
				job->color_format = p->color_format;
				job->rpu = NULL;
				job->rpu_size = 0;
				job->user_id = 0;
				job->pts_us = info.presentationTimeUs;
				dvhw_pending_take(p, info.presentationTimeUs,
					  &job->rpu, &job->rpu_size,
					  &job->user_id);
				p->bl_out_count++;
				/* DEQUEUE-ORDER RING (diagnostic): the last 8 dequeued pts -
				 * dumped when the emitter sees a negative pts step, this
				 * proves/disproves decode-order (vs display-order) output
				 * from the codec directly. Zero cost otherwise. */
				p->dq_ring[p->dq_ring_w] = info.presentationTimeUs;
				p->dq_ring_w = (p->dq_ring_w + 1) & 7;
				p->copyq.count++;
				pthread_cond_signal(&p->copyq.cond);
				pthread_mutex_unlock(&p->copyq.mtx);
			} else {
				/* unreachable in practice (2 workers drain >50ms of jobs),
				 * but the dequeued index MUST be released or the codec output
				 * pool drains and the codec wedges - last-resort inline release */
				pthread_mutex_unlock(&p->copyq.mtx);
				AMediaCodec_releaseOutputBuffer(p->codec, (size_t) oidx, false);
				p->bl_out_count++;
				p->stat_bl_postdrop++;
				bl_drop_rpu(p, info.presentationTimeUs);
			}
			clock_gettime(CLOCK_MONOTONIC, &pt1);
			p->stat_copy_ns += (int64_t)(pt1.tv_sec - pt0.tv_sec) * 1000000000L + (pt1.tv_nsec - pt0.tv_nsec);
			continue;
		}	/* end output poll loop */

		clock_gettime(CLOCK_MONOTONIC, &ph1);
		p->stat_phase_ns[2] += (int64_t)(ph1.tv_sec - ph0.tv_sec) * 1000000000L + (ph1.tv_nsec - ph0.tv_nsec);
		clock_gettime(CLOCK_MONOTONIC, &ph0);

		pthread_mutex_lock(&p->th.mtx);
		/* emit all parked BLs whose EL partners are ready. Slot check
		 * BEFORE the pop: a pair pulled out of dvhw_fel_emit without a
		 * slot to land in would have to re-enter the reorder queues
		 * (whose FIFO pairing invariant does not survive reinsertion). */
		for (;;) {
			if (frame_q_count(&p->slot_q) == 0)
				break;	/* pool backpressure: decode stops here */
			/* EMISSION-ORDER GATE: NEVER emit a parked BL while an older
			 * frame is still inside the copy pipeline (posted / in a
			 * worker / on the ready ring). The 2-worker copy ring
			 * completes out of order (measured on every build incl. the
			 * 2154 baseline: pts_dmin -41..-209 ms, i.e. emitted frames
			 * stepping 1-5 frames BACKWARD, ~33% of seconds, from the
			 * first frames of playback - the visible judder: one frame
			 * jumps back ~3, then snaps forward). The pts-sorted park in
			 * dvhw_bl_finish orders what is PARKED, but the head can
			 * pair + emit while its predecessor is still copying.
			 * Holding emission here (the missing frame parks on the
		 * NEXT collect, ~20-35ms - a single copy wall, bounded by the
		 * ring depth, no queue growth: the park gate back-pressures
		 * the feed) restores strict pts-ascending emission. It also
		 * fixes the EL pairing that the reorder race broke: with the
		 * head temporarily "newest", an older EL arriving hit the
		 * stale-EL drop in dvhw_fel_emit and was destroyed - its BL
		 * then emitted BL-only. Holding the head keeps the stale-drop
		 * from running against the wrong head. */
			if (p->bl_q_count > 0 && p->copyq_started &&
			    !p->flushing) {
				int64_t oldest_us =
					dvhw_copyq_oldest_inflight_us(&p->copyq);
				if (oldest_us != INT64_MAX &&
				    oldest_us < p->bl_q[0]->pts * 1000)
					break;
			}
			AVFrame *el_emit = NULL;
			AVFrame *bl_emit = dvhw_fel_emit(p, &el_emit);
			if (!bl_emit)
				break;
			VIDEO_FRAME *slot = frame_q_get(&p->slot_q);
			p->slot_count--;
			dvhw_fill_slot(p, slot, bl_emit, el_emit, p->emit_user_id);
			frame_q_put(&p->out_q, slot);
		}
		if (p->stat_emit) {
			/* credits/park freed by emits: wake the feed worker */
			pthread_mutex_lock(&p->feed_mtx);
			pthread_cond_broadcast(&p->feed_cond);
			pthread_mutex_unlock(&p->feed_mtx);
		}
		clock_gettime(CLOCK_MONOTONIC, &ph1);
		p->stat_phase_ns[3] += (int64_t)(ph1.tv_sec - ph0.tv_sec) * 1000000000L + (ph1.tv_nsec - ph0.tv_nsec);

		/* 1Hz pipeline stats (was decode2's tail block): the thread's
		 * AU + emit rate vs the codec feed/drain counters - the single
		 * line that catches any pacing collapse live. Timer + loop
		 * counter are PER-INSTANCE (review finding 13: the function
		 * statics they replace were shared across live codec
		 * instances, interleaving/duplicating their prints). */
		{
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			int64_t now_us = (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
			p->stat_log_loops++;
			if (!p->stat_log_last_us)
				p->stat_log_last_us = now_us;
			else if (now_us - p->stat_log_last_us >= 1000000) {
				serprintf(TAG ": fps pipe: loops=%d inq=%d slots=%d outq=%d bl_fed=%d bl_out=%d bl_z=%d el_z=%d blq_full=%d park=%d park_drop=%d el_fed=%d el_out=%d emit=%d exh=%d fed_dmin=%d fed_dmax=%d pts_dmin=%d pts_dmax=%d ph_us fed/el/out/emit=%d/%d/%d/%d copy/rpu=%d/%d getin/qin=%d/%d deq=%d postdrop=%d rdydrop=%d decdrop=%d\n",
					  p->stat_log_loops,
					  p->in_q_count, frame_q_count(&p->slot_q),
					  frame_q_count(&p->out_q),
					  p->bl_fed_count, p->stat_bl_out,
					  p->stat_bl_zerolen, p->stat_el_zerolen,
					  p->stat_bl_qfull, p->bl_q_count, p->stat_park_drop,
					  p->el_fed_count, p->el_drain_count,
					  p->stat_emit, p->el_exhausted,
					  (int)(p->stat_fed_dmin_us / 1000),
					  (int)(p->stat_fed_dmax_us / 1000),
					  p->stat_pts_dmin, p->stat_pts_dmax,
					  (int)(p->stat_phase_ns[0] / 1000),
					  (int)(p->stat_phase_ns[1] / 1000),
					  (int)(p->stat_phase_ns[2] / 1000),
					  (int)(p->stat_phase_ns[3] / 1000),
				  (int)(p->stat_copy_ns / 1000),
				  (int)(p->stat_rpu_ns / 1000),
				  (int)(p->stat_getin_ns / 1000),
				  (int)(p->stat_qin_ns / 1000),
				  p->stat_bl_deq, p->stat_bl_postdrop, p->stat_bl_readydrop,
				  p->stat_dec_drop);
				p->stat_phase_ns[0] = p->stat_phase_ns[1] = 0;
				p->stat_phase_ns[2] = p->stat_phase_ns[3] = 0;
				p->stat_copy_ns = p->stat_rpu_ns = 0;
				p->stat_getin_ns = p->stat_qin_ns = 0;
				p->stat_log_loops = 0;
				p->stat_bl_out = p->stat_emit = 0;
				p->stat_bl_qfull = 0;
				p->stat_bl_zerolen = 0;
				p->stat_bl_deq = p->stat_bl_postdrop = p->stat_bl_readydrop = 0;
				p->stat_dec_drop = 0;
				p->stat_el_zerolen = 0;
				p->stat_park_drop = 0;
				p->stat_pts_last = -1;
				p->stat_pts_dmin = 1 << 30;
				p->stat_pts_dmax = 0;
				p->stat_fed_last_us = 0;
				p->stat_fed_dmin_us = (int64_t) 1 << 62;
				p->stat_fed_dmax_us = -((int64_t) 1 << 62);
				p->stat_log_last_us = now_us;
			}
		}
		/* pace: FFmpeg mediacodec_receive_frame parity - the BLOCKING
		 * output dequeue above is the idle sleep (binder parks the
		 * thread up to 8ms when nothing is ready). Only one residual
		 * case needs handling here: idle with NO in_q AU and NO out_q
		 * work: timed cond wait (5ms cap) so dec_in/put_out/flush
		 * signals wake us instantly. Working iterations loop immediately. */
		int idle = (p->in_q_count == 0) &&
		           (frame_q_count(&p->out_q) == 0);
		if (idle) {
			/* pthread_cond_timedwait needs an ABSOLUTE time (the old code
			 * passed {0, 5ms} - epoch+5ms, always in the past, immediate
			 * return: the intended 5ms signal-responsive idle sleep never
			 * ran). The cond uses the default CLOCK_REALTIME. */
			struct timespec tw;
			clock_gettime(CLOCK_REALTIME, &tw);
			tw.tv_nsec += 5 * 1000000L;
			if (tw.tv_nsec >= 1000000000L) {
				tw.tv_sec++;
				tw.tv_nsec -= 1000000000L;
			}
			p->th_busy = 0;
			pthread_cond_broadcast(&p->th.cond);
			pthread_cond_timedwait(&p->th.cond, &p->th.mtx, &tw);
		}
		(void) 0;
	}
	pthread_mutex_unlock(&p->th.mtx);
	return NULL;
}

/* AUs still queued but not yet consumed by the thread */
static void dvhw_in_q_clear(PRIV *p)
{
	while (p->in_q_count > 0) {
		av_free(p->in_q[p->in_q_read].data);
		av_free(p->in_q[p->in_q_read].rpu);
		p->in_q_read = (p->in_q_read + 1) % DVHW_IN_Q_MAX;
		p->in_q_count--;
	}
	p->in_q_read = p->in_q_write = 0;
	/* decoder-framedrop skip ring: post-seek pts belong to the OLD
	 * position - stale entries would discard fresh EL packets */
	p->el_skip.head = p->el_skip.count = 0;
}

/* Pairs/buffered BLs a flush discards; slots in out_q are engine pool
 * frames - their payloads must be released (render frees priv/handle[1]
 * when frames retire, but a flush discards them before the sink sees
 * them, so do the payload release here). */
static void dvhw_out_q_clear(PRIV *p)
{
	VIDEO_FRAME *f;
	while ((f = frame_q_get(&p->out_q))) {
		if (f->priv)
			av_frame_free((AVFrame**) &f->priv);
		if (f->handle[1])
			av_frame_free((AVFrame**) &f->handle[1]);
		f->valid = 0;
		/* hand the empty slot back for reuse - the engine re-pools it
		 * via its own frame bookkeeping when it collects out_q */
		frame_q_put(&p->slot_q, f);
	}
}

static int dvhw_flush(STREAM_DEC_VIDEO *dec)
{
	PRIV *p = (PRIV *) dec->priv;
	int i;

	if (!p->run) {
		/* sync-fallback path (thread not running): flush codecs directly */
		if (p->codec)
			AMediaCodec_flush(p->codec);
		if (p->el_codec)
			AMediaCodec_flush(p->el_codec);
		dvhw_el_q_clear(p);
		dvhw_bl_q_clear(p);
		dvhw_pending_clear(p);
		p->el_exhausted = 0;
		return 0;
	}

	/* Park BOTH threads: decode thread first (th_busy==0), then the
	 * copy worker (busy==0 + drain its queued jobs as release-only,
	 * because those buffers belong to the pre-seek stream), then the
	 * feed worker (feed_park -> busy==0: it must not queue an AU into
	 * the codec across the flush - the codec flush below would
	 * return it into a half-rebuilt pipeline). */
	pthread_mutex_lock(&p->th.mtx);
	p->flushing = 1;
	pthread_cond_broadcast(&p->th.cond);
	while (p->th_busy)
		pthread_cond_wait(&p->th.cond, &p->th.mtx);
	pthread_mutex_unlock(&p->th.mtx);

	pthread_mutex_lock(&p->feed_mtx);
	p->feed_park = 1;
	pthread_cond_broadcast(&p->feed_cond);
	while (p->feed_busy)
		pthread_cond_wait(&p->feed_cond, &p->feed_mtx);
	p->feed_park = 0;
	pthread_mutex_unlock(&p->feed_mtx);

	pthread_mutex_lock(&p->copyq.mtx);
	p->copyq.release_only = 1;
	pthread_cond_broadcast(&p->copyq.cond);
	{
		int all_idle;
		do {
			all_idle = 1;
			for (i = 0; i < DVHW_COPY_WORKERS; i++)
				if (p->copyq.busy[i] || p->copyq.count > 0)
					all_idle = 0;
			if (!all_idle)
				pthread_cond_wait(&p->copyq.cond, &p->copyq.mtx);
		} while (!all_idle);
	}
	/* discard ready frames (pre-seek stream) */
	{
		int i;
		for (i = 0; i < p->copyq.ready_count; i++)
			av_frame_free(&p->copyq.ready[(p->copyq.ready_head + i) % DVHW_COPY_Q_MAX]);
		p->copyq.ready_count = 0;
	}
	p->copyq.release_only = 0;
	pthread_mutex_unlock(&p->copyq.mtx);

	pthread_mutex_lock(&p->th.mtx);

	/* codec + reorder/pair state: same reset the sync path did */
	if (p->codec)
		AMediaCodec_flush(p->codec);
	if (p->el_codec)
		AMediaCodec_flush(p->el_codec);
	dvhw_el_q_clear(p);
	dvhw_bl_q_clear(p);
	dvhw_pending_clear(p);
	p->el_exhausted = 0;	/* seek: the parser refills the EL queue */

	/* queued work */
	dvhw_in_q_clear(p);
	dvhw_out_q_clear(p);

	p->flushing = 0;
	pthread_cond_broadcast(&p->th.cond);
	pthread_mutex_unlock(&p->th.mtx);
	/* epoch boundary: the emitted-pts delta tracker is epoch-local - the
	 * first post-seek emit is legally d=new_pos-old_pos (a backward seek
	 * is a huge NEGATIVE delta), which the EMIT-ORDER detector would
	 * falsely flag (measured: d=-6663 dump at a rewind, queue properly
	 * ordered). Reset like the 1Hz stat block does. */
	p->stat_pts_last = -1;
	p->stat_pts_dmin = 1 << 30;
	p->stat_pts_dmax = 0;
	/* resume the feed worker (gates re-check on wake) */
	pthread_mutex_lock(&p->feed_mtx);
	pthread_cond_broadcast(&p->feed_cond);
	pthread_mutex_unlock(&p->feed_mtx);
	return 0;
}

static int dvhw_render(STREAM_DEC_VIDEO *dec, VIDEO_FRAME *dst, VIDEO_FRAME *src)
{
	// OES path: rendering happens in the DV sink (dovi_gl). FEL path: the
	// BL/EL AVFrames were consumed by dovi_gl_render in sink_put; free them
	// here like codec_ffmpeg_video does (pool recycle point).
	if (src->priv && src->dec == dec) {
		av_frame_free((AVFrame **) &src->priv);
		if (src->handle[1])
			av_frame_free((AVFrame **) &src->handle[1]);
		src->dec = NULL;
	}
	return 0;
}

static int dvhw_get_rc(STREAM_DEC_VIDEO *dec, STREAM_RC *rc)
{
	if (!rc)
		return 1;
	memset(rc, 0, sizeof(STREAM_RC));
	/* Sink-owned frame pool size. Without this the rc stays zeroed and the
	 * dovi sink clamps to a 2-frame pool: 1 frame decoding + 1 rendering,
	 * zero slack — every engine hiccup lands as late-frame drops.
	 * 14 frames gives ~12 frames (~500ms at 24fps)
	 * of pipeline slack; the sink allocates the pool, we hold no buffers. */
	rc->num_frames = 64;
	rc->cpu_type = STREAM_CPU_ARM;
	rc->mem_type = STREAM_MEM_NRM;
	return 0;
}

static int dvhw_destroy(STREAM_DEC_VIDEO *dec)
{
	afree(dec->priv);
	afree(dec);
	return 0;
}

static STREAM_SINK_VIDEO *dvhw_get_sink(STREAM_DEC_VIDEO *dec)
{
	// same libplacebo sink as the software DV path
	if (dec && dec->video && dec->video->format == VIDEO_FORMAT_DOLBY_VISION &&
	    libavos_get_dolby_vision_mode() != 0) {
		STREAM *s = (STREAM *) dec->ctx;
		void *surface = s ? stream_get_surface_handle(s) : NULL;
		if (surface) {
			STREAM_SINK_VIDEO *sink = stream_sink_video_dovi_new(surface);
			if (sink) {
				serprintf(TAG ": Dolby Vision tone-map sink (libplacebo)\n");
				return sink;
			}
		}
	}
	return NULL;
}

static STREAM_DEC_VIDEO *_new(void)
{
	STREAM_DEC_VIDEO *dec = (STREAM_DEC_VIDEO *) amalloc(sizeof(STREAM_DEC_VIDEO));
	if (!dec)
		return NULL;
	memset(dec, 0, sizeof(STREAM_DEC_VIDEO));

	static char name[] = "mediacodec-dovi";
	dec->name = name;
	dec->destroy = dvhw_destroy;
	dec->open = dvhw_open;
	dec->close = dvhw_close;
	dec->prepare = dvhw_prepare;
	dec->cleanup = dvhw_cleanup;
	dec->decode = NULL;
	dec->decode2 = NULL;
	dec->dec_in = dvhw_dec_in;
	dec->put_out = dvhw_put_out;
	dec->get_out = dvhw_get_out;
	dec->async = 1;
	dec->flush = dvhw_flush;
	dec->render = dvhw_render;
	dec->get_rc = dvhw_get_rc;
	dec->get_sink = dvhw_get_sink;

	if (!(dec->priv = acalloc(1, sizeof(PRIV)))) {
		afree(dec);
		return NULL;
	}
	return dec;
}

#define MAXW VIDEO_MAX_WIDTH
#define MAXH VIDEO_MAX_HEIGHT

// Registered at SFDEC_OMXCODEC priority (DSP2): below the sfdec2 passthrough
// decoder (DSP3, which rejects DV in tone-map mode) and above the software
// ffmpeg decoder (GPP). FEL content fails open() and lands on the software
// path for full reshaping parity.
STREAM_REGISTER_DEC_VIDEO2( VIDEO_FORMAT_DOLBY_VISION, 0, MAXW, MAXH,
                            0, SFDEC_OMXCODEC, _new,
                            "mediacodec-dovi", NULL );

#endif /* CONFIG_ANDROID && CONFIG_DOVI_TONEMAP */

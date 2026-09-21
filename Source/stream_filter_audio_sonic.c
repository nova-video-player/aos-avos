/*
 * Copyright 2026 Courville Software
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

/*
 * Audio Speed Control Filter using the Sonic library
 *
 * Experimental alternate audio-speed backend, evaluated alongside the
 * existing FFmpeg atempo filter (stream_filter_audio_atempo.c). Sonic uses a
 * pitch-synchronous algorithm tuned for speech, as opposed to atempo's WSOLA
 * approach, and is the same algorithm family used by AudioTrack
 * PlaybackParams on most stock Android devices.
 *
 * Wired into the live playback pipeline as a selectable backend alongside
 * atempo (see stream_get_audio_speed_filter() in stream.h and
 * stream_set_audio_speed_backend() in stream_video.c). The generic
 * STREAM_FILTER_AUDIO vtable includes output/media timing and effective-speed
 * checkpoints. Both software filters feed the shared presentation ledger and
 * playhead-gated video commits (see doc/audio_speed_sonic_architecture.md).
 * _filter()
 * self-reads the current global speed each call (audio_interface_get_audio_speed()),
 * the same self-contained pattern atempo's atempo_filter_input() uses.
 *
 * Supported sample formats: S16 (bits=16) and FLT (bits=32), matching the
 * two formats Sonic's public API handles natively
 * (sonicWriteShortToStream / sonicWriteFloatToStream). Other bit depths are
 * rejected rather than silently mishandled.
 *
 * Speed range mirrors the atempo backend's configured range (0.5x-2.0x) for
 * comparable A/B testing, though Sonic itself supports a much wider range.
 */

#include "global.h"
#include "stream_filter_audio.h"
#include "debug.h"
#include "astdlib.h"
#include "util.h"
#include "sonic.h"
#include "audio_interface.h"
#include "pthread.h"

#define DBGS DBG_IF(Debug[DBG_AUD])
#define DBG  DBG_IF(Debug[DBG_AUD])

// Practical speed limits, matching the atempo backend for comparable testing.
#define SPEED_MIN 0.5f
#define SPEED_MAX 2.0f

struct ctx {
	sonicStream stream;

	int channels;
	int sample_rate;
	int bits;                    // 16 (short) or 32 (float) supported

	float current_speed;
	int   enabled;
	int   stream_initialized;

	uint8_t *output_buffer;
	int      output_buffer_size; // bytes

	// One returned buffer is consumed completely before the next filter/drain.
	// Keep its production-time media span for arbitrary partial sink writes.
	UINT64 output_frames;
	INT64 input_frames;
	INT64 media_frames;
	UINT64 map_start;
	int map_frames;
	INT64 map_media_frames;
	float published_speed;
	int published_valid;
	int commit_pending;
	float commit_speed;
	UINT64 commit_boundary;

	// Protects ctx->stream (and the fields describing it: channels/
	// sample_rate/bits/stream_initialized) against concurrent access.
	// The audio thread destroys/recreates ctx->stream from _filter() and
	// sonic_reconfigure(); the renderer/control thread can call _delay()
	// concurrently via stream_get_audio_speed_filter(s)->delay(). Mirrors
	// the atempo context mutex documented in
	// doc/audio_speed_atempo_architecture.md ("A/V Sync Delay Compensation").
	pthread_mutex_t lock;
};

static void ctx_free(struct ctx *ctx)
{
	if (!ctx) {
		return;
	}
	if (ctx->stream) {
		sonicDestroyStream(ctx->stream);
		ctx->stream = NULL;
	}
	if (ctx->output_buffer) {
		afree(ctx->output_buffer);
		ctx->output_buffer = NULL;
		ctx->output_buffer_size = 0;
	}
	pthread_mutex_destroy(&ctx->lock);
	afree(ctx);
}

static int sonic_fail(AUDIO_FRAME *frame)
{
	frame->data = NULL;
	frame->size = frame->fakeSize = 0;
	frame->error = 1;
	return -1;
}

static int _open(STREAM_FILTER_AUDIO *f, AUDIO_PROPERTIES *audio)
{
	DBGS serprintf("sonic: open - channels=%d sampleRate=%d bitsPerSample=%d\n",
		audio->channels, audio->samplesPerSec, audio->bitsPerSample);

	if (audio->bitsPerSample != 16 && audio->bitsPerSample != 32) {
		serprintf("sonic: unsupported bit depth %d (only 16/32 supported)\n",
			audio->bitsPerSample);
		return -1;
	}

	struct ctx *ctx = acalloc(1, sizeof(struct ctx));
	if (!ctx) {
		serprintf("sonic: failed to allocate context\n");
		return -1;
	}

	ctx->channels    = audio->channels;
	ctx->sample_rate = audio->samplesPerSec;
	ctx->bits        = audio->bitsPerSample;
	ctx->current_speed = 1.0f;
	ctx->enabled = 0;
	pthread_mutex_init(&ctx->lock, NULL);

	ctx->stream = sonicCreateStream(ctx->sample_rate, ctx->channels);
	if (!ctx->stream) {
		serprintf("sonic: failed to create sonic stream\n");
		pthread_mutex_destroy(&ctx->lock);
		afree(ctx);
		return -1;
	}
	sonicSetSpeed(ctx->stream, 1.0f);
	ctx->stream_initialized = 1;

	f->priv = ctx;
	DBGS serprintf("sonic: initialized for %d channels, %d Hz, %d bits\n",
		ctx->channels, ctx->sample_rate, ctx->bits);
	return 0;
}

static int sonic_reconfigure(struct ctx *ctx, int channels, int sample_rate, int bits)
{
	if (bits != 16 && bits != 32) {
		serprintf("sonic: unsupported reconfigure bit depth %d\n", bits);
		return -1;
	}
	DBGS serprintf("sonic: reconfigure %dch/%dHz/%dbit -> %dch/%dHz/%dbit\n",
		ctx->channels, ctx->sample_rate, ctx->bits, channels, sample_rate, bits);

	if (ctx->stream) {
		sonicDestroyStream(ctx->stream);
		ctx->stream = NULL;
	}
	ctx->channels    = channels;
	ctx->sample_rate = sample_rate;
	ctx->bits        = bits;
	ctx->input_frames = ctx->media_frames = 0;
	ctx->map_frames = 0;
	ctx->published_valid = 0;

	ctx->stream = sonicCreateStream(ctx->sample_rate, ctx->channels);
	if (!ctx->stream) {
		serprintf("sonic: failed to recreate sonic stream\n");
		ctx->stream_initialized = 0;
		return -1;
	}
	sonicSetSpeed(ctx->stream, ctx->current_speed);
	ctx->stream_initialized = 1;
	return 0;
}

static int sonic_ensure_output_capacity(struct ctx *ctx, int bytes)
{
	if (bytes <= ctx->output_buffer_size) {
		return 0;
	}
	uint8_t *buffer = arealloc(ctx->output_buffer, bytes);
	if (!buffer) {
		return -1;
	}
	ctx->output_buffer = buffer;
	ctx->output_buffer_size = bytes;
	return 0;
}

// Drain whatever Sonic currently has ready and hand it back as one AUDIO_FRAME.
static int sonic_read_available(struct ctx *ctx, AUDIO_FRAME *frame)
{
	frame->size = 0;
	frame->fakeSize = 0;

	int available = sonicSamplesAvailable(ctx->stream);
	if (available <= 0) {
		return 0;
	}

	int bytes_per_sample = (ctx->bits / 8) * ctx->channels;
	int bytes = available * bytes_per_sample;
	if (sonic_ensure_output_capacity(ctx, bytes) < 0) {
		return -1;
	}

	int read;
	if (ctx->bits == 32) {
		read = sonicReadFloatFromStream(ctx->stream, (float *)ctx->output_buffer, available);
	} else {
		read = sonicReadShortFromStream(ctx->stream, (short *)ctx->output_buffer, available);
	}
	if (read <= 0) {
		return 0;
	}

	frame->data = ctx->output_buffer;
	frame->size = read * bytes_per_sample;
	frame->format = WAVE_FORMAT_PCM;
	frame->channels = ctx->channels;
	frame->samplesPerSec = ctx->sample_rate;
	frame->bits = ctx->bits;

	// Count media actually processed, excluding retained lookahead. In speed-only
	// mode (pitch=rate=1) Sonic has no subsequent resampling buffer. Interpolate
	// within this burst, as atempo does, rather than multiplying by the requested
	// speed: localSpeed and pitch-period edits can give a different actual span.
	INT64 media = ctx->input_frames - sonicInputSamplesBuffered(ctx->stream);
	ctx->map_start = ctx->output_frames;
	ctx->map_frames = read;
	ctx->map_media_frames = media - ctx->media_frames;
	ctx->media_frames = media;
	ctx->output_frames += read;

	float speed = sonicProcessingSpeed(ctx->stream);
	if (fabsf(speed - ctx->current_speed) < 0.001f)
		speed = ctx->current_speed;
	if (!ctx->published_valid || speed != ctx->published_speed) {
		// This boundary belongs to output, not to the setter or input admission.
		// Successive bursts describe Sonic's blended transition without flushing.
		ctx->commit_pending = 1;
		ctx->commit_speed = ctx->published_speed = speed;
		ctx->commit_boundary = ctx->map_start;
		ctx->published_valid = 1;
		DBG serprintf("sonic_commit_output: speed=%.6f requested=%.3f boundary=%llu media=%lld buffered=%d\n",
			speed, ctx->current_speed, (unsigned long long)ctx->map_start,
			(long long)media, sonicInputSamplesBuffered(ctx->stream));
	}
	return 0;
}

static int _filter(STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame)
{
	struct ctx *ctx = f->priv;
	if (!ctx || !frame) {
		return frame ? sonic_fail(frame) : -1;
	}

	if (frame->size <= 0) {
		return 0;
	}
	if (!frame->data) {
		return sonic_fail(frame);
	}

	pthread_mutex_lock(&ctx->lock);

	int frame_channels = frame->channels ? frame->channels : ctx->channels;
	int frame_rate      = frame->samplesPerSec ? frame->samplesPerSec : ctx->sample_rate;
	int frame_bits       = frame->bits ? frame->bits : ctx->bits;

	if (frame_bits != 16 && frame_bits != 32) {
		pthread_mutex_unlock(&ctx->lock);
		serprintf("sonic: unsupported frame bit depth %d\n", frame_bits);
		return sonic_fail(frame);
	}

	// Self-contained speed read, mirroring atempo's atempo_filter_input():
	// pull the current global speed on every call instead of relying on an
	// external setter, since nothing else drives stream_filter_audio_sonic_set_speed().
	float speed = audio_interface_is_audio_speed_enabled() ?
		audio_interface_get_audio_speed() : 1.0f;
	if (fabsf(ctx->current_speed - speed) >= 0.001f) {
		if (speed < SPEED_MIN) {
			speed = SPEED_MIN;
		} else if (speed > SPEED_MAX) {
			speed = SPEED_MAX;
		}
		if (ctx->stream) {
			sonicSetSpeed(ctx->stream, speed);
		}
		ctx->current_speed = speed;
		ctx->enabled = fabsf(speed - 1.0f) > 0.001f;
	}

	// Callers must have already drained any pending old-geometry output via
	// stream_filter_audio_sonic_needs_format_drain()/drain(f, frame, 1) before
	// handing us a frame with new geometry -- reconfiguring here would
	// otherwise silently discard whatever Sonic still had buffered for the
	// previous format (see stream_audio.c's format-drain gate).
	if (!ctx->stream_initialized || frame_channels != ctx->channels || frame_rate != ctx->sample_rate ||
	    frame_bits != ctx->bits) {
		if (sonic_reconfigure(ctx, frame_channels, frame_rate, frame_bits) < 0) {
			pthread_mutex_unlock(&ctx->lock);
			return sonic_fail(frame);
		}
	}

	int bytes_per_sample = (ctx->bits / 8) * ctx->channels;
	int input_samples = frame->size / bytes_per_sample;
	if (input_samples <= 0) {
		pthread_mutex_unlock(&ctx->lock);
		frame->size = 0;
		frame->fakeSize = 0;
		return 0;
	}

	int write_ret;
	if (ctx->bits == 32) {
		write_ret = sonicWriteFloatToStream(ctx->stream, (const float *)frame->data, input_samples);
	} else {
		write_ret = sonicWriteShortToStream(ctx->stream, (const short *)frame->data, input_samples);
	}
	if (!write_ret) {
		pthread_mutex_unlock(&ctx->lock);
		serprintf("sonic: write to stream failed (OOM)\n");
		return sonic_fail(frame);
	}
	ctx->input_frames += input_samples;

	if (sonic_read_available(ctx, frame) < 0) {
		pthread_mutex_unlock(&ctx->lock);
		return sonic_fail(frame);
	}

	DBG serprintf("sonic: filter speed=%.3f in=%d samples out=%d bytes\n",
		ctx->current_speed, input_samples, frame->size);
	pthread_mutex_unlock(&ctx->lock);
	return 0;
}

// True when ctx holds a different-geometry stream than frame and still has
// output pending from it. Callers (stream_audio.c) must drain(f, frame, 1)
// before feeding frame's data into filter(), or sonic_reconfigure() inside
// filter() will destroy the old-geometry stream -- and whatever audio it was
// still holding -- without ever handing that audio back.
int stream_filter_audio_sonic_needs_format_drain(STREAM_FILTER_AUDIO *f, const AUDIO_FRAME *frame)
{
	struct ctx *ctx = f ? f->priv : NULL;
	if (!ctx || !frame || frame->size <= 0) {
		return 0;
	}
	pthread_mutex_lock(&ctx->lock);
	int channels = frame->channels ? frame->channels : ctx->channels;
	int rate = frame->samplesPerSec ? frame->samplesPerSec : ctx->sample_rate;
	int bits = frame->bits ? frame->bits : ctx->bits;
	int needs = ctx->stream_initialized &&
		(channels != ctx->channels || rate != ctx->sample_rate || bits != ctx->bits);
	pthread_mutex_unlock(&ctx->lock);
	return needs;
}

static int _drain(STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame, int end)
{
	memset(frame, 0, sizeof(*frame));
	struct ctx *ctx = f->priv;
	if (!ctx) {
		return 0;
	}
	pthread_mutex_lock(&ctx->lock);
	if (!ctx->stream_initialized) {
		pthread_mutex_unlock(&ctx->lock);
		return 0;
	}
	if (end && !sonicFlushStream(ctx->stream)) {
		pthread_mutex_unlock(&ctx->lock);
		return sonic_fail(frame);
	}
	int rc = sonic_read_available(ctx, frame);
	if (end && rc == 0) {
		// Whatever Sonic was still holding (including pitch-period lookahead
		// forced out by the flush above) has now been handed back in frame.
		// Force filter()'s geometry check to run sonic_reconfigure() again
		// before accepting more input, even if the next frame happens to
		// match the current geometry, and so needs_format_drain() reports
		// false on a retry instead of re-draining an already-empty stream.
		ctx->stream_initialized = 0;
	}
	pthread_mutex_unlock(&ctx->lock);
	return rc;
}

static int _flush(STREAM_FILTER_AUDIO *f)
{
	struct ctx *ctx = f->priv;
	if (!ctx) {
		return 0;
	}
	pthread_mutex_lock(&ctx->lock);
	if (!ctx->stream) {
		pthread_mutex_unlock(&ctx->lock);
		return 0;
	}
	// Sonic has no explicit reset call; recreate the stream to drop buffered
	// state the same way the atempo backend discards its graph on seek/flush.
	sonicDestroyStream(ctx->stream);
	ctx->output_frames = 0;
	ctx->input_frames = ctx->media_frames = 0;
	ctx->map_frames = 0;
	ctx->published_valid = ctx->commit_pending = 0;
	ctx->stream = sonicCreateStream(ctx->sample_rate, ctx->channels);
	if (!ctx->stream) {
		ctx->stream_initialized = 0;
		pthread_mutex_unlock(&ctx->lock);
		return -1;
	}
	sonicSetSpeed(ctx->stream, ctx->current_speed);
	ctx->stream_initialized = 1;
	pthread_mutex_unlock(&ctx->lock);
	return 0;
}

static int _delay(STREAM_FILTER_AUDIO *f)
{
	struct ctx *ctx = f->priv;
	if (!ctx) {
		return 0;
	}
	// Diagnostic-only estimate of buffered output not yet handed back to the
	// caller. Mirrors the atempo backend's treatment of FIFO depth as
	// non-authoritative for sync (see stream_filter_audio_atempo.c _delay_locked).
	// Locked because the audio thread can destroy/recreate ctx->stream
	// (sonic_reconfigure()/_flush()) concurrently with this call, which is
	// typically made from the renderer/control thread via
	// stream_get_heard_audio_ts()/stream_sync.c.
	pthread_mutex_lock(&ctx->lock);
	if (!ctx->enabled || !ctx->stream || ctx->sample_rate <= 0) {
		pthread_mutex_unlock(&ctx->lock);
		return 0;
	}
	int available = sonicSamplesAvailable(ctx->stream);
	int rate = ctx->sample_rate;
	pthread_mutex_unlock(&ctx->lock);
	return (available * 1000) / rate;
}

static int _set_param(STREAM_FILTER_AUDIO *f, void *params, void *night_on)
{
	struct ctx *ctx = f->priv;
	if (!ctx) {
		return -1;
	}
	(void)params;
	(void)night_on;
	return 0;
}

static int _close(STREAM_FILTER_AUDIO *f)
{
	DBGS serprintf("sonic: close\n");
	struct ctx *ctx = f->priv;
	if (ctx) {
		pthread_mutex_lock(&ctx->lock);
		if (ctx->stream) {
			sonicDestroyStream(ctx->stream);
			ctx->stream = NULL;
		}
		ctx->stream_initialized = 0;
		pthread_mutex_unlock(&ctx->lock);
	}
	return 0;
}

static int _delete(STREAM_FILTER_AUDIO *f)
{
	DBGS serprintf("sonic: delete\n");
	if (f && f->priv) {
		ctx_free(f->priv);
		f->priv = NULL;
	}
	if (f) {
		afree(f);
	}
	return 0;
}

// Optional explicit setter. Playback normally supplies the global request in
// _filter(); either path publishes timing only when output is actually produced.
int stream_filter_audio_sonic_set_speed(STREAM_FILTER_AUDIO *f, float speed)
{
	struct ctx *ctx = f ? f->priv : NULL;
	if (!ctx) {
		return -1;
	}
	if (speed < SPEED_MIN) {
		speed = SPEED_MIN;
	} else if (speed > SPEED_MAX) {
		speed = SPEED_MAX;
	}
	pthread_mutex_lock(&ctx->lock);
	if (!ctx->stream || fabsf(ctx->current_speed - speed) < 0.001f) {
		int rc = ctx->stream ? 0 : -1;
		pthread_mutex_unlock(&ctx->lock);
		return rc;
	}
	sonicSetSpeed(ctx->stream, speed);
	ctx->current_speed = speed;
	ctx->enabled = fabsf(speed - 1.0f) > 0.001f;
	pthread_mutex_unlock(&ctx->lock);
	DBGS serprintf("sonic: speed set to %.3f\n", speed);
	return 0;
}

static int sonic_output_state(STREAM_FILTER_AUDIO *f, UINT64 *frames, int *queued, int *rate)
{
	struct ctx *ctx = f ? f->priv : NULL;
	if (!ctx)
		return 0;
	pthread_mutex_lock(&ctx->lock);
	if (frames) *frames = ctx->output_frames;
	if (queued) *queued = ctx->stream ? sonicSamplesAvailable(ctx->stream) : 0;
	if (rate) *rate = ctx->sample_rate;
	pthread_mutex_unlock(&ctx->lock);
	return 1;
}

static int sonic_take_commit(STREAM_FILTER_AUDIO *f, float *speed, UINT64 *boundary)
{
	struct ctx *ctx = f ? f->priv : NULL;
	if (!ctx)
		return 0;
	pthread_mutex_lock(&ctx->lock);
	int pending = ctx->commit_pending;
	if (pending) {
		*speed = ctx->commit_speed;
		*boundary = ctx->commit_boundary;
		ctx->commit_pending = 0;
	}
	pthread_mutex_unlock(&ctx->lock);
	return pending;
}

static int sonic_output_media(STREAM_FILTER_AUDIO *f, UINT64 start, int frames,
	INT64 *media_frames, int *rate)
{
	struct ctx *ctx = f ? f->priv : NULL;
	if (!ctx || frames <= 0)
		return 0;
	pthread_mutex_lock(&ctx->lock);
	int found = ctx->map_frames > 0 && start >= ctx->map_start &&
		start - ctx->map_start <= (UINT64)ctx->map_frames &&
		(UINT64)frames <= (UINT64)ctx->map_frames - (start - ctx->map_start);
	if (found) {
		INT64 lo = start - ctx->map_start;
		// Differences of endpoints keep split writes additive to the last frame.
		*media_frames = ctx->map_media_frames * (lo + frames) / ctx->map_frames -
			ctx->map_media_frames * lo / ctx->map_frames;
		*rate = ctx->sample_rate;
	}
	pthread_mutex_unlock(&ctx->lock);
	return found;
}

STREAM_FILTER_AUDIO *stream_filter_audio_sonic_new(void)
{
	STREAM_FILTER_AUDIO *f = acalloc(1, sizeof(STREAM_FILTER_AUDIO));
	if (!f) {
		serprintf("sonic: failed to allocate filter structure\n");
		return NULL;
	}

	static char name[] = "sonic";
	f->name      = name;
	f->delete    = _delete;
	f->open      = _open;
	f->close     = _close;
	f->filter    = _filter;
	f->flush     = _flush;
	f->set_param = _set_param;
	f->delay     = _delay;
	f->drain     = _drain;
	f->get_output_state = sonic_output_state;
	f->take_speed_commit = sonic_take_commit;
	f->lookup_output_media = sonic_output_media;

	DBGS serprintf("sonic: audio speed filter created\n");
	return f;
}

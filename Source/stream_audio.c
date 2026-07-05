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
#include "stream_sync.h"
#include "audio_spdif.h"
#include "audio_interface.h"
#include "ac3_recode.h"
#include "atime.h"
#include "debug.h"
#include "atime.h"
#include "util.h"
#include "file.h"

#include <string.h>
#include <math.h>

#ifdef CONFIG_ANDROID
#endif

#define DBGS DBG_IF(Debug[DBG_STREAM])
#define DBGA DBG_IF(Debug[DBG_AUD])
#define DBGV DBG_IF(Debug[DBG_VID])

#define DBG DBG_IF(Debug[DBG_STREAM])
#define DBG2 DBG_IF(Debug[DBG_STREAM] > 1)
#define DBG3 DBG_IF(Debug[DBG_STREAM] > 2)
#define ERR if( 1 )

#ifdef CONFIG_STREAM

AV_PROPERTIES *stream_force_audio_props = NULL;

void stream_audio_props_changed( STREAM *s, STREAM_CDATA *cdata );
void stream_audio_samplerate_changed( STREAM *s );

static int zero_time = 200;
static int stream_audio_chunk = 4096;
static int stream_audio_pcm_accum_ms = 40;
static int ac3_sink_configured = 0;  // Track if sink is configured for AC3 passthrough
static int ac3_reconfigure_pending = 1;  // Force initial reconfiguration when AC3 recoding starts
static int ac3_force_mode2 = 0;  // Debug A/B: force raw AC3 AudioTrack mode2 for recoding
// AC3-recode mode2 plain-policy gate. When the AC3-recode sink resolves to passthrough
// mode2 (raw AC3, e.g. an eARC route), adopt TWO elements of the ordinary-mode2 timing
// policy (NOT the whole policy — recode keeps its dedicated wall-clock pacer and stays
// exempt from the ordinary mode2 lead gate):
//   (1) PTS-seeded STREAM_SYNC_SAMPLES audio clock instead of the mode1 CDATA synthetic
//       startup anchor (handled in this file), and
//   (2) app_latency instead of pipeline_latency for the static heard delay
//       (handled in audiotrack_get_latency via stream_audio_ac3_mode2_plain_policy()).
// Both must apply together: with the synthetic anchor the static latency cancels, so
// fixing only one is ineffective (mode1 stays in sync; only resolved-mode2 is affected).
// Default on. This is a PER-PLAYBACK / startup policy: the samples-clock transition and
// the AudioTrack latency policy are both latched when the AC3 sink resolves to mode2.
// Toggling the flag mid-stream does not change the current playback; to A/B, change the
// flag and then start a fresh playback.
static int ac3_mode2_plain_policy = 1;
int stream_audio_ac3_mode2_plain_policy( void ) { return ac3_mode2_plain_policy; }
// Diagnostic A/B only: retain the mode2 samples clock but force pipeline_latency
// for every AC3-recode mode2 output layout. Latched when AudioTrack is configured;
// set before starting a fresh playback.
static int ac3_mode2_force_pipeline = 1;
int stream_audio_ac3_mode2_force_pipeline( void ) { return ac3_mode2_force_pipeline; }
static int audio_format_configured = -1;  // Track audio format to avoid redundant passthrough reconfigurations
static int startup_anchor_log_count = 0;  // Cap startup anchor diagnostics per playback
static int startup_write_log_count = 0;   // Cap first-write diagnostics per playback
static int atempo_gate_log_count = 0;     // Cap atempo runtime gating diagnostics per playback
static int resume_write_log_count = 0;    // Cap post-resume write diagnostics; reset on each resume
static int resume_seen_pending = 0;       // Tracks audio_resume_pending transition to fire reset exactly once per resume
extern int stream_audio_paused;
extern int libavos_get_ac3_recoding_enabled(void);
extern int libavos_get_max_pcm_channels(void);

#define AC3_RECODE_FRAME_US ((int64_t)AC3_RECODE_FRAME_SAMPLES * 1000000 / AC3_RECODE_SAMPLE_RATE)
#define AC3_RECODE_WRITE_AHEAD_BURSTS 3

static int pcm_channel_cap = 0;

#ifdef CONFIG_SPDIF
static int stream_audio_select_ac3_passthrough_mode(const char *tag)
{
	extern int get_hdmi_supports_iec(void);
	int supports_iec = get_hdmi_supports_iec();
	int passthrough_mode = (ac3_force_mode2 || !supports_iec) ? 2 : 1;
	// Always restore the selected framing mode. A prior forced-mode2 run leaves
	// the global SPDIF mode at 2, so merely clearing the debug flag is insufficient.
	spdif_set_passthrough( passthrough_mode );
	serprintf("%s: AC3 recode mode=%d iec=%d forced_mode2=%d\n",
		tag, passthrough_mode, supports_iec, ac3_force_mode2);
	return passthrough_mode;
}
#endif

static int _stream_audio_speed_diag_active( STREAM *s )
{
	return s && s->audio_speed_diag_writes_left > 0;
}

static void _stream_atempo_ledger_reset(STREAM *s)
{
	if (!s) {
		return;
	}
	s->atempo_ledger_active = 0;
	s->atempo_ledger_count = 0;
	s->atempo_ledger_write = 0;
	s->atempo_ledger_output_frames = 0;
	s->atempo_ledger_base_written_frames = 0;
	s->atempo_ledger_next_ts_us = 0;
	s->atempo_ledger_last_log_ms = 0;
	s->atempo_ledger_dense_until_ms = 0;
	s->atempo_ledger_next_rst_us = 0;
	s->atempo_ledger_media_cursor = 0;
	s->atempo_ledger_media_valid = 0;
}

static int _stream_atempo_ledger_arm(STREAM *s, int sample_rate)
{
	if (!s || !s->audio_ctx || sample_rate <= 0 || s->audio_time < 0) {
		return 0;
	}
	UINT64 written_frames = 0;
	int written_rate = 0;
	if (!audio_interface_get_written_frames(s->audio_ctx, &written_frames, &written_rate) || written_rate <= 0) {
		return 0;
	}
	s->atempo_ledger_active = 1;
	s->atempo_ledger_count = 0;
	s->atempo_ledger_write = 0;
	s->atempo_ledger_base_written_frames = written_frames;
	s->atempo_ledger_output_frames = written_frames;
	s->atempo_ledger_next_ts_us = (int64_t)s->audio_time * 1000;
	s->atempo_ledger_last_log_ms = 0;
	// Snapshot the af_atempo media/RST baseline at the ledger epoch.  epoch_rst is
	// the media position of the ledger anchor TS; seed the running media/RST cursor
	// from it.  media_cursor tracks (ns_in - ring) = filter-consumed media that has
	// reached the ring head; next_rst_us is the RST(media) clock the ledger reports
	// back to the commit poll.
	s->atempo_epoch_rst = TS_TO_RST_TIME( s->audio_time, int );
	s->atempo_ledger_next_rst_us = (int64_t)s->atempo_epoch_rst * 1000;
	s->atempo_ledger_media_cursor = 0;
	s->atempo_ledger_media_valid = 0;
	{
		INT64 epoch_ns_in = 0, epoch_ns_out = 0;
		int epoch_ring = 0, epoch_af_rate = 0;
		if( stream_filter_audio_atempo_get_audit_state( s->audio_filter_atempo, &epoch_ns_in, &epoch_ns_out, &epoch_ring, &epoch_af_rate ) ) {
			s->atempo_ledger_media_cursor = epoch_ns_in - epoch_ring;
			s->atempo_ledger_media_valid = 1;
		}
	}
	DBG {
		UINT64 playhead_frames = 0;
		int playhead_rate = 0, playhead_src = 0, playhead_age = 0;
		int playhead_valid = audio_interface_get_presented_frames(
			s->audio_ctx, &playhead_frames, &playhead_rate, &playhead_src, &playhead_age, 1);
		int old_heard = stream_get_heard_audio_ts( s, s->audio_time );
		int ledger_at_playhead = STREAM_NO_PTS_VALUE;
		int arm_bias = STREAM_NO_PTS_VALUE;
		if( playhead_valid && written_rate > 0 ) {
			int64_t queued_frames = (int64_t)written_frames - (int64_t)playhead_frames;
			int64_t queued_us = (queued_frames * 1000000LL) / written_rate;
			ledger_at_playhead = s->audio_time - (int)(queued_us / 1000);
			arm_bias = ledger_at_playhead - old_heard;
		}
		serprintf("at_ledger_arm: written=%llu playhead=%llu queued=%lld out_cursor=%llu audio=%d old_heard=%d ledger_at_playhead=%d arm_bias=%d rate=%d playhead_valid=%d src=%d age=%d epoch_rst=%d\n",
			(unsigned long long)written_frames, (unsigned long long)playhead_frames,
			playhead_valid ? (long long)((int64_t)written_frames - (int64_t)playhead_frames) : -1,
			(unsigned long long)s->atempo_ledger_output_frames, s->audio_time,
			old_heard, ledger_at_playhead, arm_bias,
			written_rate, playhead_valid, playhead_src, playhead_age,
			s->atempo_epoch_rst);
	}
	return 1;
}

static int64_t _stream_atempo_ledger_frames_to_us(int frames, int sample_rate)
{
	return sample_rate > 0 ? ((int64_t)frames * 1000000) / sample_rate : 0;
}

// Append an output-side hold block for manual-delay silence: the playhead must
// be able to cross these output frames, but heard media time must NOT advance
// through them.  So advance output_frames only; leave next_ts_us / next_rst_us
// (the media/RST clocks) frozen and mark block_is_hold so the lookup plateaus
// heard media across the inserted silence.
static void _stream_atempo_ledger_append_hold(STREAM *s, int nframes, int sample_rate)
{
	if (!s || nframes <= 0 || sample_rate <= 0) {
		return;
	}
	if (!s->atempo_ledger_active && !_stream_atempo_ledger_arm(s, sample_rate)) {
		return;
	}
	STREAM_ATEMPO_LEDGER_ENTRY *entry = &s->atempo_ledger[s->atempo_ledger_write];
	entry->output_frames_start = s->atempo_ledger_output_frames;
	entry->block_ts_start = (int)(s->atempo_ledger_next_ts_us / 1000);
	entry->block_nframes = nframes;
	entry->rate = sample_rate;
	entry->block_rst_start = (int)(s->atempo_ledger_next_rst_us / 1000);
	entry->block_rst_span = 0;
	entry->block_is_hold = 1;
	entry->block_media_frames = 0;
	s->atempo_ledger_write = (s->atempo_ledger_write + 1) % STREAM_ATEMPO_LEDGER_SIZE;
	if (s->atempo_ledger_count < STREAM_ATEMPO_LEDGER_SIZE) {
		s->atempo_ledger_count++;
	}
	s->atempo_ledger_output_frames += (UINT64)nframes;
	if( s->atempo_ledger_dense_until_ms > 0 && atime() <= s->atempo_ledger_dense_until_ms ) {
		DBG serprintf("at_ledger_hold: out_start=%llu ts=%d nframes=%d rate=%d out_next=%llu\n",
			(unsigned long long)entry->output_frames_start, entry->block_ts_start,
			nframes, sample_rate, (unsigned long long)s->atempo_ledger_output_frames);
	}
}

static int _stream_atempo_ledger_reserve(STREAM *s, int nframes, int sample_rate)
{
	if (!s || nframes <= 0 || sample_rate <= 0) {
		return 0;
	}
	if (!s->atempo_ledger_active && !_stream_atempo_ledger_arm(s, sample_rate)) {
		return 0;
	}
	STREAM_ATEMPO_LEDGER_ENTRY *entry = &s->atempo_ledger[s->atempo_ledger_write];
	entry->output_frames_start = s->atempo_ledger_output_frames;
	entry->block_ts_start = (int)(s->atempo_ledger_next_ts_us / 1000);
	entry->block_nframes = nframes;
	entry->rate = sample_rate;
	// Advance the media/RST span of this output block.  Option B (primary): the
	// wrapper production output->media map records, at PRODUCTION time, the media
	// (ns_in - ring) consumed for each output-sample burst; look this block's span
	// up by its output-frame index.  This avoids the Option-A bias where sampling
	// ns_in-ring at this (write) time over-attributes media to output still queued
	// in the wrapper FIFO.  Option A (live ns_in-ring delta) is kept as the fallback
	// on map miss, and its media_cursor is advanced every block so it stays current.
	// 1:1 (TS-slope) is the final fallback so the ledger never stalls.
	entry->block_rst_start = (int)(s->atempo_ledger_next_rst_us / 1000);
	entry->block_is_hold = 0;
	entry->block_media_frames = 0;
	{
		int64_t block_rst_span_us = _stream_atempo_ledger_frames_to_us(nframes, sample_rate);
		int span_source = 0;       // 0=1:1, 1=A(live), 2=B(map)
		int64_t a_span_us = -1;    // Option A span (diagnostic comparison)
		// Option A: live ns_in-ring delta; always advance media_cursor so the A
		// pointer remains valid for any later block where the map misses.
		if( s->atempo_ledger_media_valid ) {
			INT64 ns_in_now = 0, ns_out_now = 0;
			int ring_now = 0, af_rate_now = 0;
			if( stream_filter_audio_atempo_get_audit_state( s->audio_filter_atempo, &ns_in_now, &ns_out_now, &ring_now, &af_rate_now ) && af_rate_now > 0 ) {
				INT64 media_now = ns_in_now - ring_now;
				INT64 span_frames = media_now - s->atempo_ledger_media_cursor;
				if( span_frames < 0 ) {
					span_frames = 0;
				}
				a_span_us = ((int64_t)span_frames * 1000000) / af_rate_now;
				s->atempo_ledger_media_cursor = media_now;
				entry->block_media_frames = span_frames;
				block_rst_span_us = a_span_us;
				span_source = 1;
			}
		}
		// Option B: production-map lookup by output-frame index.  This reserve runs
		// immediately AFTER _filter() produced and read exactly `nframes` output
		// samples from the wrapper FIFO (1:1, verified at the call site), so the
		// wrapper's cumulative output cursor now sits at the END of this block; the
		// block occupies wrapper range [out_cursor - nframes, out_cursor).  Reading
		// the cursor fresh each reserve needs no base bridge and is robust to partial
		// AudioTrack writes: wrapper output space is independent of how many frames
		// the sink accepted, so it never diverges from written-frame space.
		{
			UINT64 wrap_out_now = 0;
			int wrap_fifo_now = 0, wrap_rate_now = 0;
			INT64 b_span_frames = 0;
			int b_rate = 0;
			if( stream_filter_audio_atempo_get_ledger_stats( s->audio_filter_atempo, &wrap_out_now, &wrap_fifo_now, &wrap_rate_now )
				&& wrap_out_now >= (UINT64)nframes
				&& stream_filter_audio_atempo_lookup_output_media( s->audio_filter_atempo,
					wrap_out_now - (UINT64)nframes, nframes, &b_span_frames, &b_rate ) && b_rate > 0 ) {
				UINT64 w_start = wrap_out_now - (UINT64)nframes;
				if( b_span_frames < 0 ) {
					b_span_frames = 0;
				}
				block_rst_span_us = ((int64_t)b_span_frames * 1000000) / b_rate;
				span_source = 2;
				if( s->atempo_ledger_dense_until_ms > 0 && atime() <= s->atempo_ledger_dense_until_ms ) {
					DBG serprintf("at_ledger_omap: w_start=%llu nframes=%d b_span_us=%lld a_span_us=%lld diff_us=%lld\n",
						(unsigned long long)w_start, nframes,
						(long long)block_rst_span_us, (long long)a_span_us,
						(long long)( a_span_us >= 0 ? block_rst_span_us - a_span_us : 0 ));
				}
			}
		}
		(void)span_source;
		entry->block_rst_span = (int)(block_rst_span_us / 1000);
		s->atempo_ledger_next_rst_us += block_rst_span_us;
	}
	s->atempo_ledger_write = (s->atempo_ledger_write + 1) % STREAM_ATEMPO_LEDGER_SIZE;
	if (s->atempo_ledger_count < STREAM_ATEMPO_LEDGER_SIZE) {
		s->atempo_ledger_count++;
	}
	s->atempo_ledger_output_frames += (UINT64)nframes;
	s->atempo_ledger_next_ts_us += _stream_atempo_ledger_frames_to_us(nframes, sample_rate);
	if( s->atempo_ledger_dense_until_ms > 0 && atime() <= s->atempo_ledger_dense_until_ms ) {
		DBG serprintf("at_ledger_reserve: out_start=%llu ts=%d nframes=%d rate=%d out_next=%llu ts_next=%lld\n",
			(unsigned long long)entry->output_frames_start, entry->block_ts_start,
			nframes, sample_rate, (unsigned long long)s->atempo_ledger_output_frames,
			(long long)(s->atempo_ledger_next_ts_us / 1000));
	}
	return 1;
}

static void _stream_atempo_ledger_finalize(STREAM *s, int reserved_frames, int written_frames, int sample_rate)
{
	if (!s || !s->atempo_ledger_active || reserved_frames <= 0 || sample_rate <= 0) {
		return;
	}
	int idx = (s->atempo_ledger_write - 1 + STREAM_ATEMPO_LEDGER_SIZE) % STREAM_ATEMPO_LEDGER_SIZE;
	STREAM_ATEMPO_LEDGER_ENTRY *entry = &s->atempo_ledger[idx];
	if (written_frames <= 0) {
		s->atempo_ledger_write = idx;
		if (s->atempo_ledger_count > 0) {
			s->atempo_ledger_count--;
		}
		s->atempo_ledger_output_frames -= (UINT64)reserved_frames;
		s->atempo_ledger_next_ts_us -= _stream_atempo_ledger_frames_to_us(reserved_frames, sample_rate);
		// Roll back the media/RST cursor too so a cancelled (zero-write) reserve does
		// not leave the RST clock ahead.  next_rst_us rolls by the chosen span;
		// media_cursor (the Option-A pointer) rolls by the live A delta we advanced.
		s->atempo_ledger_next_rst_us -= (int64_t)entry->block_rst_span * 1000;
		if( s->atempo_ledger_media_valid ) {
			s->atempo_ledger_media_cursor -= entry->block_media_frames;
		}
		if( s->atempo_ledger_dense_until_ms > 0 && atime() <= s->atempo_ledger_dense_until_ms ) {
			DBG serprintf("at_ledger_cancel: reserved=%d out_next=%llu ts_next=%lld\n",
				reserved_frames, (unsigned long long)s->atempo_ledger_output_frames,
				(long long)(s->atempo_ledger_next_ts_us / 1000));
		}
		return;
	}
	if (written_frames != reserved_frames) {
		int delta = written_frames - reserved_frames;
		entry->block_nframes = written_frames;
		if (delta > 0) {
			s->atempo_ledger_output_frames += (UINT64)delta;
		} else {
			s->atempo_ledger_output_frames -= (UINT64)(-delta);
		}
		s->atempo_ledger_next_ts_us += _stream_atempo_ledger_frames_to_us(delta, sample_rate);
		// Scale this block's media/RST span by the written/reserved ratio so the RST
		// slope stays consistent on a partial write, and roll the RST cursor + media
		// cursor by the same shrink/grow.  block_rst_span (chosen source) drives
		// next_rst_us; block_media_frames (live A delta) drives media_cursor.
		int old_span_ms = entry->block_rst_span;
		int new_span_ms = reserved_frames > 0
			? (int)(((int64_t)old_span_ms * written_frames) / reserved_frames)
			: old_span_ms;
		int span_diff_ms = new_span_ms - old_span_ms;
		entry->block_rst_span = new_span_ms;
		s->atempo_ledger_next_rst_us += (int64_t)span_diff_ms * 1000;
		if( s->atempo_ledger_media_valid ) {
			INT64 old_media_frames = entry->block_media_frames;
			INT64 new_media_frames = reserved_frames > 0
				? ( old_media_frames * written_frames ) / reserved_frames
				: old_media_frames;
			s->atempo_ledger_media_cursor += new_media_frames - old_media_frames;
			entry->block_media_frames = new_media_frames;
		}
	}
	if( s->atempo_ledger_dense_until_ms > 0 && atime() <= s->atempo_ledger_dense_until_ms ) {
		DBG serprintf("at_ledger_append: out_start=%llu ts=%d reserved=%d nframes=%d rate=%d out_next=%llu ts_next=%lld\n",
			(unsigned long long)entry->output_frames_start, entry->block_ts_start,
			reserved_frames, written_frames, sample_rate,
			(unsigned long long)s->atempo_ledger_output_frames,
			(long long)(s->atempo_ledger_next_ts_us / 1000));
	}
}

static int _stream_audio_time_diag_active( STREAM *s )
{
	if( !s ) {
		return 0;
	}
	if( _stream_audio_speed_diag_active( s ) ) {
		return 1;
	}
	if( s->audio_time >= 0 && s->video_time >= 0 && ABS( s->audio_time - s->video_time ) >= 120 ) {
		return 1;
	}
	return 0;
}

static int stream_audio_format_supports_passthrough(int format)
{
	switch( format ) {
	case WAVE_FORMAT_AC3:
	case WAVE_FORMAT_EAC3:
	case WAVE_FORMAT_E_AC3_JOC:
	case WAVE_FORMAT_DTS:
	case WAVE_FORMAT_DTS_HD:
	case WAVE_FORMAT_DTS_HD_MA:
	case WAVE_FORMAT_TRUEHD:
		return 1;
	default:
		return 0;
	}
}

static int stream_audio_format_passthrough_available(int format)
{
	if( !stream_audio_format_supports_passthrough( format ) ) {
		return 0;
	}
#ifdef CONFIG_SPDIF
	return spdif_format_passthrough_supported( format );
#else
	return 1;
#endif
}

static int stream_audio_requested_passthrough_for_format(int format)
{
#ifdef CONFIG_SPDIF
	int passthrough_mode = spdif_is_passthrough_on();
	if( passthrough_mode && !stream_audio_format_passthrough_available( format ) ) {
		DBG serprintf("stream_audio: passthrough mode %d disabled for unsupported format %04X\n",
			passthrough_mode, format);
		passthrough_mode = 0;
	}
	return passthrough_mode;
#else
	(void)format;
	return 0;
#endif
}

void stream_audio_reset_ac3_passthrough_state(void)
{
	ac3_sink_configured = 0;
	ac3_reconfigure_pending = 1;
	audio_format_configured = -1;
	pcm_channel_cap = libavos_get_max_pcm_channels();
}

#define PASSTHROUGH_HAL_STANDBY_WAIT_MS 100

void stream_audio_wait_for_passthrough_idle(STREAM *s, const char *reason)
{
#ifdef CONFIG_SPDIF
	if (spdif_is_passthrough_on() == 2) {
		DBG serprintf("stream_audio: waiting %d ms for passthrough HAL (%s)\n",
			PASSTHROUGH_HAL_STANDBY_WAIT_MS, reason ? reason : "reconfig");
		msec_sleep(PASSTHROUGH_HAL_STANDBY_WAIT_MS);
	}
#else
	(void)s;
	(void)reason;
#endif
}

static void stream_audio_init_sink_defaults(AUDIO_PROPERTIES *sink)
{
	if( !sink ) {
		return;
	}
	if( sink->bitsPerSample == 0 ) {
		sink->bitsPerSample = 16;
	}
	if( sink->channels == 0 ) {
		sink->channels = 2;
	}
	if( sink->samplesPerSec == 0 ) {
		sink->samplesPerSec = 48000;
	}
	if( sink->bytesPerFrame == 0 && sink->channels && sink->bitsPerSample ) {
		sink->bytesPerFrame = sink->channels * sink->bitsPerSample / 8;
	}
	if( sink->bytesPerSec == 0 && sink->bytesPerFrame && sink->samplesPerSec ) {
		sink->bytesPerSec = sink->bytesPerFrame * sink->samplesPerSec;
	}
	if( sink->format == 0 ) {
		sink->format = WAVE_FORMAT_PCM;
	}
}

AUDIO_PROPERTIES *stream_audio_get_sink_props(STREAM *s)
{
	if( !s ) {
		return NULL;
	}
	AUDIO_PROPERTIES *sink = &s->audio_sink_props;
	return sink;
}

// ************************************************************
//
//	stream_audio_copy_sink_from_source
//
//	Initializes audio sink properties from the current source stream.
//	This function MUST be called before every audio_sink->start() call
//	to ensure sink properties are properly synchronized with the source.
//
//	What it does:
//	  1. Copies all audio properties from source (s->audio) to sink (s->audio_sink_props)
//	  2. Applies default values for any missing/zero fields (via stream_audio_init_sink_defaults)
//	  3. Forces format to WAVE_FORMAT_PCM when passthrough is disabled and AC3 recoding is off,
//	     since all compressed formats are decoded to PCM in this mode
//
//	NOTE: AC3 recoding and native passthrough paths call this function and then
//	override the format field to WAVE_FORMAT_AC3 or other compressed formats.
//	The PCM forcing step is harmless in these cases.
//
// ************************************************************
void stream_audio_copy_sink_from_source(STREAM *s)
{
	if( !s || !s->audio ) {
		return;
	}
	AUDIO_PROPERTIES *sink = stream_audio_get_sink_props( s );
	if( !sink ) {
		return;
	}
	memcpy( sink, s->audio, sizeof( AUDIO_PROPERTIES ) );
	stream_audio_init_sink_defaults( sink );

	// If a downmix was requested, reflect it in sink properties so AudioTrack
	// is created with the correct channel count.
	if( s->audio->request_channels > 0 &&
	    s->audio->request_channels < sink->channels ) {
		sink->channels = s->audio->request_channels;
		if( sink->bitsPerSample ) {
			sink->bytesPerFrame = sink->channels * sink->bitsPerSample / 8;
			sink->bytesPerSec = sink->samplesPerSec * sink->bytesPerFrame;
		}
	}

	// When passthrough is disabled, or when the current route does not support
	// this compressed format, decode to PCM and configure the sink as PCM before
	// the first decoded frame arrives.
	if( !libavos_get_ac3_recoding_enabled() &&
	    !stream_audio_requested_passthrough_for_format( sink->format ) ) {
		if( sink->format != WAVE_FORMAT_PCM ) {
			sink->format = WAVE_FORMAT_PCM;
		}
	}
}

static int stream_audio_setup_ac3_sink(STREAM *s)
{
#ifdef CONFIG_SPDIF
	if( !s || !s->audio_sink )
		return -1;

	AUDIO_PROPERTIES *sink = stream_audio_get_sink_props( s );
	stream_audio_copy_sink_from_source( s );

	// AC3 recoding now applies to all layouts (mono through 7.1).
	// Always proceed with passthrough setup; spdif_init will fail if the HAL cannot handle it.

	sink->format = WAVE_FORMAT_AC3;
	sink->channels = 2;
	sink->bitsPerSample = 16;
	sink->bytesPerFrame = 4;
	sink->samplesPerSec = 48000;
	sink->bytesPerSec = sink->samplesPerSec * sink->bytesPerFrame;

	stream_audio_wait_for_passthrough_idle(s, "ac3-init");

	if( !spdif_init( sink ) ) {
		serprintf("stream_audio_setup_ac3_sink: spdif_init failed\n");
		return -1;
	}

	int passthrough_mode = stream_audio_select_ac3_passthrough_mode(
		"stream_audio_setup_ac3_sink" );
	s->audio_sink->set_passthrough( s, passthrough_mode );

	stream_audio_wait_for_passthrough_idle(s, "ac3-prestart");

	if( s->audio_sink->start( s ) ) {
		serprintf("stream_audio_setup_ac3_sink: audiotrack start failed\n");
		return -1;
	}

	ac3_sink_configured = 1;
	ac3_reconfigure_pending = 0;
	audio_format_configured = sink->format;
	serprintf("stream_audio_setup_ac3_sink: sink ready fmt=%04X rate=%d ch=%d\n",
		sink->format, sink->samplesPerSec, sink->channels);
	return 0;
#else
	(void)s;
	return -1;
#endif
}

// ************************************************************
//
//	stream_audio_flush
//
// ************************************************************
void stream_audio_flush( STREAM *s )
{
	s->audio_buffer_size = 0;
	s->audio_end = 0;
	s->audio_time_remainder_us = 0;
	s->pcm_accum_size = 0;
	s->mode2_heard_interp_valid = 0;
	s->mode2_heard_interp_ts = STREAM_NO_PTS_VALUE;
	s->mode2_heard_interp_wall_ms = 0;
	s->mode2_heard_interp_raw_ts = STREAM_NO_PTS_VALUE;
	s->mode2_heard_interp_delay_ms = -1;
	s->mode2_heard_interp_last_log_ms = 0;
	// Discard any stale frontier seed; the paths that empty the sink buffer
	// (sink flush in the seek paths, passthrough sink reopen on format change)
	// re-arm it after this flush runs.
	s->mode2_heard_frontier_seed_pending = 0;
	s->ac3_recode_next_write_wall_ms = 0;
	s->ac3_recode_pacer_valid = 0;
	s->ac3_recode_pacer_max_lead_ms = 0;
	s->manual_audio_delay_target_ms = (s->av_delay < 0) ? -s->av_delay : 0;
	s->manual_audio_delay_applied_ms = 0;
	s->manual_audio_hold_pending_ms = 0;
	// AudioTrack playhead epoch is anchored to the pre-flush frame position;
	// clear it so the epoch clock does not run against a stale base.
	s->at_speed_epoch_active = 0;
	_stream_atempo_ledger_reset( s );

	if( s->audio_dec ) {
		s->audio_dec->flush( s->audio );
	}
	// Flush all active filters
	if( s->audio_filter_compress && s->audio_filter_compress->flush ) {
		s->audio_filter_compress->flush( s->audio_filter_compress );
	}
	if( s->audio_filter_ac3 && s->audio_filter_ac3->flush ) {
		s->audio_filter_ac3->flush( s->audio_filter_ac3 );
	}
	if( s->audio_filter && s->audio_filter->flush ) {
		s->audio_filter->flush( s->audio_filter );
	}
}

// ************************************************************
//
//	_set_audio_time
//
// ************************************************************
static void _set_audio_time( STREAM *s, int time )
{
	int passthrough = (s && s->audio_sink && s->audio_sink->get_passthrough) ?
		s->audio_sink->get_passthrough( s ) : 0;
	int old_time = s ? s->audio_time : -1;
	s->audio_time = time;
DBGA serprintf(" <<%d>> ", s->audio_time);
	if( passthrough == 1 ) {
		DBG serprintf("pt_mode1_audio_time_set: old=%d new=%d video=%d sync_a=%d seek_epoch=%d start_pending=%d resume_pending=%d ref=%d\n",
			old_time, s->audio_time, s->video_time, s->sync_a_time, s->seek_epoch,
			s->audio_start_pending, s->audio_resume_pending, s->audio_ref_time);
	}
	stream_sync_audio( s, s->audio_time );
}

// Enter the plain-mode2 PTS-seeded STREAM_SYNC_SAMPLES audio clock. Used for ordinary
// mode2 passthrough and, under the ac3_mode2_plain_policy gate, for AC3 recode
// whose sink resolves to mode2. Clears the CDATA synthetic startup anchor so
// startup_anchor_commit does not fire, and seeds the sample clock from the first PTS.
// Resets the wall-clock pacer once (it re-seeds on the next write). Callers must gate
// on s->sync_mode != STREAM_SYNC_SAMPLES so this runs exactly once per playback.
static void stream_audio_enter_mode2_sync_samples( STREAM *s, int chunk_pts, int ac3_recode )
{
DBG	serprintf("mode2_sync_mode: forcing STREAM_SYNC_SAMPLES (was %d) ac3_recode=%d\n",
		s->sync_mode, ac3_recode);
	s->sync_mode = STREAM_SYNC_SAMPLES;
	// Clear the CDATA startup anchor that may have been set by the current chunk's
	// PTS processing above; startup_anchor_commit would otherwise reintroduce the
	// synthetic video_time + pipeline_latency anchor we are avoiding.
	s->audio_start_pending    = 0;
	s->audio_start_pts        = STREAM_NO_PTS_VALUE;
	s->audio_start_target_ts  = STREAM_NO_PTS_VALUE;
	s->audio_time_remainder_us = 0;
	s->mode2_heard_interp_valid = 0;
	s->mode2_heard_interp_ts = STREAM_NO_PTS_VALUE;
	s->mode2_heard_interp_wall_ms = 0;
	s->mode2_heard_interp_raw_ts = STREAM_NO_PTS_VALUE;
	s->mode2_heard_interp_delay_ms = -1;
	s->mode2_heard_interp_last_log_ms = 0;
	s->ac3_recode_next_write_wall_ms = 0;
	s->ac3_recode_pacer_valid = 0;
	s->ac3_recode_pacer_max_lead_ms = 0;
	// Seed sample clock from the current chunk PTS if available so the first write
	// can immediately accumulate fakeSize duration without waiting for the next chunk.
	if( chunk_pts != STREAM_NO_PTS_VALUE ) {
		s->audio_ref_time = chunk_pts;
		s->audio_samples  = 0;
		_set_audio_time( s, chunk_pts );
DBG		serprintf("mode2_sync_mode: ref=%d\n", s->audio_ref_time);
	} else {
		s->audio_ref_time = -1;
		s->audio_samples  = 0;
		s->audio_time     = -1;
	}
}

// ************************************************************
//
//	_add_audio_time
//
// ************************************************************
static void _add_audio_time( STREAM *s, int time )
{
	if( s->audio_time != -1 ) {
		int passthrough = (s && s->audio_sink && s->audio_sink->get_passthrough) ?
			s->audio_sink->get_passthrough( s ) : 0;
		int before = s->audio_time;
		s->audio_time += time;
DBGA serprintf(" <+%d> ", time);
		if( passthrough == 1 ) {
			DBG serprintf("pt_mode1_audio_time_add: delta=%d before=%d after=%d video=%d sync_a=%d seek_epoch=%d remainder_us=%lld\n",
				time, before, s->audio_time, s->video_time, s->sync_a_time,
				s->seek_epoch, (long long)s->audio_time_remainder_us);
		}
		if( _stream_audio_time_diag_active( s ) ) {
			DBG serprintf("audio_time_apply: delta=%d before=%d after=%d video=%d remainder_us=%lld speed=%.3f\n",
				time, before, s->audio_time, s->video_time,
				(long long)s->audio_time_remainder_us,
				audio_interface_get_audio_speed() );
		}
		stream_sync_audio( s, s->audio_time );
	}
}

void stream_audio_debug( STREAM *s, int samples, int decoded, int time );

// ************************************************************
//
//	_decode
//
// ************************************************************
static int _decode( AUDIO_PROPERTIES *a, UCHAR *data, int size, AUDIO_FRAME *frame, int *decoded )
{
	STREAM *s = a->ctx;
	int time;
	s->audio_dec->decode( s->audio, data, size, frame, decoded, &time);

	stream_audio_debug( s, frame->size / s->audio->bytesPerFrame, *decoded, time );
	
	return 0;
}

static int _abort( STREAM *s )
{
	if( thread_state_asked( &s->audio_tstate ) == THREAD_RUNNING )
		return 0;
serprintf("_audio_abort!\r\n");		
	return 1;
}

static int _pcm_accum_target_bytes(const AUDIO_FRAME *frame)
{
	if( !frame || !frame->channels || !frame->bits || !frame->samplesPerSec ) {
		return 0;
	}
	int bytes_per_sample = frame->bits / 8;
	if( bytes_per_sample <= 0 ) {
		return 0;
	}
	int bytes_per_sec = frame->samplesPerSec * frame->channels * bytes_per_sample;
	int target = (bytes_per_sec * stream_audio_pcm_accum_ms) / 1000;
	// Keep a practical lower bound even for low-rate content.
	if( target < 4096 ) {
		target = 4096;
	}
	return target;
}

static void _pcm_accum_flush_to_sink(STREAM *s)
{
	if( !s || s->pcm_accum_size <= 0 ) {
		return;
	}
	// Pre-filter accumulator cannot be written directly to sink: it would bypass
	// atempo/compress/JNI filters. Drop tail on teardown/flush boundaries.
	DBG serprintf("stream_audio: pcm_accum drop on end size=%d\n", s->pcm_accum_size);
	s->pcm_accum_size = 0;
}

extern int DEBUG_delay;
void _stream_resync( STREAM *s );

static int64_t _stream_ac3_recode_written_duration_us(int size_written, int frame_size)
{
	if( size_written <= 0 ) {
		return 0;
	}
	if( frame_size <= 0 || size_written >= frame_size ) {
		return AC3_RECODE_FRAME_US;
	}
	return (AC3_RECODE_FRAME_US * size_written) / frame_size;
}

static int _wait( STREAM *s, int wait )
{
	int inserted = 0;
	if( s->audio_sink ) {
		while( wait ) {
			int to_wait = MIN( 20, wait );
			int samples = to_wait * s->audio->samplesPerSec / 1000;
			int size = samples * s->audio->bytesPerFrame;
			UCHAR silence[size];
			memset( silence, 0, size );
			AUDIO_FRAME frame = { 0 };
			frame.data = silence;
			frame.size = size;
			frame.format = WAVE_FORMAT_PCM;

			while( !s->audio_sink->can_write( s, frame.size ) ) {
				if( _abort( s ) ) {
					return inserted;
				}
				stream_yield_RT();
			}
			if( _abort( s ) ) {
				return inserted;
			}

			// This silence is an output hold for user A/V delay, not media
			// progress.  Do not advance audio_time.  When the atempo ledger is
			// active, append a zero-span hold block so the playhead can cross
			// the inserted silence without advancing heard media time.
			int size_written = s->audio_sink->write( s, &frame );
			if( size_written > 0 && audio_interface_is_using_atempo() &&
			    s->audio->bytesPerFrame > 0 && s->audio->samplesPerSec > 0 ) {
				_stream_atempo_ledger_append_hold( s,
					size_written / s->audio->bytesPerFrame,
					s->audio->samplesPerSec );
			}
			// Credit only the silence actually accepted by the sink. A partial or
			// failed write must not over-credit the scheduler hold, which would
			// push manual_audio_delay_applied_ms above what was really inserted.
			int written_ms = ( size_written > 0 && s->audio->bytesPerFrame > 0 &&
			    s->audio->samplesPerSec > 0 )
				? ( size_written / s->audio->bytesPerFrame ) * 1000 / s->audio->samplesPerSec
				: 0;
			wait -= to_wait;
			inserted += written_ms;
			if( size_written < size ) {
				// Sink did not take the full chunk; stop rather than spin.
				break;
			}
		}
	}
	return inserted;
}

static void _write_zero_data( STREAM *s, int time ) 
{
	int bytes = s->audio->bytesPerFrame * s->audio->samplesPerSec * time / 1000;
DBGA serprintf("_write_zero_data %d -> %d\r\n", time, bytes );

	UCHAR *zero = acalloc(1, bytes);
	
	while( !s->audio_sink->can_write( s,  bytes ) ) {
DBGA serprintf("x");
		msec_sleep( 1 );
	}
	if( _abort( s ) ) {
		afree(zero);
		return;
	}
DBGA serprintf("-Z-");
	AUDIO_FRAME frame = { 0 };
	frame.data = zero;
	frame.size = bytes;
	frame.format = WAVE_FORMAT_PCM;
	s->audio_sink->write( s, &frame );
	afree(zero);
}

// ************************************************************
//
//	_audio_decode
//
// ************************************************************
static void _audio_decode( STREAM *s )
{
	static int out_of_audio;
	
	if( s->paused || stream_audio_paused ) {
		s->audio_resume_pending = 1;
		s->audio_resume_valid_pending = 0;
		// Only arm the video hold for real pause/resume, not during seek preview.
		// Seek sets seek_paused before paused, so this distinguishes the two cases.
		// Arming the hold during seek would block every preview frame behind audio
		// readiness, destroying smooth scrubbing feedback.
		if( !s->seek_paused ) {
			s->video_hold_for_delay = 1;
			s->video_hold_for_resume_audio = 1;
		}
		s->manual_audio_delay_applied_ms = 0;
		s->manual_audio_hold_pending_ms = 0;
		s->pcm_accum_size = 0;
	}

	if( s->audio->valid && (!(s->paused || stream_audio_paused) || s->play_n_audio_frames ) ) {
		if( s->audio_sink && s->audio_preload ) {
			s->audio_preload = 0;
			// restuff the audio pipe! - unless this is a passthrough sink
			int passthrough = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
			if( s->audio_sink->syncable( s ) && !passthrough ) {
				s->at_speed_epoch_active = 0;
				_stream_atempo_ledger_reset( s );
				s->audio_sink->flush( s );
				s->manual_audio_delay_applied_ms = 0;
				s->manual_audio_hold_pending_ms = 0;
				s->audio_sink->preload( s );
				/* AudioTrack.pause()+flush leaves the track paused; resume playback so subsequent writes succeed. */
				s->audio_sink->start( s );
				if( s->audio_stuff_zero ) {
					s->audio_stuff_zero = 0;
					_write_zero_data( s, zero_time );
				}
			}
		}

		if( s->play_n_audio_frames > 0 ) {
			s->play_n_audio_frames --;
		}
		
		// Capture the sanitized PTS of the most recently fetched chunk so it remains
		// accessible after the inner cdata loop exits (cdata is scoped to that loop).
		int chunk_pts = STREAM_NO_PTS_VALUE;
decode_next_chunk:
		// no more audio in this chunk, then look for next
		while( s->audio_buffer_size <= 0 && !_abort( s ) ){

			STREAM_CDATA cdata = { 0 };

			// and try to get a new one
			if ( s->parser->get_audio_cdata( s, &s->audio_now, &cdata  ) ) {
				if ( s->audio_parse_end ) {
					if( s->audio_end == 0 ) {
serprintf("audio end\r\n");
						s->audio_end = 1;
						if( s->audio->format == WAVE_FORMAT_MPEGLAYER3 || s->audio->format == WAVE_FORMAT_AAC ) {
serprintf("audio flush\r\n");
							// append dummy chunk to make decoders happy and make them output all frames
							s->audio_buffer = s->audio_now.data;
							s->audio_buffer_size = s->audio->format == WAVE_FORMAT_MPEGLAYER3 ? 2048 : 6144;
							memset( s->audio_buffer, 0, s->audio_buffer_size );
						} else {
							if( s->audio_sink ) {
								_pcm_accum_flush_to_sink( s );
								// end, signal to the sink that we are finished
								s->audio_sink->end( s );
							}
						}
					}
				}
				if ( out_of_audio == 0 ) {
					out_of_audio = 1;
//serprintf("_OOA_");
				}
				// no more chunks, wait....
				stream_yield_RT();
				continue;
			}

			// if we have video as well, if video has stopped, drop all audio as well, but consume all chunks!
			if( s->video->valid && s->video_end ) {
DBGV serprintf("drop audio chunk: time %d\r\n", cdata.time );			
				continue;
			}
			
			if( cdata.valid ) {
				if( cdata.time != STREAM_NO_PTS_VALUE && cdata.time < 0 ) {
					// drop this shit!
DBGV serprintf("audio in the past! %d\r\n", cdata.time );
					continue;
				}
				if( !s->seek_audio_drop && s->seek_epoch > 0 && s->audio_time < 0 &&
					s->video_time >= 0 && cdata.time != STREAM_NO_PTS_VALUE &&
					cdata.time + 1000 < s->video_time ) {
					// First audio after seek is far behind video; rebase to avoid freeze.
					DBG serprintf("AUDIO_PTS_BEHIND_VIDEO: time=%d video=%d\n",
						cdata.time, s->video_time);
					cdata.time = s->video_time;
				}
				if( !s->seek_audio_drop && s->seek_epoch > 0 && s->audio_time >= 0 &&
					cdata.time != STREAM_NO_PTS_VALUE &&
					cdata.time + 1000 < s->audio_time ) {
					// After seek, treat large backward PTS jumps as invalid to avoid sync freeze.
					DBG serprintf("AUDIO_PTS_BACKWARD: time=%d prev=%d\n",
						cdata.time, s->audio_time);
					cdata.time = STREAM_NO_PTS_VALUE;
				}
				if( s->seek_audio_drop && cdata.time != STREAM_NO_PTS_VALUE &&
					cdata.time < s->seek_audio_target_ts ) {
					DBG serprintf("AUDIO_SEEK_DROP: time=%d target=%d\n",
						cdata.time, s->seek_audio_target_ts);
					continue;
				}
				if( s->seek_audio_drop && cdata.time == STREAM_NO_PTS_VALUE ) {
					DBG serprintf("AUDIO_SEEK_DROP_SKIP: no pts, target=%d\n",
						s->seek_audio_target_ts);
				}
				if( s->seek_audio_drop && cdata.time != STREAM_NO_PTS_VALUE &&
					cdata.time >= s->seek_audio_target_ts ) {
					DBG serprintf("AUDIO_SEEK_HIT: time=%d target=%d\n",
						cdata.time, s->seek_audio_target_ts);
					s->seek_audio_drop = 0;
					s->seek_audio_target_ts = 0;
				}

				if( stream_force_audio_props ) {
					cdata.changed = stream_force_audio_props;
					stream_force_audio_props = NULL;
				}
				// check if some audio props changed
				if( cdata.changed ) {
					stream_audio_props_changed( s, &cdata );
				}

				// we have a valid chunk
				if( cdata.pos != -1 ) {
					s->audio_pos = cdata.pos;
				}
				s->audio_buffer      = s->audio_now.data;
				s->audio_buffer_size = cdata.size;

				if( cdata.audio_skip ) {
serprintf("audio_skip(%d)!\r\n", cdata.time);
					_stream_resync( s );
					s->audio_ref_time = -1;
				}

				// Save sanitized PTS for use after the inner loop exits.
				chunk_pts = cdata.time;

				if( s->sync_mode == STREAM_SYNC_SAMPLES && s->speed == STREAM_SPEED_NORMAL ) {
					if( s->audio_ref_time == -1 && cdata.time != STREAM_NO_PTS_VALUE ) {
						s->audio_ref_time = cdata.time;
						s->audio_samples  = 0;
						_set_audio_time( s, cdata.time );
DBGA serprintf(" [[%d]] ", s->audio_ref_time);
					}
				} else {
					if( cdata.time != STREAM_NO_PTS_VALUE ) {
						int pts = cdata.time;
						if( s->put_time_mode &&
							s->audio_time < 0 && !s->audio_start_pending &&
							s->video_time >= 0 ) {
							// Startup: delay audio_time until the first audible output.
							// Capture a fixed target based on the first audio PTS so the hold
							// does not chase a moving audio_time.
							s->audio_start_pending = 1;
							s->audio_start_pts = pts;
							s->audio_start_target_ts = STREAM_NO_PTS_VALUE;
							DBG serprintf("audio_start_pending: pts=%d video_time=%d\n",
								s->audio_start_pts, s->video_time);
						} else if( s->audio_time < 0 ) {
							_set_audio_time( s, pts );
						}
					}
				}

				// While accumulating tiny PCM batches, avoid repeatedly re-anchoring put_time
				// with unchanged audio_time (dt=0); sync will be updated when a batch is written.
				if( s->pcm_accum_size == 0 ) {
					while( !_abort( s ) && stream_sync_audio( s, s->audio_time ) ) {
DBGS serprintf("~");
						msec_sleep( 10 );
						stream_yield_RT();
					}
				}

				out_of_audio = 0;

				if( s->dump_audio_fd > 0 ) {
					file_write( s->dump_audio_fd, s->audio_buffer, s->audio_buffer_size );
				}
			}
		} 

		if( s->speed != STREAM_SPEED_NORMAL ) {
			// we play SLOW video, eat up all the audio that is behind us
			if( s->video_time > s->audio_time ) {
				// eat all bytes in current audio chunk
				s->audio_buffer_size = 0;
			}
			return;
		} 

		int passthrough = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
		int ac3_recoding = libavos_get_ac3_recoding_enabled();
		int passthrough_supported = stream_audio_format_passthrough_available( s->audio->format );
		int passthrough_active = passthrough && (passthrough_supported || ac3_recoding);
		if( passthrough && !passthrough_supported && !ac3_recoding ) {
			DBG serprintf("stream_audio: codec %04X not supported by route for passthrough, decoding as PCM\n",
				s->audio->format);
			if( s->audio_sink ) {
				s->audio_sink->set_passthrough( s, 0 );
			}
			passthrough_active = 0;
			passthrough = 0;
		}

		// Mode2 passthrough: force STREAM_SYNC_SAMPLES once the sink resolves to mode2.
		// CDATA/PTS mode enters a synthetic startup anchor (video_time + pipeline_latency)
		// and advances from byte-ratio duration; mode2 compressed packets are better served
		// by sample counting from the first valid demuxer PTS (same as FLAC).
		// AC3 recoding historically kept the mode1-designed recode policy (CDATA anchor +
		// wall-clock pacer) even when its sink resolved to mode2. The plain-policy gate opts
		// AC3 recode into this same plain-mode2 audio clock when its sink resolves to mode2.
		// The sample clock here counts decoded PCM samples (audio_samples), independent of
		// fakeSize, and recoded AC3 has a fixed 1536-samples/48000Hz = 32 ms cadence. The
		// pacer is left intact so only the startup/audio-clock policy changes.
		// Plain mode2 (non-recode) resolves its passthrough mode before this point, so it is
		// caught here. AC3 recode resolves mode2 lazily later in this iteration
		// (stream_audio_setup_ac3_sink), so the recode experiment normally transitions there,
		// before the first write/anchor. The recode branch below is only a fallback for the
		// rare case where that first chunk carried no PTS: it catches the transition on a later
		// chunk once a valid PTS arrives (passthrough now reads 2).
		int ac3_recode_mode2_sync = ac3_recoding && ac3_mode2_plain_policy;
		if( passthrough_active && passthrough == 2 &&
			s->sync_mode != STREAM_SYNC_SAMPLES ) {
			if( !ac3_recoding ) {
				stream_audio_enter_mode2_sync_samples( s, chunk_pts, 0 );
			} else if( ac3_recode_mode2_sync && chunk_pts != STREAM_NO_PTS_VALUE ) {
				stream_audio_enter_mode2_sync_samples( s, chunk_pts, 1 );
			}
		}

		AUDIO_FRAME audio_frame = { 0 };
		int decoded = 0;

		audio_frame.time = s->audio_time;

		// For AC3 recoding, always decode ALL formats (including AC3) to PCM to enable filters
		// This provides consistent audio boost/night mode support for all source formats
		if( s->audio_dec && (!passthrough_active || ac3_recoding) ) {
			// Decode audio to PCM
			audio_frame.samplesPerSec = s->audio->samplesPerSec;	

			// we need to pass the STREAM to the _decode() call!
			s->audio->ctx = s;
			_decode( s->audio, s->audio_buffer, s->audio_buffer_size, &audio_frame, &decoded );
		
			// did the sample rate change?
			if( !audio_frame.error && audio_frame.size && audio_frame.samplesPerSec && audio_frame.samplesPerSec != s->audio->samplesPerSec ) {
serprintf("sample_rate changed! %d\r\n", audio_frame.samplesPerSec);
				s->audio->sourceSamples = s->audio->samplesPerSec;
				s->audio->samplesPerSec = audio_frame.samplesPerSec;
				if( s->audio->channels && s->audio->bitsPerSample ) {
					s->audio->bytesPerFrame = s->audio->channels * s->audio->bitsPerSample / 8;
				}
				if( s->audio->bytesPerFrame == 0 ) {
					int floor_channels = s->audio->channels ? s->audio->channels : 2;
					s->audio->bytesPerFrame = floor_channels * 2; // assume 16-bit PCM
				}
				if( s->audio->samplesPerSec && s->audio->bytesPerFrame ) {
					s->audio->bytesPerSec = s->audio->samplesPerSec * s->audio->bytesPerFrame;
				}
				stream_audio_samplerate_changed( s );		
			}
		
		} else {
#ifdef CONFIG_SPDIF
			AUDIO_PROPERTIES *spdif_props = stream_audio_get_sink_props( s );
			spdif_props->ctx = s;
			spdif_encapsulate( spdif_props, s->audio_buffer, s->audio_buffer_size, &audio_frame, &decoded );
#endif
		}

#ifdef CONFIG_SPDIF
		// In plain passthrough mode the SPDIF muxer produces IEC frames but leaves the frame
		// metadata empty (format/channels/rate/bits). Preserve the source properties so the
		// filter/reconfigure logic treats the frame as the original compressed format instead
		// of thinking it turned into WAVE_FORMAT_UNKNOWN, which would force unwanted sink
		// reconfigurations and break passthrough mode 2.
		if( passthrough_active && !ac3_recoding ) {
			if( !audio_frame.format )
				audio_frame.format = s->audio->format;
			if( !audio_frame.channels )
				audio_frame.channels = s->audio->channels;
			if( !audio_frame.samplesPerSec )
				audio_frame.samplesPerSec = s->audio->samplesPerSec;
			if( !audio_frame.bits )
				audio_frame.bits = s->audio->bitsPerSample;
		}
#endif

//serprintf("(dec %d | %d )", decoded, audio_frame.size );
		s->audio_buffer      += decoded;
		s->audio_buffer_size -= decoded;

		if( s->sync_mode == STREAM_SYNC_SAMPLES ) {
			if( audio_frame.error ) {
serprintf(" ae! ");
				// there was a decoding error, resync the time
				s->audio_ref_time  = -1;
			}
		} else {
			// Audio time will be updated AFTER filtering (see below)
			// to account for actual output size (important for atempo filter)
		}
		
		if( s->dump_pcm_fd > 0 ) {
			file_write( s->dump_pcm_fd, audio_frame.data, audio_frame.size );
		}

	int original_format = 0;
	int original_channels = 0;
	int original_rate = 0;
	int original_bits = 0;
	int frame_channels = 0;
	int use_atempo = 0;
	int using_pcm_accum = 0;
	int dbg_atempo_in = 0, dbg_atempo_out = 0, dbg_atempo_fifo = 0;

		// Reset resume_write diagnostic counter before atempo runs so the
		// first post-resume filter call is captured at index [0].
		// Key on the actual pending flag transition: fire once when pending
		// goes true, disarm when it goes false. This is robust regardless of
		// how many outer-loop iterations audio_resume_pending stays set.
		if (s->audio_resume_pending && !resume_seen_pending) {
			resume_write_log_count = 0;
			resume_seen_pending = 1;
		}
		if (!s->audio_resume_pending) {
			resume_seen_pending = 0;
		}

		// Pre-filter PCM accumulation: only when atempo is active AND speed != 1.0x.
		// Coalesces tiny decoder output so atempo WSOLA runs on larger batches.
		// At exactly 1.0x atempo is a passthrough; accumulating a larger batch
		// only increases the write quantum and the A/V diff oscillation amplitude.
		// atempo_filter_enabled: whether the filter will actually run this frame.
		// atempo_accum_enabled:  whether pre-accumulation should coalesce frames
		//                        (disabled at 1.0x to halve the write burst size).
		int atempo_filter_enabled = (s->audio_filter_atempo != NULL &&
			audio_interface_is_audio_speed_enabled() &&
			audio_interface_is_using_atempo() &&
			passthrough != 1 && passthrough != 2);
		int atempo_accum_enabled = atempo_filter_enabled &&
			fabsf(audio_interface_get_audio_speed() - 1.0f) > 1e-6f;
		int pcm_eligible = (atempo_accum_enabled &&
			!passthrough_active &&
			!ac3_recoding &&
			!audio_frame.error &&
			audio_frame.size > 0 &&
			audio_frame.format == WAVE_FORMAT_PCM &&
			audio_frame.channels > 0 &&
			audio_frame.bits > 0 &&
			audio_frame.samplesPerSec > 0);

		if( pcm_eligible ) {
			if( s->pcm_accum_size > 0 &&
				(s->pcm_accum_format   != audio_frame.format ||
				 s->pcm_accum_channels != audio_frame.channels ||
				 s->pcm_accum_bits     != audio_frame.bits ||
				 s->pcm_accum_rate     != audio_frame.samplesPerSec) ) {
				DBG serprintf("stream_audio: pcm_accum reset on format change (%04X/%dch/%db/%dHz -> %04X/%dch/%db/%dHz)\n",
					s->pcm_accum_format, s->pcm_accum_channels, s->pcm_accum_bits, s->pcm_accum_rate,
					audio_frame.format, audio_frame.channels, audio_frame.bits, audio_frame.samplesPerSec);
				s->pcm_accum_size = 0;
			}

			if( s->pcm_accum_size == 0 ) {
				s->pcm_accum_format = audio_frame.format;
				s->pcm_accum_channels = audio_frame.channels;
				s->pcm_accum_bits = audio_frame.bits;
				s->pcm_accum_rate = audio_frame.samplesPerSec;
			}

			if( s->pcm_accum_capacity < s->pcm_accum_size + audio_frame.size ) {
				int new_cap = MAX( s->pcm_accum_capacity * 2, s->pcm_accum_size + audio_frame.size );
				unsigned char *new_buf = arealloc( s->pcm_accum_data, new_cap );
				if( !new_buf ) {
					DBG serprintf("stream_audio: pcm_accum alloc failed (%d), fallback to direct frame\n", new_cap);
				} else {
					s->pcm_accum_data = new_buf;
					s->pcm_accum_capacity = new_cap;
				}
			}

			if( s->pcm_accum_data && s->pcm_accum_capacity >= s->pcm_accum_size + audio_frame.size ) {
				memcpy( s->pcm_accum_data + s->pcm_accum_size, audio_frame.data, audio_frame.size );
				s->pcm_accum_size += audio_frame.size;

				int pcm_accum_target = _pcm_accum_target_bytes( &audio_frame );
				if( s->pcm_accum_size < pcm_accum_target ) {
					// Keep decoding in this call to avoid thread-loop overhead
					// and produce a steady batch cadence for tiny-frame codecs.
					goto decode_next_chunk;
				}

				audio_frame.data = s->pcm_accum_data;
				audio_frame.size = s->pcm_accum_size;
				audio_frame.format = s->pcm_accum_format;
				audio_frame.channels = s->pcm_accum_channels;
				audio_frame.bits = s->pcm_accum_bits;
				audio_frame.samplesPerSec = s->pcm_accum_rate;
				using_pcm_accum = 1;
			}
		} else if( s->pcm_accum_size > 0 ) {
			// Non-eligible frame arrived while PCM was accumulated.
			// Flush the pending PCM through the normal filter/sink path
			// instead of dropping it, which would cause silent playback.
			DBG serprintf("stream_audio: pcm_accum flush pending=%d (next format=%04X passthrough_active=%d ac3_recoding=%d)\n",
				s->pcm_accum_size, audio_frame.format, passthrough_active, ac3_recoding);
			audio_frame.data = s->pcm_accum_data;
			audio_frame.size = s->pcm_accum_size;
			audio_frame.format = s->pcm_accum_format;
			audio_frame.channels = s->pcm_accum_channels;
			audio_frame.bits = s->pcm_accum_bits;
			audio_frame.samplesPerSec = s->pcm_accum_rate;
			using_pcm_accum = 1;
		}

		if( s->audio_sink ) {
			AUDIO_PROPERTIES *sink_props = stream_audio_get_sink_props( s );
			if( !audio_frame.error ) {
				// Store original format and properties before filtering
				original_format = sink_props ? sink_props->format : s->audio->format;
				original_channels = (s->audio->request_channels > 0) ?
					s->audio->request_channels : s->audio->channels;
				original_rate = s->audio->samplesPerSec;
				original_bits = s->audio->bitsPerSample;

					DBG3 serprintf("stream_audio: decoded frame fmt=%04X size=%d passthrough=%d active=%d recoding=%d\n",
						audio_frame.format, audio_frame.size, passthrough, passthrough_active, ac3_recoding);

				// For AC3 recoding, always run filters on ALL decoded formats
				// This provides consistent audio boost/night mode for all sources
				int run_filter = (!passthrough_active || ac3_recoding);
				frame_channels = audio_frame.channels ? audio_frame.channels : s->audio->channels;

				// Apply atempo speed control filter FIRST (changes audio duration)
				// Smart bypass: Only use atempo when all conditions are met:
				// 1. Audio speed feature is enabled
				// 2. User selected atempo (not AudioTrack PlaybackParams)
				// 3. NOT in passthrough mode 1 or 2 (compressed audio to receiver)
				use_atempo = (s->audio_filter_atempo != NULL);
				int audio_speed_enabled = audio_interface_is_audio_speed_enabled();
				int using_atempo_pref = audio_interface_is_using_atempo();
				if (!audio_speed_enabled) {
					use_atempo = 0;  // Audio speed feature disabled
				}
				if (!using_atempo_pref) {
					use_atempo = 0;  // User chose AudioTrack-based speed
				}
				if (passthrough > 0) {
					use_atempo = 0;  // Passthrough mode active
				}
				if (atempo_gate_log_count < 10) {
					DBG2 serprintf("stream_audio: atempo_gate[%d] filter=%p speed_enabled=%d using_atempo_pref=%d passthrough=%d frame_size=%d use=%d speed=%.3f\n",
						atempo_gate_log_count, s->audio_filter_atempo, audio_speed_enabled,
						using_atempo_pref, passthrough, audio_frame.size, use_atempo,
						audio_interface_get_audio_speed());
					atempo_gate_log_count++;
				}

				if (use_atempo && audio_frame.size > 0) {
					int _atempo_before_fifo = (s->audio_filter_atempo && s->audio_filter_atempo->delay) ?
						s->audio_filter_atempo->delay( s->audio_filter_atempo ) : 0;
					if( _stream_audio_speed_diag_active( s ) ) {
						DBG serprintf("atempo_diag: epoch=%d speed=%.3f in_size=%d in_rate=%d in_ch=%d in_fake=%d fifo_before=%d audio_time=%d video_time=%d\n",
							s->audio_speed_diag_epoch, audio_interface_get_audio_speed(),
							audio_frame.size, audio_frame.samplesPerSec, audio_frame.channels,
							audio_frame.fakeSize, _atempo_before_fifo, s->audio_time, s->video_time);
					}
					if (resume_write_log_count < 3) {
						dbg_atempo_in = audio_frame.size;
						dbg_atempo_fifo = _atempo_before_fifo;
					}
					DBG serprintf("stream_audio: applying atempo filter\n");
					s->audio_filter_atempo->filter(s->audio_filter_atempo, &audio_frame);
					if (resume_write_log_count < 3) {
						dbg_atempo_out = audio_frame.size;
						dbg_atempo_fifo = (s->audio_filter_atempo && s->audio_filter_atempo->delay) ?
							s->audio_filter_atempo->delay( s->audio_filter_atempo ) : 0;
					}
					if( _stream_audio_speed_diag_active( s ) ) {
						int after_delay = (s->audio_filter_atempo && s->audio_filter_atempo->delay) ?
							s->audio_filter_atempo->delay( s->audio_filter_atempo ) : 0;
						DBG serprintf("atempo_diag: epoch=%d speed=%.3f out_size=%d out_rate=%d out_ch=%d out_fake=%d fifo_after=%d audio_time=%d video_time=%d\n",
							s->audio_speed_diag_epoch, audio_interface_get_audio_speed(),
							audio_frame.size, audio_frame.samplesPerSec, audio_frame.channels,
							audio_frame.fakeSize, after_delay, s->audio_time, s->video_time);
					}
				}

				if( run_filter ) {
					// Apply filters in order: compress -> AC3 -> JNI
															// 1. Compression/boost filter
															if( s->audio_filter_compress && audio_frame.size > 0 ) {
																// For multichannel audio, skip compression if both boost and night mode are disabled.
																if( frame_channels > 2 && s->audio_filter_level == 0 && !s->audio_filter_night_on ) {
																	DBG serprintf("stream_audio: skipping compress filter -- level=%d night_on=%d channels=%d\n",
																		s->audio_filter_level, s->audio_filter_night_on, frame_channels);
																} else {
																	{
																		// Lazy initialization for AC3 recoding mode
																		// Check if filter needs to be opened (priv == NULL means not opened yet)
																		if( ac3_recoding && !s->audio_filter_compress->priv ) {
																			AUDIO_PROPERTIES props = {0};
																			props.channels = audio_frame.channels ? audio_frame.channels : s->audio->channels;
																			props.samplesPerSec = audio_frame.samplesPerSec ? audio_frame.samplesPerSec : s->audio->samplesPerSec;
																			props.bitsPerSample = audio_frame.bits ? audio_frame.bits : s->audio->bitsPerSample;
										
																			DBG serprintf("stream_audio: lazy init compress filter with frame properties (%dch/%dHz)\n",
																				props.channels, props.samplesPerSec);
										
																			if( s->audio_filter_compress->open( s->audio_filter_compress, &props ) ) {
																				serprintf("stream_audio: ERROR: failed to lazy init compress filter\n");
																				// Delete the filter to prevent future attempts
																				if( s->audio_filter_compress->delete ) {
																					s->audio_filter_compress->delete( s->audio_filter_compress );
																				}
																				s->audio_filter_compress = NULL;
																			} else {
																				DBG serprintf("stream_audio: compress filter lazy init complete\n");
																			}
																		}
										
																		if( s->audio_filter_compress ) {
																			DBG serprintf("stream_audio: applying compress filter\n");
																			s->audio_filter_compress->filter( s->audio_filter_compress, &audio_frame );
																		}
																	}
																}
															}					}
					// 2. AC3 encoding filter (only in AC3 recoding mode)
					// Use audio_frame.channels if set, otherwise fall back to s->audio->channels
					if( s->audio_filter_ac3 && audio_frame.size > 0 ) {
						DBG serprintf("stream_audio: applying AC3 filter (pre format=%04X size=%d channels=%d)\n",
							audio_frame.format, audio_frame.size, frame_channels);
						s->audio_filter_ac3->filter( s->audio_filter_ac3, &audio_frame );
						DBG serprintf("stream_audio: AC3 filter applied (post format=%04X size=%d fakeSize=%d)\n",
							audio_frame.format, audio_frame.size, audio_frame.fakeSize);
					}
					// 3. Legacy AGC filter (fallback if compress not available)
					if( s->audio_filter && audio_frame.size > 0 ) {
						DBG serprintf("stream_audio: applying AGC filter\n");
						s->audio_filter->filter( s->audio_filter, &audio_frame );
					}
				}
				// 4. JNI filter always runs
				s->audio_filter_jni->filter( s->audio_filter_jni, &audio_frame );

					DBG3 serprintf("stream_audio: post-filter frame fmt=%04X size=%d\n",
						audio_frame.format, audio_frame.size);

				// Check if filter changed the audio format or layout (e.g., PCM -> AC3 recoding)
				int expected_format = sink_props ? sink_props->format : original_format;
				int is_pcm_to_pcm = (!ac3_recoding && sink_props &&
				                     sink_props->format == WAVE_FORMAT_PCM &&
				                     audio_frame.format == WAVE_FORMAT_PCM);
				int format_changed = 0;
				if( !is_pcm_to_pcm ) {
					format_changed = audio_frame.format && audio_frame.format != expected_format;
				}
				if( ac3_recoding && ac3_sink_configured && sink_props &&
				    sink_props->format == WAVE_FORMAT_AC3 && audio_frame.format == WAVE_FORMAT_AC3 ) {
					// Once the sink is configured for AC3 recoding, treat AC3 frames as expected
					format_changed = 0;
				}
				int channels_changed = audio_frame.channels && audio_frame.channels != original_channels;
				int samplerate_changed = audio_frame.samplesPerSec && audio_frame.samplesPerSec != original_rate;
				int bits_changed = audio_frame.bits && audio_frame.bits != original_bits;

				// Track if sink is already configured for AC3 recoding to avoid redundant reconfigurations
				int is_ac3_recoding = ac3_recoding && audio_frame.format == WAVE_FORMAT_AC3;
				// Containerized AC3 frames always report 2 channels @48kHz regardless of original layout.
				// Avoid treating those synthetic values as real layout changes.
				int passthrough_layout_changed = (!is_ac3_recoding) && (channels_changed || samplerate_changed || bits_changed);
				// For AC3 recoding, reconfigure once when the AC3 sink is (re)opened.
				int need_reconfigure = format_changed || passthrough_layout_changed ||
				                       (is_ac3_recoding && (ac3_reconfigure_pending || !ac3_sink_configured));
				if( is_ac3_recoding ) {
					if( !ac3_sink_configured ) {
						if( stream_audio_setup_ac3_sink( s ) ) {
							DBG serprintf("stream_audio: failed to configure AC3 sink, fallback to PCM\n");
						} else if( ac3_recode_mode2_sync &&
						           s->sync_mode != STREAM_SYNC_SAMPLES &&
						           s->audio_sink->get_passthrough &&
						           s->audio_sink->get_passthrough( s ) == 2 ) {
							// The AC3 sink just resolved to mode2. Enter the plain-mode2 samples
							// clock NOW, before the first write commits the mode1 CDATA anchor.
							// Seed from this first frame's PTS so frame 0 is on the clock; if the
							// chunk has no PTS the helper enters an unseeded state (ref=-1) that a
							// later valid PTS seeds, rather than letting frame 0 use the old policy.
							stream_audio_enter_mode2_sync_samples( s, chunk_pts, 1 );
						}
						need_reconfigure = 0;
					} else if( sink_props && sink_props->format == WAVE_FORMAT_AC3 ) {
						need_reconfigure = 0;
						audio_format_configured = sink_props->format;
						DBG serprintf("stream_audio: AC3 sink already configured, skipping reconfigure\n");
					}
				}

				if( audio_frame.size > 0 && need_reconfigure ) {
					DBG serprintf("audio format changed by filter: %04X -> %04X, reconfiguring sink (passthrough=%d, ac3=%d)\n",
						original_format, audio_frame.format, passthrough, ac3_recoding);
					// Always update audio_format_configured when reconfiguring sink to prevent
					// redundant passthrough reconfiguration in the audio thread loop.
					audio_format_configured = stream_audio_get_sink_props( s )->format;
					if( is_ac3_recoding ) {
						ac3_reconfigure_pending = 0;
					}

					// For AC3 recoding, we need to keep s->audio with the ORIGINAL source properties
					// (6-channel or 8-channel EAC3) for proper timing calculations, but configure
					// AudioTrack with AC3 2-channel IEC61937 format.
					// The sync code uses s->audio->bytesPerFrame to convert fakeSize to samples,
					// so s->audio must reflect the original decoded PCM format, not the AC3 container.
					if( !is_ac3_recoding ) {
						// For non-AC3-recoding format changes, update s->audio properties normally
						if( format_changed ) {
							s->audio->format = audio_frame.format;
						}
						if( samplerate_changed ) {
							s->audio->samplesPerSec = audio_frame.samplesPerSec;
							s->audio->sourceSamples = audio_frame.samplesPerSec;
						}
						if( channels_changed ) {
							s->audio->channels = audio_frame.channels;
							s->audio->sourceChannels = audio_frame.channels;
						}
						if( bits_changed ) {
							s->audio->bitsPerSample = audio_frame.bits;
							s->audio->sourceBitsPerSample = audio_frame.bits;
						}
						if( s->audio->channels && s->audio->bitsPerSample ) {
							s->audio->bytesPerFrame = s->audio->channels * s->audio->bitsPerSample / 8;
						}
						if( s->audio->bytesPerFrame == 0 ) {
							int floor_channels = s->audio->channels ? s->audio->channels : 2;
							s->audio->bytesPerFrame = floor_channels * 2; // assume 16-bit PCM
						}
						if( s->audio->samplesPerSec && s->audio->bytesPerFrame ) {
							s->audio->bytesPerSec = s->audio->samplesPerSec * s->audio->bytesPerFrame;
						}
					}
					// For AC3 recoding: s->audio keeps the original source format/channels/bits
					// but we need to temporarily update to AC3 format for AudioTrack configuration

					// DEBUG: Check if format was accidentally modified
					if( is_ac3_recoding && original_format != s->audio->format ) {
DBG serprintf("stream_audio: WARNING! s->audio->format changed from %04X to %04X during AC3 recoding!\r\n",
						original_format, s->audio->format);
					}
					// Reconfigure audio sink with new parameters
					if( s->audio_sink ) {
						if( s->audio_sink_open ) {
							int passthrough_mode = stream_audio_requested_passthrough_for_format( s->audio->format );
							if( passthrough_mode == 0 ) {
								s->at_speed_epoch_active = 0;
								_stream_atempo_ledger_reset( s );
								s->manual_audio_delay_target_ms = (s->av_delay < 0) ? -s->av_delay : 0;
								s->manual_audio_delay_applied_ms = 0;
								s->manual_audio_hold_pending_ms = 0;
								s->audio_sink->flush( s );
							}
							s->audio_sink->stop( s );
							if( passthrough_mode > 0 ) {
								stream_audio_wait_for_passthrough_idle(s, "format-change");
								if( s->audio_sink->close && s->audio_sink->open ) {
									s->audio_sink->close( s );
									stream_audio_wait_for_passthrough_idle(s, "passthrough-reopen");
									if( s->audio_sink->open( s ) ) {
										DBG serprintf("failed to reopen audio sink for passthrough\n");
										s->audio_sink_open = 0;
									}
								}
								// The recreated track starts with an EMPTY buffer: the HAL
								// consumes the first write immediately, so the first frame's
								// physical presentation begins at write time, not
								// selected_delay later. Tell the mode2 heard interpolator to
								// seed its next epoch at the frontier (audio_time) instead of
								// audio_time - selected_delay. It cannot infer this itself:
								// audio_time is continuous across a mid-playback track change,
								// so the raw heard endpoint does not jump backward (avos-446:
								// raw-seeded epochs made the sync gate hold video against a
								// phantom deficit that the wall-anchored blit schedule then
								// kept forever, ~380-1050ms added per track change).
								s->mode2_heard_frontier_seed_pending = 1;
							} else if( !is_ac3_recoding &&
							           (format_changed || channels_changed || samplerate_changed || bits_changed) &&
							           s->audio_sink->close && s->audio_sink->open ) {
								s->audio_sink->close( s );
								if( s->audio_sink->open( s ) ) {
									DBG serprintf("failed to reopen audio sink after PCM format change\n");
									s->audio_sink_open = 0;
								}
							}
						}
						// For AC3 recoding, the AC3 encoder outputs AC3 compressed frames that need
						// to be sent via passthrough mode 1 (manual IEC61937 wrapping).
						// This allows androidTV devices with ARC (non-eARC) to transmit multichannel
						// audio to soundbars that support AC3 but not the original codec or PCM multichannel.
						AUDIO_PROPERTIES *sink = stream_audio_get_sink_props( s );
						if( is_ac3_recoding ) {
							DBG serprintf("AC3 recoding: configuring sink for compressed passthrough mode 1 (IEC61937)\n");

							AUDIO_PROPERTIES saved_sink = *sink;

							// Configure sink to emit IEC61937-wrapped AC3 regardless of source layout
							sink->format = WAVE_FORMAT_AC3;
							sink->channels = 2;
							sink->bitsPerSample = 16;
							sink->bytesPerFrame = 4;
							sink->samplesPerSec = 48000;
							sink->bytesPerSec = sink->samplesPerSec * sink->bytesPerFrame;

#ifdef CONFIG_SPDIF
							int ac3_sink_started = 0;
							if( spdif_init(sink) ) {
								int passthrough_mode = stream_audio_select_ac3_passthrough_mode(
									"AC3 recoding reconfigure" );
								s->audio_sink->set_passthrough( s, passthrough_mode );
								// Call start() with AC3 2-channel format
								if( s->audio_sink->start( s ) ) {
									DBG serprintf("failed to restart audio sink after AC3 recoding\n");
									s->audio_sink_open = 0;
									ac3_sink_configured = 0;
									*sink = saved_sink;
								} else {
									s->audio_sink_open = 1;
									ac3_sink_configured = 1;
									ac3_reconfigure_pending = 0;
									ac3_sink_started = 1;
									audio_format_configured = sink->format;
								}
							} else {
								DBG serprintf("AC3 recoding: failed to initialize SPDIF muxer\n");
								*sink = saved_sink;
								s->audio_sink->set_passthrough( s, 0 );
								ac3_sink_configured = 0;
								ac3_reconfigure_pending = 1;
								s->audio_sink_open = 0;
							}
							(void)ac3_sink_started;
#else
							(void)sink;
#endif
						} else {
							// For non-AC3 recoding, copy source properties to sink and reconfigure.
							// Do NOT copy if we're in AC3 recoding mode with configured sink, as this
							// would overwrite the AC3 format with the source codec format.
							if( !libavos_get_ac3_recoding_enabled() || !ac3_sink_configured ) {
								stream_audio_copy_sink_from_source( s );
							}
							// Set passthrough mode based on whether this route supports the sink format.
							int passthrough_mode = stream_audio_requested_passthrough_for_format( sink->format );
#ifdef CONFIG_SPDIF
							if(passthrough_mode && spdif_init(sink)) {
								DBG serprintf("stream_audio: regular passthrough enabled, mode=%d\n", passthrough_mode);
							}
#endif
							s->audio_sink->set_passthrough( s, passthrough_mode );
							if( !libavos_get_ac3_recoding_enabled() || !ac3_sink_configured ) {
								ac3_sink_configured = 0;
								ac3_reconfigure_pending = 1;
							}
							if( s->audio_sink->start( s ) ) {
								DBG serprintf("failed to restart audio sink after format change\n");
								s->audio_sink_open = 0;
							} else {
								s->audio_sink_open = 1;
								audio_format_configured = sink->format;
							}
						}
					}
				}

					int ac3_recode_output_frames = 1;
					// After the sink is configured for passthrough, wrap AC3 frames in IEC61937
					if( ac3_recoding && audio_frame.format == WAVE_FORMAT_AC3 && audio_frame.size > 0 ) {
						if( !ac3_sink_configured ) {
							ERR serprintf("stream_audio: AC3 sink not configured, dropping IEC61937 frame\n");
							audio_frame.size = 0;
						} else {
						DBG serprintf("stream_audio: wrapping AC3 recoded frame in IEC61937 (pre size=%d fakeSize=%d)\n",
							audio_frame.size, audio_frame.fakeSize);

							// Preserve the PCM-equivalent byte count before wrapping so we can keep
							// accurate timing after IEC encapsulation (spdif_get overwrites fakeSize).
							int pcm_fake_size = audio_frame.fakeSize;
							int pcm_channels = audio_frame.channels > 0 ? audio_frame.channels : original_channels;
							int pcm_bpf = pcm_channels > 0 ? pcm_channels * (int)sizeof(int16_t) : 0;
							if( pcm_fake_size > 0 && pcm_bpf > 0 ) {
								ac3_recode_output_frames = pcm_fake_size /
									(AC3_RECODE_FRAME_SAMPLES * pcm_bpf);
								if( ac3_recode_output_frames < 1 ) ac3_recode_output_frames = 1;
							}
							AUDIO_FRAME wrapped_frame = {0};
							int dummy_decoded = 0;
							AUDIO_PROPERTIES *spdif_props = stream_audio_get_sink_props( s );
							DBG2 serprintf("IEC wrap input: frame=%p size=%d pcm_fake=%d\n",
								audio_frame.data, audio_frame.size, pcm_fake_size);
							spdif_encapsulate_frames( spdif_props, audio_frame.data, audio_frame.size,
								&wrapped_frame, &dummy_decoded, ac3_recode_output_frames );

							if( wrapped_frame.size > 0 ) {
								DBG2 serprintf("IEC wrap output: wrapped_size=%d wrapped_fake=%d decoded=%d\n",
									wrapped_frame.size, wrapped_frame.fakeSize, dummy_decoded);
								audio_frame = wrapped_frame;

								// Restore fakeSize to the PCM-equivalent size calculated by the AC3 filter.
								if( pcm_fake_size > 0 ) {
									audio_frame.fakeSize = pcm_fake_size;
								}

							DBG serprintf("stream_audio: successfully wrapped AC3 in IEC61937 (post size=%d fakeSize=%d)\n",
								audio_frame.size, audio_frame.fakeSize);
							} else {
								ERR serprintf("stream_audio: FAILED to wrap AC3 in IEC61937, size=%d\n", wrapped_frame.size);

								if( pcm_fake_size > 0 ) {
									audio_frame.fakeSize = pcm_fake_size;
								}
							DBG serprintf("stream_audio: wrapped frame size=%d fakeSize=%d (dropping frame)\n",
								audio_frame.size, audio_frame.fakeSize);
							// Do not send raw AC3 frames when IEC encapsulation fails; wait for the muxer
							// to output a proper burst on the next iteration to avoid corrupt audio.
							audio_frame.size = 0;
						}
					}
				}
				// Prepare per-chunk audio time accounting (updated after write).
				// We compute the duration once here, but only advance audio_time once
				// we actually write to the sink, so output holds don't advance the clock.
				int bits_per_sample = (audio_frame.bits ? audio_frame.bits : original_bits);
				if (bits_per_sample == 0) bits_per_sample = 16;
				int bytes_per_sample = bits_per_sample / 8;
				int channels = audio_frame.channels ? audio_frame.channels : original_channels;
				if (channels == 0) channels = 2;
				int sample_rate = audio_frame.samplesPerSec ? audio_frame.samplesPerSec : original_rate;
				if (sample_rate == 0) sample_rate = 48000;

				// For A/V sync scaling, we need the PCM-equivalent duration of written data.
				// Mode 2 / AC3 recoding: fakeSize carries the PCM-equivalent payload size.
				// Mode 1 (IEC): each write IS one complete IEC burst whose duration equals
				int64_t bytes_per_sec = (int64_t)sample_rate * channels * bytes_per_sample;

				// slowly drain the audio data we have, while updating the audio time...
				int size = audio_frame.size;
				int total_size = audio_frame.size;
				int ac3_recode_packet_size = 0;
				int ac3_recode_fake_per_packet = 0;
				if( ac3_recoding && ac3_recode_output_frames > 0 &&
				    total_size % ac3_recode_output_frames == 0 ) {
					ac3_recode_packet_size = total_size / ac3_recode_output_frames;
					if( audio_frame.fakeSize > 0 ) {
						ac3_recode_fake_per_packet = audio_frame.fakeSize / ac3_recode_output_frames;
					}
				}
				int loop_write_count = 0;
				while( size > 0 ) {
					if( _abort( s ) ) {
						return;
					}
					// Startup A/V alignment: if audio is significantly ahead at the very
					// beginning or after a seek, delay audio output briefly so video can catch up.
					if( s->put_time_mode && s->video && s->video->valid &&
						s->audio_start_pending && s->audio_start_pts != STREAM_NO_PTS_VALUE &&
						!passthrough_active ) {
						if( s->audio_start_target_ts == STREAM_NO_PTS_VALUE ) {
							int anchor_delay = stream_get_anchor_delay_ms( s, 1 );
							int static_latency = s->audio_ctx ? audio_interface_get_latency( s->audio_ctx ) : 0;
							if( anchor_delay < 0 ) anchor_delay = 0;
							if( static_latency > anchor_delay ) {
								anchor_delay = static_latency;
							}
							// For seeking, we must account for the fact that video_time may already
							// be at the seek target.
							s->audio_start_target_ts = s->audio_start_pts - anchor_delay;
							// Guard against underflow
							if( s->audio_start_target_ts < 0 ) s->audio_start_target_ts = 0;

							DBG serprintf("startup_anchor_target: pts=%d video=%d anchor=%d static=%d target=%d\n",
								s->audio_start_pts, s->video_time, anchor_delay, static_latency, s->audio_start_target_ts);
						}

						// Only hold if we have a valid video time to compare against.
						if (s->video_time >= 0) {
							int diff = s->video_time - s->audio_start_target_ts;
							// Relax hold threshold for TrueHD (very high packet cadence) to avoid startup freeze.
							// TrueHD emits tiny 833us bursts; holding on each one creates a massive bottleneck.
							int hold_threshold = (audio_frame.format == WAVE_FORMAT_TRUEHD) ? -300 : -32;
							if( diff < hold_threshold ) {
								// Audio is too far ahead of its 'audible' start point relative to video.
								if (loop_write_count % 10 == 0) {
									DBG serprintf("startup_audio_hold: v=%d target=%d diff=%d fmt=%04X loop=%d\n",
										s->video_time, s->audio_start_target_ts, diff, audio_frame.format, loop_write_count);
								}
								msec_sleep( 10 );
								continue;
							}
						}
					}
					// Do not split compressed passthrough bursts. IEC61937 / raw codec frames
					// must reach AudioTrack atomically; chunking them into generic PCM-sized
					// writes can break HAL parsing and lead to dead/broken passthrough tracks.
					if( ac3_recoding && ac3_recode_packet_size > 0 ) {
						// Preserve one raw AC3 frame / IEC burst per AudioTrack write.
						audio_frame.size = MIN(ac3_recode_packet_size, size);
						audio_frame.fakeSize = ac3_recode_fake_per_packet;
					} else if( passthrough_active ) {
						audio_frame.size = size;
					} else {
						audio_frame.size = MIN( stream_audio_chunk * s->audio->channels, size );
					}

					// Internal video pacing alone cannot sustain large negative A/V offsets.
					// Apply user negative A/V delay as additional audio hold (silence insertion).
					if( s->put_time_mode && s->audio_sink ) {
						int passthrough = s->audio_sink->get_passthrough ? s->audio_sink->get_passthrough( s ) : 0;
						int target_ms = (s->av_delay < 0) ? -s->av_delay : 0;
						s->manual_audio_delay_target_ms = target_ms;
						if( !passthrough ) {
							if( s->manual_audio_delay_applied_ms < target_ms ) {
								int add_ms = target_ms - s->manual_audio_delay_applied_ms;
								DBG serprintf("manual_audio_delay_hold: av_delay=%d target=%d applied=%d add=%d\n",
									s->av_delay, target_ms, s->manual_audio_delay_applied_ms, add_ms);
								int inserted_ms = _wait( s, add_ms );
								// The inserted silence is a real wall-clock gap before the
								// next decoded frame reaches the video sink. Record it so
								// videosink_put_time() can treat that gap as intentional and
								// not reanchor (which would cancel the manual delay).
								s->manual_audio_hold_pending_ms += inserted_ms;
								s->manual_audio_delay_applied_ms += inserted_ms;
								if( s->manual_audio_delay_applied_ms > target_ms ) {
									s->manual_audio_delay_applied_ms = target_ms;
								}
							} else if( s->manual_audio_delay_applied_ms > target_ms ) {
								// We cannot pull already queued audio back in time; shrink target
								// so future holds follow the latest user setting.
								DBG serprintf("manual_audio_delay_reduce: av_delay=%d target=%d applied=%d\n",
									s->av_delay, target_ms, s->manual_audio_delay_applied_ms);
								s->manual_audio_delay_applied_ms = target_ms;
							}
						} else {
							s->manual_audio_delay_applied_ms = 0;
						}
					}

					// Audio lead gate: hold the producer when heard audio is materially
					// ahead of video. Applies to PCM and mode2 passthrough: audio_time
					// advances by logical (fakeSize-equivalent) duration, so the gate
					// correctly reflects queued logical audio, not raw byte capacity.
					// Normal IEC mode1 passthrough and AC3 recode are exempt (the gate
					// returns 0 for them); AC3 recode is paced by the wall-clock burst
					// rate limiter below instead.
					while( !_abort( s ) && stream_sync_pcm_audio_lead_gate( s, ac3_recoding ) ) {
							msec_sleep( 10 );
							stream_yield_RT();
					}
					if( _abort( s ) ) {
						return;
					}

					// AC3 recode wall-clock burst pacer (entry wait). AC3 recode reports
					// mode 1 but re-encodes frames freely: AudioTrack can_write blind-
					// latches (no real backpressure) and the heard playhead never goes
					// dynamic, so a sleep-gate either starves the compressed producer or
					// only relocates the steady-state error (avos-229/231/232/233).
					// Instead, pace burst writes against media wall time with a small,
					// bounded write-ahead. Exact just-in-time pacing made process-wide
					// stalls show up as 90ms receiver holes; a few bursts of headroom lets
					// the receiver ride through those stalls without allowing startup
					// runaway. The schedule is advanced after each successful write below.
					int ac3_pace_chunk_ms = 0;
					int ac3_pace_lead_ms = 0;
					if( ac3_recoding && passthrough_active ) {
						// AC3 encoder output is one 1536-sample frame at 48 kHz
						// (32 ms). Do not derive the cadence from fakeSize/source
						// bytes_per_sec: decoded input may be 44.1 kHz or have a
						// different channel count, which was advancing AC3 recode
						// timing by ~34.8 ms per burst instead of 32 ms.
						ac3_pace_chunk_ms = (int)(AC3_RECODE_FRAME_US / 1000);
						ac3_pace_lead_ms = ac3_pace_chunk_ms * AC3_RECODE_WRITE_AHEAD_BURSTS;
						s->ac3_recode_pacer_max_lead_ms = ac3_pace_lead_ms;
						if( s->ac3_recode_pacer_valid ) {
							int wait_ms = s->ac3_recode_next_write_wall_ms - ac3_pace_lead_ms - atime();
							while( wait_ms > 0 && !_abort( s ) ) {
								msec_sleep( wait_ms > 10 ? 10 : wait_ms );
								stream_yield_RT();
								wait_ms = s->ac3_recode_next_write_wall_ms - ac3_pace_lead_ms - atime();
							}
							if( _abort( s ) ) {
								return;
							}
						}
					}

					// no error, output PCM
					DBG3 serprintf("stream_audio: checking if sink can_write %d bytes\n", audio_frame.size);
					int can_write_retries = 0;
					while( !s->audio_sink->can_write( s, audio_frame.size ) ) {
						can_write_retries++;
						if (can_write_retries % 100 == 0) {
							DBG serprintf("stream_audio: sink->can_write still returning false after %d attempts\n",
								can_write_retries);
						}
						if( _abort( s ) ) {
							return;
						}
						stream_yield_RT();
					}
					if( _abort( s ) ) {
						return;
					}
					if( (s->paused || stream_audio_paused) && !s->play_n_audio_frames ) {
						DBG serprintf("stream_audio: pause raced before write, dropping pending audio frame (%d bytes)\n",
							audio_frame.size);
						size = 0;
						break;
					}
					DBG3 serprintf("stream_audio: calling sink->write with frame fmt=%04X size=%d\n",
						audio_frame.format, audio_frame.size);
					int ledger_reserved = 0;
					int ledger_reserved_frames = 0;
					int ledger_bpf = 0;
					if( use_atempo && !passthrough_active && !ac3_recoding &&
						bytes_per_sample > 0 && channels > 0 && sample_rate > 0 ) {
						ledger_bpf = bytes_per_sample * channels;
						ledger_reserved_frames = ledger_bpf > 0 ? audio_frame.size / ledger_bpf : 0;
						ledger_reserved = _stream_atempo_ledger_reserve(
							s, ledger_reserved_frames, sample_rate );
					}
					// Arm the one-shot reanchor before the write so the latch is ready,
					// but do not apply yet: audio_time must only be rebased after bytes
					// are confirmed committed (size_written > 0).
					if( s->audio_resume_pending ) {
						DBG serprintf("stream_audio: first audio output after resume (audio_time=%d video_time=%d seek_epoch=%d t=%d)\n",
							s->audio_time, s->video_time, s->seek_epoch, atime());
						stream_sync_pcm_reanchor_arm( s, passthrough_active );
						s->audio_resume_pending = 0;
					}
					int size_written = s->audio_sink->write( s, &audio_frame );
					DBG3 serprintf("stream_audio: sink->write returned %d\n", size_written);
					if( ledger_reserved ) {
						int written_frames = (size_written > 0 && ledger_bpf > 0) ?
							size_written / ledger_bpf : 0;
						_stream_atempo_ledger_finalize(
							s, ledger_reserved_frames, written_frames, sample_rate );
					}

					// For A/V sync scaling, we need the PCM-equivalent duration of written data.
					// Mode 2 / AC3 recoding: fakeSize carries the PCM-equivalent payload size.
					// Mode 1 (IEC): each write IS one complete IEC burst whose duration equals
					// the container rate (e.g. 32ms for 48kHz EAC3).
					int64_t effective_chunk_size = size_written;
					if ((passthrough_active && passthrough != 1) || ac3_recoding) {
						if (audio_frame.fakeSize > 0) {
							// Scaled proportional to actual write size to handle partial writes correctly
							effective_chunk_size = (int64_t)(((int64_t)size_written * audio_frame.fakeSize) / audio_frame.size);
						}
					}

					loop_write_count++;
					if( _stream_audio_speed_diag_active( s ) ) {
						int sink_delay = s->audio_ctx ? audio_interface_get_delay( s->audio_ctx ) : -1;
						int atempo_delay = (s->audio_filter_atempo && s->audio_filter_atempo->delay) ?
							s->audio_filter_atempo->delay( s->audio_filter_atempo ) : 0;
						DBG serprintf("audio_write_diag: epoch=%d speed=%.3f atempo=%d req=%d wrote=%d total=%d effective=%lld bps=%lld audio_time=%d video_time=%d sink_delay=%d last_good=%d\n",
							s->audio_speed_diag_epoch, audio_interface_get_audio_speed(),
							atempo_delay, audio_frame.size, size_written, total_size,
							(long long)effective_chunk_size, (long long)bytes_per_sec, s->audio_time,
							s->video_time, sink_delay, s->last_good_delay_ms);
						s->audio_speed_diag_writes_left--;
					}
					if (startup_write_log_count < 5) {
						DBG2 serprintf("startup_write[%d]: before_time=%d video=%d fmt=%04X req=%d wrote=%d effective_size=%lld bytes_per_sec=%lld ref=%d start_pending=%d resume_pending=%d\n",
							startup_write_log_count, s->audio_time, s->video_time, audio_frame.format,
							audio_frame.size, size_written, (long long)effective_chunk_size, (long long)bytes_per_sec,
							s->audio_ref_time, s->audio_start_pending, s->audio_resume_pending);
						startup_write_log_count++;
					}

					if( _abort( s ) ) {
						return;
					}
					if( size_written <= 0 ) {
						DBG serprintf("stream_audio: write failed (%d), dropping remainder\n", size_written);
						if( s->video_hold_for_resume_audio ) {
							DBG serprintf("stream_audio: releasing video hold on write failure (pt=%d recode=%d)\n",
								passthrough_active, ac3_recoding);
							s->video_hold_for_resume_audio = 0;
						}
						size = 0;
						break;
					}
					// AC3 recode wall-clock burst pacer (schedule advance). Keep an ideal
					// media-time write cursor and allow a bounded lead at entry. Do not
					// rebase on normal 50-100ms process stalls: catch up into the allowed
					// headroom instead. Rebase only after a large discontinuity such as
					// pause/seek/long stall.
					if( ac3_recoding && passthrough_active && ac3_pace_chunk_ms > 0 ) {
						int now_ms = atime();
						int old_next_ms = s->ac3_recode_next_write_wall_ms;
						int late_ms = s->ac3_recode_pacer_valid ? now_ms - old_next_ms : 0;
						int rebase_ms = MAX( 200, ac3_pace_lead_ms * 3 );
						if( !s->ac3_recode_pacer_valid || late_ms > rebase_ms ) {
							s->ac3_recode_next_write_wall_ms = now_ms + ac3_pace_chunk_ms;
						} else {
							s->ac3_recode_next_write_wall_ms += ac3_pace_chunk_ms;
						}
						s->ac3_recode_pacer_valid = 1;
						DBG2 serprintf("ac3_recode_pacer: chunk_ms=%d lead_ms=%d cur_lead=%d now=%d old_next=%d next=%d late=%d fake=%d bps=%lld\n",
							ac3_pace_chunk_ms, ac3_pace_lead_ms,
							s->ac3_recode_next_write_wall_ms - now_ms,
							now_ms, old_next_ms,
							s->ac3_recode_next_write_wall_ms, late_ms,
							audio_frame.fakeSize, (long long)bytes_per_sec);
					}
					// Apply one-shot reanchor now that bytes are confirmed committed.
					int rw_audio_time_before = s->audio_time;
					stream_sync_pcm_reanchor_update( s, passthrough_active );
					if (resume_write_log_count < 3) {
						int rw_dyn_delay = s->audio_ctx ? audio_interface_get_delay( s->audio_ctx ) : -1;
						int rw_dyn_valid = s->audio_ctx ? audio_interface_is_delay_valid( s->audio_ctx ) : 0;
						int rw_static_lat = s->audio_ctx ? audio_interface_get_latency( s->audio_ctx ) : 0;
						DBG serprintf("resume_write[%d]: audio_before=%d audio_after=%d video=%d reanchor_src=%d reanchor_delay=%d last_good=%d last_good_valid=%d static_lat=%d dyn_valid=%d dyn_delay=%d atempo_in=%d atempo_out=%d atempo_fifo=%d wrote=%d zero_time_ms=%d\n",
							resume_write_log_count,
							rw_audio_time_before, s->audio_time, s->video_time,
							s->pcm_reanchor_source, s->pcm_reanchor_delay_ms,
							s->last_good_delay_ms, s->last_good_delay_valid,
							rw_static_lat, rw_dyn_valid, rw_dyn_delay,
							dbg_atempo_in, dbg_atempo_out, dbg_atempo_fifo,
							size_written, zero_time);
						resume_write_log_count++;
					}
					if( s->video_hold_for_resume_audio ) {
						DBG serprintf("stream_audio: first resumed audio write committed (%d bytes, pt=%d recode=%d)\n",
							size_written, passthrough_active, ac3_recoding);
						s->video_hold_for_resume_audio = 0;
					}
					if( (passthrough_active || ac3_recoding) && size_written < audio_frame.size ) {
						DBG serprintf("stream_audio: passthrough short write %d/%d fmt=%04X pt=%d recode=%d, dropping burst remainder\n",
							size_written, audio_frame.size, audio_frame.format, passthrough_active, ac3_recoding);
						size = 0;
					}
					if( s->audio_start_pending && s->audio_start_pts != STREAM_NO_PTS_VALUE ) {
						int anchor_delay = stream_get_anchor_delay_ms( s, 1 );
						int static_latency = s->audio_ctx ? audio_interface_get_latency( s->audio_ctx ) : 0;
						if( anchor_delay < 0 ) anchor_delay = 0;
						if( static_latency > anchor_delay ) {
							anchor_delay = static_latency;
						}
						// Anchor to first audio PTS. This represents 'generation time'.
						// The sync machinery (heard_ts) will subtract latency to find 'heard time'.
						int start_time = s->audio_start_pts;
						if( start_time < 0 ) start_time = 0;
						// For passthrough in put_time mode: set audio_time = video_time + latency
						// so heard_ts aligns to video_time immediately. Also reset sched anchors so
						// the next put_time sees no_sched=1 and reanchors correctly regardless of
						// any earlier no_sched reanchor that fired before this commit.
						// Passthrough frames are atomic bursts and cannot be held like PCM; alignment
						// must be done via audio_time here (PCM uses startup_audio_hold instead).
						// Restores android_sync=0 behavior removed during refactoring.
						if( s->put_time_mode && passthrough_active && s->video_time >= 0 && anchor_delay > 0 ) {
							start_time = s->video_time + anchor_delay;
							sfdec2_refresh_sched_anchor( s );
						}

						DBG serprintf("startup_anchor_commit: pts=%d video=%d anchor=%d static=%d start=%d put_time=%d\n",
							s->audio_start_pts, s->video_time, anchor_delay, static_latency, start_time, s->put_time_mode);

						_set_audio_time( s, start_time );
						s->audio_start_pending = 0;
						s->audio_start_target_ts = STREAM_NO_PTS_VALUE;
						DBG serprintf("audio_start_commit: audio_time=%d ref=%d samples=%d\n",
							s->audio_time, s->audio_ref_time, s->audio_samples);
					}
					// Update audio time based on actual written output (not decoded bytes).
					if( s->sync_mode != STREAM_SYNC_SAMPLES ) {
						int64_t chunk_time_us = 0;
						int audio_time_before_step = s->audio_time;

						// Byte-ratio timing is valid for PCM-like fixed-rate outputs.
						// Keep VBR and incomplete-metadata paths on conservative fallback timing.
						int use_rational_timing = (!s->audio->vbr &&
							audio_frame.size > 0 &&
							bytes_per_sample > 0 &&
							channels > 0 &&
							sample_rate > 0 &&
							bytes_per_sec > 0 &&
							total_size > 0);

						if( ac3_recoding && passthrough_active ) {
							chunk_time_us = _stream_ac3_recode_written_duration_us(
								size_written, audio_frame.size );
						} else if( use_rational_timing ) {
							chunk_time_us = ((int64_t)effective_chunk_size * 1000000) / bytes_per_sec;
						} else if( s->audio->bytesPerSec > 0 ) {
							chunk_time_us = ((int64_t)effective_chunk_size * 1000000) / s->audio->bytesPerSec;
						}

						if( chunk_time_us > 0 ) {
							int64_t remainder_before = s->audio_time_remainder_us;
							s->audio_time_remainder_us += chunk_time_us;
							int add_ms = (int)(s->audio_time_remainder_us / 1000);
							s->audio_time_remainder_us %= 1000;

							if( add_ms > 0 ) {
								if( _stream_audio_time_diag_active( s ) ) {
									int atempo_delay = (s->audio_filter_atempo && s->audio_filter_atempo->delay) ?
										s->audio_filter_atempo->delay( s->audio_filter_atempo ) : 0;
									DBG serprintf("audio_time_step: epoch=%d speed=%.3f chunk_us=%lld remainder_before=%lld remainder_after=%lld add_ms=%d size_written=%d total=%d atempo=%d before=%d video=%d loop=%d\n",
										s->audio_speed_diag_epoch, audio_interface_get_audio_speed(),
										(long long)chunk_time_us, (long long)remainder_before,
										(long long)s->audio_time_remainder_us, add_ms, size_written, total_size,
										atempo_delay, s->audio_time, s->video_time, loop_write_count);
								}
								if( use_atempo ) {
									_add_audio_time( s, add_ms );
								} else {
									_add_audio_time( s, RST_TO_TS_DELTA(add_ms, int) );
								}
							}
							if( passthrough_active && passthrough >= 2 ) {
								static int mode2_write_diag_last_wall = 0;
								int now_ms = atime();
								if( now_ms > mode2_write_diag_last_wall + 2000 ) {
									AUDIO_PROPERTIES *spdif_props = stream_audio_get_sink_props( s );
									int duration_bpf = spdif_props ? spdif_props->bytesPerFrame : 0;
									int duration_rate = spdif_props ? spdif_props->samplesPerSec : 0;
									int latency = s->audio_ctx ? audio_interface_get_latency( s->audio_ctx ) : 0;
									int heard_latency = latency;
									int raw_heard = s->audio_time - heard_latency;
									int heard = stream_get_heard_audio_ts( s, s->audio_time );
									int diff = STREAM_NO_PTS_VALUE;
									if( s->sync_v_time != STREAM_NO_PTS_VALUE ) {
										diff = s->sync_v_time - heard;
									}
									mode2_write_diag_last_wall = now_ms;
									DBG serprintf("mode2_write_timeline: wall=%d fmt=%04X pt=%d recode=%d req=%d wrote=%d fake=%d effective=%lld chunk_us=%lld dur_bpf=%d dur_rate=%d add_ms=%d before=%d after=%d heard=%d raw_heard=%d video=%d sync_v=%d diff=%d latency=%d\n",
										now_ms, audio_frame.format, passthrough, ac3_recoding,
										audio_frame.size, size_written, audio_frame.fakeSize,
										(long long)effective_chunk_size, (long long)chunk_time_us,
										duration_bpf, duration_rate,
										add_ms, audio_time_before_step, s->audio_time,
										heard, raw_heard, s->video_time, s->sync_v_time, diff, heard_latency);
								}
							}
						}
					}

					if( size_written > 0 && s->sync_mode == STREAM_SYNC_SAMPLES && audio_frame.size && s->audio_ref_time != -1 ) {
						// add the samples and calc new time
						if( s->audio->samplesPerSec ) {
							int bpf = s->audio->bytesPerFrame;
#ifdef CONFIG_SPDIF
							if( passthrough_active && audio_frame.fakeSize > 0 ) {
								AUDIO_PROPERTIES *spdif_props = stream_audio_get_sink_props( s );
								// CRITICAL: When using fakeSize, we MUST use sink bytesPerFrame (not source)
								// fakeSize was calculated in spdif_decode using sink properties (2ch 16bit = 4 bytes)
								// Source bytesPerFrame may differ (e.g., 6ch = 12 bytes), causing 3x sync drift
								if( spdif_props && spdif_props->bytesPerFrame > 0 ) {
									bpf = spdif_props->bytesPerFrame;
								} else {
									// Fallback: use 2ch 16bit (4 bytes) which is standard for passthrough
									bpf = 4;
									DBG serprintf("stream_audio EAC3_SYNC: WARNING! spdif_props->bytesPerFrame=%d, using fallback=4\n",
										spdif_props ? spdif_props->bytesPerFrame : 0);
								}
								DBG serprintf("stream_audio EAC3_SYNC: passthrough_active=%d fakeSize=%d size_written=%d bpf=%d source_bpf=%d format=%04X\n",
									passthrough_active, audio_frame.fakeSize, size_written, bpf, s->audio->bytesPerFrame, s->audio->format);
							}
#endif
							// Mode 1 IEC: the write IS the full IEC burst; use size_written so the
							// clock advances by the complete burst duration (e.g. 20 ms for TrueHD,
							// 32 ms for EAC3).  fakeSize is a per-subframe unit and under-counts by
							// up to 96x.  Mode 2 uses fakeSize as the logical PCM-equivalent duration.
							// AC3 recoding emits fixed AC3 encoder frames: 1536 samples at 48 kHz.
							int pt_bytes = size_written;
							if( ac3_recoding ) {
								int ac3_samples = AC3_RECODE_FRAME_SAMPLES;
								if( size_written > 0 && audio_frame.size > 0 && size_written < audio_frame.size ) {
									ac3_samples = (int)(((int64_t)AC3_RECODE_FRAME_SAMPLES * size_written) /
										audio_frame.size);
								}
								pt_bytes = ac3_samples;
							} else if (passthrough_active && passthrough != 1 && audio_frame.fakeSize > 0) {
								pt_bytes = audio_frame.fakeSize;
							}
							s->audio_samples += ac3_recoding ? pt_bytes : pt_bytes / bpf;
							// Use actual source sample rate for sync when passthrough is inactive.
							// When decoding to PCM, use decoded frame rate if available (most reliable),
							// else fallback to the currently configured rate.
							// For IEC mode 1, samples are counted at container rate (e.g. 192 kHz)
							// so we must also divide by the container rate, not the content rate.
							int sync_rate = s->audio->samplesPerSec;
							if (!passthrough_active && audio_frame.samplesPerSec > 0) {
								sync_rate = audio_frame.samplesPerSec;
							} else if (passthrough_active && passthrough == 1 && !ac3_recoding &&
								audio_frame.samplesPerSec > 0) {
								sync_rate = audio_frame.samplesPerSec; // container rate (192 kHz)
							} else if( ac3_recoding ) {
								sync_rate = AC3_RECODE_SAMPLE_RATE;
							}
							int delta = (UINT64)1000 * (UINT64)s->audio_samples / (UINT64)sync_rate;
							int prev_audio_time = s->audio_time;

							// Check if atempo filter is active - output samples are already in TS domain (physical time)
							int use_atempo = (s->audio_filter_atempo != NULL);
							if (!audio_interface_is_audio_speed_enabled() || !audio_interface_is_using_atempo()) {
								use_atempo = 0;
							}
							if (passthrough_active || ac3_recoding) {
								use_atempo = 0;
							}
							if( use_atempo ) {
								// Atempo rewrites PCM before AudioTrack, so the written sample count
								// already represents TS-domain physical duration.
								_set_audio_time( s, s->audio_ref_time + delta );
								DBG serprintf("stream_audio SAMPLES: atempo active, audio_time = %d + %d (no scaling)\n",
									s->audio_ref_time, delta);
							} else {
								// Plain PCM and AudioTrack PlaybackParams write unmodified media
								// samples, so convert their RST-duration clock into TS.
								_set_audio_time( s, s->audio_ref_time + RST_TO_TS_DELTA(delta, int) );
								DBG serprintf("stream_audio SAMPLES: normal, audio_time = %d + RST_TO_TS(%d)\n",
									s->audio_ref_time, delta);
							}
							if( passthrough == 1 ) {
								DBG serprintf("pt_mode1_samples_clock: fake=%d pt_bytes=%d size_written=%d audio_samples=%lld bpf=%d sync_rate=%d delta_ms=%d ref=%d prev=%d now=%d format=%04X\n",
									audio_frame.fakeSize, pt_bytes, size_written, (long long)s->audio_samples,
									bpf, sync_rate, delta, s->audio_ref_time, prev_audio_time,
									s->audio_time, s->audio->format);
							}
							DBG serprintf("stream_audio SAMPLES: audio_time update prev=%d now=%d using_atempo=%d speed=%.3f delta_ms=%d sync_rate=%d configured_rate=%d\n",
								prev_audio_time, s->audio_time, use_atempo, audio_interface_get_audio_speed(), delta, sync_rate, s->audio->samplesPerSec);
							// if size_written < size, we don't want to go out of sync on passthrough
							audio_frame.fakeSize = 0;
						}
					}

					size             -= size_written;
					audio_frame.data += size_written;
				}

				if( using_pcm_accum ) {
					s->pcm_accum_size = 0;
				}

				if( s->audio_sink->syncable( s ) && s->audio_sink->can_write( s, size ) ) {
					if( !_abort( s ) ) {
						s->audio_yield = 0;
						return;
					}
				}
			
			} else if( s->audio_end ) {
				// end, signal to the sink that we are finished
				_pcm_accum_flush_to_sink( s );
				s->audio_sink->end( s );
			} else if( audio_frame.error == STREAM_ERROR_FATAL ) {
				// fatal error, we need to stop
				s->video_error           = VE_ERROR;
				s->video_error_qualifier = VEQ_AUDIO_PROFILE_AND_LEVEL_UNSUPPORTED;
			}
	}
	return;
}

// ************************************************************
//
//	stream_audio_dec_thread
//
// ************************************************************
void *stream_audio_dec_thread( void *data )
{
	STREAM *s = (STREAM *)data;
	float last_audio_speed = audio_interface_get_audio_speed();
DBGS serprintf("PID[%5d] stream_audio_thread::Starting\r\n", getpid() );

	// Reset AC3 sink configuration flag for new playback session
	ac3_sink_configured = 0;
	startup_anchor_log_count = 0;
	startup_write_log_count = 0;
	atempo_gate_log_count = 0;
	resume_write_log_count = 0;
	resume_seen_pending = 0;
	s->pcm_accum_size = 0;
	// Initialize with current format to prevent redundant reconfiguration on first audio thread loop iteration.
	// The sink was already configured by start() before the audio thread began, so we use the current format
	// to avoid a redundant set_passthrough call that would recreate the AudioTrack unnecessarily.
	audio_format_configured = stream_audio_get_sink_props( s )->format;

	while( thread_state_get( &s->audio_tstate ) != THREAD_EXIT ) {
		float current_audio_speed = audio_interface_get_audio_speed();
		if( fabsf(current_audio_speed - last_audio_speed) > 0.001f ) {
			if( s->pcm_accum_size > 0 ) {
				DBG serprintf("stream_audio: pcm_accum reset on speed change %.3f->%.3f (pending=%d)\n",
					last_audio_speed, current_audio_speed, s->pcm_accum_size);
				s->pcm_accum_size = 0;
			}
			last_audio_speed = current_audio_speed;
		}

		AUDIO_PROPERTIES *sink = stream_audio_get_sink_props( s );
		if( sink && sink->format != audio_format_configured ) {
			DBG serprintf("stream_audio thread: sink format changed old=%04X new=%04X src_fmt=%04X ac3=%d configured=%d pending=%d pt=%d\n",
				audio_format_configured, sink->format, s->audio ? s->audio->format : 0,
				libavos_get_ac3_recoding_enabled(), ac3_sink_configured,
				ac3_reconfigure_pending, s->audio_sink ? s->audio_sink->get_passthrough(s) : -1);
			if( s->pcm_accum_size > 0 ) {
				DBG serprintf("stream_audio: pcm_accum reset on sink format change (%04X->%04X, pending=%d)\n",
					audio_format_configured, sink->format, s->pcm_accum_size);
				s->pcm_accum_size = 0;
			}
#ifdef CONFIG_SPDIF
			// In AC3 recoding mode with configured sink, keep it pinned to AC3.
			// Only update audio_format_configured to stop re-detecting this as a format change.
			if(libavos_get_ac3_recoding_enabled() && ac3_sink_configured &&
			   sink->format == WAVE_FORMAT_AC3) {
				// Sink is pinned to AC3, just record the format for next iteration
				audio_format_configured = WAVE_FORMAT_AC3;
				DBG serprintf("AC3 recoding: sink pinned to AC3 (source format=%04X)\n",
					s->audio->format);
				goto skip_format_change;
			}
#endif

#ifdef CONFIG_SPDIF
			if(libavos_get_ac3_recoding_enabled()) {
				// AC3 recoding: waiting for _audio_decode to configure sink to AC3.
				// Don't update audio_format_configured - let _audio_decode set it.
				DBG serprintf("AC3 recoding: waiting for AC3 sink setup (source=%04X, sink=%04X)\n",
					s->audio->format, sink->format);
			} else {
				int passthrough_mode = stream_audio_requested_passthrough_for_format( sink->format );
				if(passthrough_mode && spdif_init(sink) && s->audio_sink) {
					// Only call spdif_init if passthrough is actually enabled to avoid unnecessary side effects
					s->audio_sink->set_passthrough(s, passthrough_mode );
					audio_format_configured = sink->format;
				} else
#endif
			{
				s->audio_sink->set_passthrough(s, 0);
				audio_format_configured = sink->format;
			}
#ifdef CONFIG_SPDIF
			}
#endif
		}
skip_format_change:

		thread_state_ack( &s->audio_tstate );
		s->audio_yield = 1;
		if( thread_state_get( &s->audio_tstate ) == THREAD_RUNNING ) {
			_audio_decode( s );
		}
		
		if( s->audio_yield ) {
			stream_yield_RT();
		}
	}
	if( s->pcm_accum_data ) {
		afree( s->pcm_accum_data );
		s->pcm_accum_data = NULL;
	}
	s->pcm_accum_capacity = 0;
	s->pcm_accum_size = 0;
DBGS serprintf("PID[%5d] stream_audio_thread::Exiting\r\n", getpid() );	
 	return NULL;
}

#ifdef DEBUG_MSG
#include <stdlib.h>

static void _zero_time( int argc, char *argv[] )
{ 
	if( argc > 1 ) {
		zero_time = atoi( argv[1] );
	}
serprintf("zero_time: %d\r\n", zero_time ); 
}

static void _audio_chunk( int argc, char *argv[] )
{ 
	if( argc > 1 ) {
		stream_audio_chunk = atoi( argv[1] );
	}
serprintf("stream_audio_chunk: %d\r\n", stream_audio_chunk );
}

void *AV_get_ctx( void );

static void _audio_singlestep( int argc, char *argv[] ) 
{
	STREAM *s = AV_get_ctx();

	if( !s )
		return;

	s->play_n_audio_frames = ((argc > 1) ? atoi(argv[1]) : 1 );
serprintf("\r\n");
}

DECLARE_DEBUG_COMMAND("szti", _zero_time );
DECLARE_DEBUG_COMMAND("sac",  _audio_chunk );
DECLARE_DEBUG_COMMAND("sa",   _audio_singlestep );
DECLARE_DEBUG_PARAM("ac3_force_mode2", ac3_force_mode2 );
DECLARE_DEBUG_PARAM("ac3_mode2_plain_policy", ac3_mode2_plain_policy );
DECLARE_DEBUG_PARAM("ac3_mode2_force_pipeline", ac3_mode2_force_pipeline );


#endif

#endif

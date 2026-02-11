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
#include "util.h"
#include "audio_interface.h"
#include "stream.h"
#include "stream_sync.h"

#ifdef CONFIG_ANDROID
int get_android_sync(void);
#endif

#ifdef CONFIG_AUDIO_AC3
extern int libavos_get_ac3_recoding_enabled(void);
#endif

#ifdef CONFIG_STREAM

#define DBGS	DBG_IF(Debug[DBG_STREAM])
#define DBGV   	DBG_IF(Debug[DBG_VID])
#define DBGVY	DBG_IF(Debug[DBG_VID]||Debug[DBG_SYNC])
#define DBGY	DBG_IF(Debug[DBG_SYNC])
#define DBGV1  	DBG_IF(Debug[DBG_VID] == 1)
#define DBGV2  	DBG_IF(Debug[DBG_VID] > 1)
#define DBGV3 	DBG_IF(Debug[DBG_VID] > 2)

#define DBG DBG_IF(Debug[DBG_SYNC])

extern int stream_max_delay;
extern int stream_no_sync;
extern int stream_video_paused;
extern int stream_audio_paused;
extern int stream_bdrop_threshold;
extern int stream_pdrop_threshold;

static volatile int	stream_dbg_delay = 0;

static int stream_use_xbmc_smoothing = 0;

static int stream_calc_lwma(int current, int *history, int *count)
{
	if (*count < 3) {
		history[*count] = current;
		(*count)++;
	} else {
		history[0] = history[1];
		history[1] = history[2];
		history[2] = current;
	}

	long long sum = 0;
	int i;
	for (i = 0; i < *count; i++) {
		sum += (long long)(i + 1) * history[i];
	}

	int n = *count;
	if (n == 0) return current;

	return (int)(sum * 2 / (n * (n + 1)));
}

// ************************************************************
//
//	stream_sync_restart
//
// ************************************************************
int stream_sync_restart( STREAM *s )
{
	s->delay         = 0;
	s->delay_valid   = 0;
	s->last_good_delay_ms = 0;
	s->last_good_delay_valid = 0;
	s->last_good_atempo_delay_ms = 0;
	s->drop          = 0;
	s->drop_P        = 0;
	s->drop_B        = 0;
	
	s->delay_history_count = 0;
	s->av_delay_history_count = 0;

	return 0;
}

// ************************************************************
//
//	stream_get_heard_audio_ts
//
// ************************************************************
static int _get_anchor_delay_ms(STREAM *s, int *valid, int allow_static)
{
	int delay_valid = s->audio_ctx ? audio_interface_is_delay_valid(s->audio_ctx) : 1;
	int delay = stream_sync_av_delay(s);
	int anchor_valid = delay_valid;

#ifdef CONFIG_ANDROID
	if (!delay_valid) {
		if (s->last_good_delay_valid) {
			delay = s->last_good_delay_ms;
			if( !get_android_sync() ) {
				// android_sync=0: keep last-good delay as a usable anchor when timing drops invalid
				// during steady playback. This avoids sudden loss of latency compensation.
				anchor_valid = 1;
			}
			// TODO: consider enabling last-good anchoring for android_sync=1 to keep latency
			// consistent when timing goes invalid, but this risks regressions from anchoring on
			// stale delay (visible catch-up bursts on some devices).
		} else if (allow_static && s->audio_ctx) {
			int static_latency = audio_interface_get_latency(s->audio_ctx);
			if (static_latency > 0) {
				delay = static_latency;
			}
		}
	}
#else
	(void)allow_static;
#endif
	if (valid) {
		*valid = anchor_valid;
	}
	return anchor_valid ? delay : 0;
}

int stream_get_anchor_delay_ms( STREAM *s, int allow_static )
{
	return _get_anchor_delay_ms( s, NULL, allow_static );
}

static int _stream_get_heard_audio_ts_internal( STREAM *s, int fallback_ts )
{
	if( !s || !s->audio || !s->audio->valid || s->audio_time < 0 ) {
		return fallback_ts;
	}
#ifdef CONFIG_ANDROID
#else
	int allow_static = 0;
#endif
#ifdef CONFIG_ANDROID
	int allow_static = 1;
#endif

	int delay_valid = s->audio_ctx ? audio_interface_is_delay_valid( s->audio_ctx ) : 1;
	int suppress_static_heard_delay = 0;
	int anchor_delay = (s->smoothed_av_delay >= 0) ? s->smoothed_av_delay :
		_get_anchor_delay_ms(s, NULL, allow_static);
#ifdef CONFIG_ANDROID
	// Late-audio start guard for static delay: avoid subtracting static latency
	// when audio starts significantly after video at the very beginning.
	if( delay_valid && s->put_time_mode && s->audio_ctx &&
		s->sync_v_time >= 0 && s->sync_v_time < 1000 ) {
		int audio_lead = s->audio_time - s->sync_v_time;
		int static_latency = audio_interface_get_latency( s->audio_ctx );
		if( static_latency > 0 && s->smoothed_av_delay == static_latency && audio_lead > 150 ) {
			delay_valid = 0;
			anchor_delay = 0;
			suppress_static_heard_delay = 1;
		}
	}
#endif
	int heard_delay = anchor_delay;
	if (!delay_valid) {
		// Use raw delay (playhead/static) for heard-time only; do not anchor sync.
		heard_delay = s->audio_ctx ? audio_interface_get_delay( s->audio_ctx ) : 0;
#ifdef CONFIG_ANDROID
		// When atempo is active, include filter delay in heard-time even if timing is invalid.
		// Otherwise speed changes can anchor without accounting for the atempo pipeline latency.
		if( audio_interface_is_audio_speed_enabled() && audio_interface_is_using_atempo() ) {
			int chain_delay = stream_sync_av_delay( s );
			if( chain_delay > heard_delay ) {
				heard_delay = chain_delay;
			}
		}
#endif
#ifdef CONFIG_ANDROID
		// Startup grace: if timing is invalid at the very start, include static latency
		// in heard-time to avoid large initial A/V offset.
		if( s->put_time_mode && s->audio_time > 0 && s->sync_v_time >= 0 &&
			s->sync_v_time < 500 && s->audio_ctx ) {
			int static_latency = audio_interface_get_latency( s->audio_ctx );
			if( static_latency > heard_delay ) {
				heard_delay = static_latency;
			}
		}
		if( suppress_static_heard_delay ) {
			// Dynamic delay is disabled and audio starts late: avoid applying static latency
			// to heard-time during startup to prevent large A/V offset.
			heard_delay = 0;
		}
#endif
	}
DBGY	serprintf("heard_ts_delay: audio_time=%d smoothed=%d delay_valid=%d anchor_delay=%d heard_delay=%d av_delay=%d ctx=%p\n",
		s->audio_time, s->smoothed_av_delay, delay_valid, anchor_delay, heard_delay, s->av_delay, s->audio_ctx);

	int heard_ts = s->audio_time - heard_delay - RST_TO_TS_DELTA( s->av_delay, int );
	if( heard_ts < 0 ) {
		heard_ts = 0;
	}

DBGY	serprintf("heard_ts_calc: audio_time=%d heard_ts=%d\n", s->audio_time, heard_ts);
	return heard_ts;
}

int stream_get_heard_audio_ts( STREAM *s, int fallback_ts )
{
	return _stream_get_heard_audio_ts_internal( s, fallback_ts );
}

// ************************************************************
//
//	stream_sync_init
//
// ************************************************************
int stream_sync_init( STREAM *s, int time )
{
	char hms_buf[32];
	DBG serprintf("stream_sync_init(time = %d (%s))\n", time, ms_to_hms_string(time, hms_buf, sizeof(hms_buf)));

	s->video_time     = -1;
	s->audio_time     = -1;
	s->audio_ref_time = -1;
	s->audio_start_pending = 0;
	s->audio_start_pts = STREAM_NO_PTS_VALUE;
	s->audio_start_target_ts = STREAM_NO_PTS_VALUE;
	s->smoothed_av_delay = -1;
	s->last_good_delay_ms = 0;
	s->last_good_delay_valid = 0;
	s->last_good_atempo_delay_ms = 0;
	s->warmup_video_frames = 0;

	if( s->video->valid ) {
		s->video_time = time;
	} else if( s->sync_mode != STREAM_SYNC_SAMPLES ) {
		s->audio_time = time;
	}

	if( s->video->valid && s->audio->valid && !s->slideshow ) {
DBGS serprintf("sync_init\r\n");
 		s->sync_audio   =  1;
		s->sync_video   =  1;
		s->sync_v_time  = -1;
		s->sync_a_time  = -1;
	}

	stream_sync_restart( s );

	return 0;
}

// *****************************************************************************
//
//	_stream_sync_av_delay
//	returns delay in ms in real world domain unscaled by audio speed
//	it contains all hardware delays in the audio and video chain
//
// *****************************************************************************
int stream_sync_av_delay( STREAM *s )
{
	// Defensive check: validate stream pointer and audio/video validity to prevent crashes
	if (!s || !s->audio || !s->video) {
		return 0;
	}

	// this returns delta(audio_delay - video_delay) in ms in real world domain (i.e. ts delta)
	if ( !s->audio->valid || !s->video->valid ) {
		// if we have no audio & video
		return 0;
	}

	// Additional safety: check if sinks are being torn down
	if (s->audio_sink && !s->audio_sink_open) {
		return 0;
	}
	if (s->video_sink && !s->video_sink->is_open) {
		return 0;
	} 
	
	// audio data passes through decoder, filters, and sink
	// world time audio decoder delay not dependant on audio speed
	int codec_delay = s->audio_dec ? s->audio_dec->delay( s->audio ) : 0;

	// world time audio filter delay (sum of all active filters) not dependant on audio speed
	// Only count delays from filters that are actually being applied
	int filter_delay = 0;
	int passthrough = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
	int ac3_recoding = 0;
#ifdef CONFIG_AUDIO_AC3
	ac3_recoding = libavos_get_ac3_recoding_enabled();
#endif

	// atempo filter runs independently of other filters (controls playback speed)
	int atempo_delay = 0;
	int use_atempo = (s->audio_filter_atempo != NULL);
	if (!audio_interface_is_audio_speed_enabled() || !audio_interface_is_using_atempo()) {
		use_atempo = 0;
	}
	if( use_atempo && s->audio_filter_atempo->delay ) {
		atempo_delay = s->audio_filter_atempo->delay( s->audio_filter_atempo );
		filter_delay += atempo_delay;
	}
	DBGY serprintf("stream_sync_av_delay: atempo_delay=%d filter_atempo=%p delay_fn=%p enabled=%d\n",
		atempo_delay, s->audio_filter_atempo,
		s->audio_filter_atempo ? s->audio_filter_atempo->delay : NULL, use_atempo);

	// Filters run in: normal PCM mode OR AC3 recoding mode (all formats)
	int run_filter = (!passthrough || ac3_recoding);
	if( run_filter ) {
		// Sum delays from filters that are actually applied
		if( s->audio_filter_compress && s->audio_filter_compress->delay ) {
			filter_delay += s->audio_filter_compress->delay( s->audio_filter_compress );
		}
		if( s->audio_filter_ac3 && s->audio_filter_ac3->delay ) {
			filter_delay += s->audio_filter_ac3->delay( s->audio_filter_ac3 );
		}
		if( s->audio_filter && s->audio_filter->delay ) {
			filter_delay += s->audio_filter->delay( s->audio_filter );
		}
	}

	// world time audio sink delay (audiotrack system_delay on android) not dependant on audio speed
	int sink_delay = s->audio_sink ? s->audio_sink->delay( s ) : 0;
	// wold time video sink delay not dependant on audio speed
	int video_delay;
	if( s->vtime_post_sink ) {
		// we sample after the sink, so the time stamps are the one we get out of the sink
		video_delay = 0;
	} else {
		// we sample before the sink, so the video frames have to pass through the sink
	  	video_delay = ( s->video_sink && s->video_sink->delay )  ? s->video_sink->delay( s->video_sink ) : 0;
 	}
	if( s->sync_mode == STREAM_SYNC_SAMPLES ) {
		// In sample-based sync, the audio sink's sample counter is the master clock.
		// The codec_delay is upstream from the sink and not part of this clock,
		// so it's excluded to prevent an incorrect sync bias.
		int total_delay = /*codec_delay +*/ filter_delay + sink_delay - video_delay;
DBGY		serprintf("stream_sync_av_delay: samples mode codec=%d filter=%d (atempo=%d) sink=%d video=%d total=%d speed=%.3f using_atempo=%d passthrough=%d ac3=%d\n",
			codec_delay, filter_delay, atempo_delay, sink_delay, video_delay, total_delay,
			audio_interface_get_audio_speed(), s->audio_filter_atempo != NULL, passthrough, ac3_recoding);
		return total_delay;
	} else {
		int total_delay = codec_delay + filter_delay + sink_delay - video_delay;
DBGY		serprintf("stream_sync_av_delay: codec=%d filter=%d (atempo=%d) sink=%d video=%d total=%d speed=%.3f using_atempo=%d passthrough=%d ac3=%d\n",
			codec_delay, filter_delay, atempo_delay, sink_delay, video_delay, total_delay,
			audio_interface_get_audio_speed(), s->audio_filter_atempo != NULL, passthrough, ac3_recoding);
		return total_delay;
	}
}

// ************************************************************
//
//	_stream_av_diff
//	returns diff in ms in real world domain: i.e. uses ts 
//	scaled timestamps when audio_speed != 1.0 and adding real
//	world delay between audio and video	(based on ts timestamps)
//	and real world hardware delays
//
// ************************************************************
static int _stream_av_diff( STREAM *s, int video_time, int audio_time )
{
	if( s && !s->put_time_mode && s->video_sink && s->video_sink->put_time ) {
		// Lazy init: ensure put_time_mode is active once the sink is available.
		s->put_time_mode = 1;
	}
DBGY	serprintf("stream_av_diff: put_time_mode=%d\n", s ? s->put_time_mode : -1);
	// Computes video presentation time - audio presentation time
	// Positive value means video is ahead of audio, negative means audio is ahead
	// Formula accounts for buffering delays: audio/video timestamps represent generation time,
	// but actual presentation happens later after passing through decoder/filter/sink pipelines
	// So the formula is: ( video_time - video_delay ) - ( audio_time - ( codec_delay + filter_delay + sink_delay ) )
	//                  = video_time - audio_time + codec_delay + filter_delay + sink_delay - video_delay
	// The sync difference is the video timestamp (V_pts) minus the audio clock predicted for when the video frame displays: diff = V_pts - A_clk_pred.
	// This predicted audio clock is A_clk_pred = (A_pts - A_latency) + V_latency, so the final formula is diff = V_pts - A_pts + A_latency - V_latency.
	int sync_delay = stream_sync_av_delay( s );
	int use_heard_time = 0;
	if( s->put_time_mode ) {
		// In put_time mode, audio_time is anchored to heard time via stream_get_heard_audio_ts.
		// Avoid double-counting delay in the diff by using the same heard-time reference.
		int heard_audio_ts = stream_get_heard_audio_ts( s, audio_time );
		audio_time = heard_audio_ts;
		sync_delay = 0;
		use_heard_time = 1;
	}
#ifdef CONFIG_ANDROID
	// In put_time_mode, heard-time is already in the audio-presented domain.
	// Do not re-add smoothed delay here or we double-count latency.
	if( !use_heard_time && s->put_time_mode && s->smoothed_av_delay > 0 ) {
		// Keep diff aligned with the smoothed anchor when not using heard-time.
		sync_delay = s->smoothed_av_delay;
	}
#endif
#ifdef CONFIG_ANDROID

	if( !use_heard_time && sync_delay <= 0 ) {
		int delay_valid = s->audio_ctx ? audio_interface_is_delay_valid( s->audio_ctx ) : 1;
		if( delay_valid && s->smoothed_av_delay > 0 ) {
			sync_delay = s->smoothed_av_delay;
		} else if( delay_valid && s->audio_ctx ) {
			int static_latency = audio_interface_get_latency( s->audio_ctx );
			if( static_latency > 0 ) {
				sync_delay = static_latency;
			}
		}
	}
#endif
	int using_atempo = (s->audio_filter_atempo != NULL);
	int diff = ( video_time - audio_time ) + sync_delay + RST_TO_TS_DELTA( s->av_delay + stream_dbg_delay, int );
DBGY	serprintf("stream_av_diff: v=%d a=%d sync_delay=%d av_delay=%d dbg_delay=%d diff=%d speed=%.3f using_atempo=%d\n",
		video_time, audio_time, sync_delay, s->av_delay, stream_dbg_delay, diff,
		audio_interface_get_audio_speed(), using_atempo);
	return diff;
}

// ************************************************************
//
//	stream_sync_audio
//
// ************************************************************
static int _stream_get_atempo_delay( STREAM *s )
{
	int use_atempo = (s && s->audio_filter_atempo != NULL);
	if( !use_atempo || !audio_interface_is_audio_speed_enabled() || !audio_interface_is_using_atempo() ) {
		return 0;
	}
	if( s->audio_filter_atempo->delay ) {
		return s->audio_filter_atempo->delay( s->audio_filter_atempo );
	}
	return 0;
}

int stream_sync_audio( STREAM *s, int audio_time )
{
	// Defensive check: validate stream pointer to prevent JNI abort crashes
	if (!s) {
		return 0;
	}
	if( s->sync_a_time == -1 && audio_time != -1 ) {
		DBG serprintf(
			"FIRST_AUDIO_POST_SEEK: audio_time=%d video_time=%d seek_epoch=%d target_sync=%d use_target=%d drop=%d target_ts=%d\n",
			audio_time, s->video_time, s->seek_epoch, s->seek_target_sync_time,
			s->seek_use_target_sync, s->seek_audio_drop, s->seek_audio_target_ts );
	}

#ifdef CONFIG_ANDROID
	int allow_static = 1;
#else
	int allow_static = 0;
#endif
	int anchor_valid = 1;
	int delay_valid = s->audio_ctx ? audio_interface_is_delay_valid( s->audio_ctx ) : 1;
	int current_av_delay = _get_anchor_delay_ms(s, &anchor_valid, allow_static);
	int anchor_delay = current_av_delay;

	static int last_anchor_delay = -1;
	if( !delay_valid && s->last_good_delay_valid ) {
		// After seek, prefer last known good delay over static latency.
		current_av_delay = s->last_good_delay_ms;
		anchor_delay = current_av_delay;
		anchor_valid = 1;
	}
	if( !get_android_sync() && !delay_valid && !anchor_valid ) {
		// No usable timing (no dynamic and no static/last-good); disable delay compensation.
		current_av_delay = 0;
		anchor_delay = 0;
	}
	if( delay_valid ) {
		int delay_streak = s->audio_ctx ? audio_interface_get_delay_valid_streak( s->audio_ctx ) : 0;
		int allow_update = 1;
#ifdef CONFIG_ANDROID
		// android_sync=1: require a short valid streak before accepting a new last_good delay.
		// This avoids capturing transient fallback values (startup_hold) as "good" and
		// biasing speed-change anchoring.
		if( get_android_sync() && delay_streak < 3 ) {
			allow_update = 0;
			DBG serprintf( "stream_sync_audio: skip last_good update (delay_streak=%d current=%d)\n",
				delay_streak, current_av_delay );
		}
#endif
		if( allow_update ) {
			s->last_good_delay_ms = current_av_delay;
			s->last_good_delay_valid = 1;
			s->last_good_atempo_delay_ms = _stream_get_atempo_delay( s );
			DBG serprintf( "stream_sync_audio: last_good_delay=%d last_good_atempo=%d speed=%.3f\n",
				s->last_good_delay_ms, s->last_good_atempo_delay_ms, audio_interface_get_audio_speed() );
			if( s->smoothed_av_delay == -1 ) {
				s->smoothed_av_delay = current_av_delay;
			} else {
				// Check if passthrough mode is active (constant latency, no smoothing needed)
				int passthrough = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
				if( !passthrough ) {
					// Normal mode: smooth dynamic delays
					if (stream_use_xbmc_smoothing) {
						s->smoothed_av_delay = stream_calc_lwma(current_av_delay, s->av_delay_history, &s->av_delay_history_count);
					} else {
						s->smoothed_av_delay = (s->smoothed_av_delay * s->delay_fb + current_av_delay * (1000 - s->delay_fb)) / 1000;
					}
				}
			}
		}
	}
	if( !get_android_sync() && anchor_delay > 0 && s->video_sink && s->video_sink->put_time && audio_time != -1 ) {
		int anchor_ts_raw = audio_time - anchor_delay - RST_TO_TS_DELTA( s->av_delay, int );
		if( anchor_ts_raw < 0 ) {
DBGY			serprintf("anchor_wait: audio_time=%d delay=%d av_delay=%d\n",
				audio_time, anchor_delay, s->av_delay);
			// Non-android_sync: defer anchoring until audible time exists.
			return 0;
		}
	}

	if( anchor_valid && s->video_sink && s->video_sink->put_time && audio_time != -1 ) {
		if( !stream_no_sync || s->sync_a_time == -1 ) {
			int anchor_ts = stream_get_heard_audio_ts( s, audio_time );
#ifdef CONFIG_ANDROID
			// android_sync=1: allow negative heard_ts for internal anchoring at startup.
			if( get_android_sync() && anchor_delay > 0 ) {
				int raw_anchor_ts = audio_time - anchor_delay - RST_TO_TS_DELTA( s->av_delay, int );
				if( raw_anchor_ts < 0 ) {
					anchor_ts = raw_anchor_ts;
				}
			}
#endif
DBGY			serprintf("anchor_ts: audio_time=%d smoothed=%d current=%d av_delay=%d anchor=%d\n",
				audio_time, s->smoothed_av_delay, current_av_delay, s->av_delay, anchor_ts);
			anchor_ts -= RST_TO_TS_DELTA( stream_dbg_delay, int );
			s->video_sink->put_time( s->video_sink, anchor_ts );
			// Audio-driven anchor is authoritative in put_time mode.
			// Prevent the video path from re-anchoring to a different reference.
			s->sink_ref_time = anchor_ts;
			s->vid_ref_time = s->video_time;
		}
	}

	DBGY serprintf("smoothed_av_delay: %d (raw: %d)\n", s->smoothed_av_delay, current_av_delay);

	s->sync_a_time = audio_time;
	
	if( !s->sync_audio || s->play_n_audio_frames || stream_no_sync ) {
		return 0;
	}
	
	// let data without timestamp pass!
	if( audio_time == -1 ) {
		return 0;
	}

	// video is already at end, let data pass!
	if( s->video_end ) {
		return 0;
	}

	// video is paused, ignore
	if( stream_video_paused ) {
		return 0;
	}

DBGY serprintf("{SSA %d}} ", audio_time );
	// both audio and video need to have a valid timestamp before we can start
	int audio_time_for_diff = s->sync_a_time;
	if( s->put_time_mode && s->audio_time != -1 ) {
		// In put_time mode, compare against heard time to stay aligned with sink anchoring.
		audio_time_for_diff = stream_get_heard_audio_ts( s, s->audio_time );
	}
	if( s->sync_v_time == -1 || audio_time_for_diff == -1 )
		return 1;
	
	// if audio is in the future, delay it (but only if significantly ahead)
	int diff = _stream_av_diff( s, s->sync_v_time, audio_time_for_diff );

	// Only block audio if it's significantly ahead (more than threshold)
	if( diff < 0 ) {
DBGY serprintf("{{A %d}} ", diff );
		// In put_time mode, keep video gating active; audio sets the anchor.
		if( !s->put_time_mode ) {
			s->sync_video = 0;
		}
		return 1;
	}
	// allow audio to play from now on
	s->sync_audio = 0;
	
	return 0;
}

// ************************************************************
//
//	stream_sync_video
//
// ************************************************************
int stream_sync_video( STREAM *s, int video_time )
{
	// Defensive check: validate stream pointer to prevent JNI abort crashes
	if (!s) {
		return 0;
	}

	// Additional validation: check critical nested pointers to prevent use-after-free crashes
	// This can occur during stream teardown when the STREAM structure is being deallocated
	// while another thread is still calling this function
	if (!s->audio || !s->video) {
		return 0;
	}

	s->sync_v_time = video_time;

#ifdef CONFIG_ANDROID
	if( get_android_sync() ) {
		return 0;
	}
#endif

	if( !s->sync_video || s->speed != STREAM_SPEED_NORMAL || s->play_n_video_frames || stream_no_sync ) {
		return 0;
	}
	// let data without timestamp pass!
	if( video_time == -1 ) {
		return 0;
	}
	
	// audio is already at end, let data pass!
	if( s->audio_end ) {
		return 0;
	}
		
	// audio is paused, ignore
	if( stream_audio_paused ) {
		return 0;
	}

#ifdef CONFIG_ANDROID
	{
		int allow_static = 1;
		int anchor_valid = 1;
		_get_anchor_delay_ms(s, &anchor_valid, allow_static);
		int delay_valid = s->audio_ctx ? audio_interface_is_delay_valid(s->audio_ctx) : -1;

		// Late-audio start guard when dynamic delay is disabled:
		// static latency is always "valid" in that mode and can over-shift heard time.
		// If audio starts significantly after video, suppress anchor validity briefly.
		if( anchor_valid && delay_valid == 1 && s->put_time_mode &&
			s->audio_time > 0 && s->sync_v_time >= 0 && s->sync_v_time < 1000 ) {
			int audio_lead = s->audio_time - s->sync_v_time;
			if( audio_lead > 150 ) {
				anchor_valid = 0;
			}
		}

		if( !anchor_valid ) {
			// Startup grace: if audio has started but timing is invalid, allow a brief
			// static-latency anchor to avoid large A/V offset at start.
			if( s->put_time_mode && s->audio_time > 0 && s->sync_v_time >= 0 &&
				s->sync_v_time < 500 && delay_valid == 0 ) {
				int static_latency = s->audio_ctx ? audio_interface_get_latency(s->audio_ctx) : 0;
				if( static_latency > 0 ) {
					// We only need anchor_valid to allow sync_video gating; anchor delay itself
					// is still computed via stream_get_heard_audio_ts().
					anchor_valid = 1;
				}
			}
		}
		if( !anchor_valid ) {
DBGY			serprintf("sync_video: timing unavailable, free-run video (anchor_valid=0 smoothed=%d audio_time=%d sync_a=%d vtime=%d put_time=%d delay_valid=%d)\n",
				s->smoothed_av_delay, s->audio_time, s->sync_a_time, s->sync_v_time,
				s->put_time_mode, delay_valid);
			return 0;
		}
	}
#else
	{
		int allow_static = 0;
		int anchor_valid = 1;
		// Use the unified anchor delay to decide if timing is available.
		_get_anchor_delay_ms(s, &anchor_valid, allow_static);
		if( !anchor_valid ) {
DBGY			serprintf("sync_video: timing unavailable, free-run video (anchor_valid=0 smoothed=%d audio_time=%d sync_a=%d vtime=%d put_time=%d)\n",
				s->smoothed_av_delay, s->audio_time, s->sync_a_time, s->sync_v_time,
				s->put_time_mode);
			return 0;
		}
	}
#endif
	
DBGY serprintf("{SSV %d}} ", video_time );
	// both audio and video need to have a valid timestamp before we can start
	int audio_time_for_diff = s->sync_a_time;
	if( s->put_time_mode && s->audio_time != -1 ) {
		// In put_time mode, compare against heard time to stay aligned with sink anchoring.
		audio_time_for_diff = stream_get_heard_audio_ts( s, s->audio_time );
	}
	if( s->sync_v_time == -1 || audio_time_for_diff == -1 )
		return 1;
	if( s->put_time_mode && audio_time_for_diff <= 0 ) {
		// Startup/resume warm-up: allow a few frames before audible time exists.
		if( s->warmup_video_frames < 5 ) {
			s->warmup_video_frames++;
			return 0;
		}
		return 1;
	}

	// if video is in the future, delay it
	int diff = _stream_av_diff( s, s->sync_v_time, audio_time_for_diff );
	// if we sample post sink, allow us to start 500ms early
	int max_rst = s->vtime_post_sink ? 500 : 0;
	if( s->seek_epoch > 0 && s->put_time_mode && !s->seek_converge_done ) {
		// During post-seek convergence, don't allow early start.
		max_rst = 0;
	}

	// Post-seek convergence: allow a short window to align to heard audio,
	// then re-anchor once and stop gating to avoid stutter.
	if( s->seek_epoch > 0 && s->put_time_mode && s->audio_time != -1 ) {
		if( s->seek_converge_epoch != s->seek_epoch ) {
			s->seek_converge_epoch = s->seek_epoch;
			s->seek_converge_until_ms = atime() + 500;
			s->seek_converge_done = 0;
		}
		if( !s->seek_converge_done && atime() >= s->seek_converge_until_ms ) {
			if( s->video_sink && s->video_sink->put_time ) {
				int anchor_ts = stream_get_heard_audio_ts( s, s->audio_time );
#ifdef CONFIG_ANDROID
				if( get_android_sync() && s->audio_ctx ) {
					int delay_valid = audio_interface_is_delay_valid( s->audio_ctx );
					if( !delay_valid && s->last_good_delay_valid ) {
						int effective_delay = s->last_good_delay_ms;
						int atempo_delay = _stream_get_atempo_delay( s );
						effective_delay += atempo_delay - s->last_good_atempo_delay_ms;
						if( effective_delay < 0 ) {
							effective_delay = 0;
						}
						anchor_ts = s->audio_time - effective_delay - RST_TO_TS_DELTA( s->av_delay, int );
						if( anchor_ts < 0 ) {
							anchor_ts = 0;
						}
DBGY					serprintf("post-seek converge anchor: last_good=%d atempo=%d eff=%d audio=%d anchor=%d\n",
							s->last_good_delay_ms, atempo_delay, effective_delay, s->audio_time, anchor_ts);
					}
				}
#endif
DBGY				serprintf("post-seek converge anchor: diff=%d anchor_ts=%d\n",
					diff, anchor_ts);
				s->video_sink->put_time( s->video_sink, anchor_ts );
				s->sink_ref_time = anchor_ts;
				s->vid_ref_time = s->video_time;
			}
			s->seek_converge_done = 1;
		}
		if( s->seek_converge_done ) {
			// After convergence, stop gating to avoid stutter.
			return 0;
		}
	}
	// Wait if video is LATE by more than the threshold.
	int max_wait = RST_TO_TS_DELTA( max_rst, int );
	if( diff > max_wait ) {
DBGY serprintf( "{{V %d}} ", diff );
		s->sync_audio = 0;
		return 1; // Wait
	}

	// allow video to play from now on
	if( !s->put_time_mode ) {
		s->sync_video = 0;
	}
	
	return 0;
}

// ************************************************************
//
//	stream_sync
//
// ************************************************************
void stream_sync( STREAM *s )
{
	// Defensive check: validate stream pointer and nested pointers to prevent JNI abort crashes
	// This crash can occur during stream teardown when the STREAM structure is being deallocated
	// while another thread (e.g., FileObserver) is still calling this function
	if (!s || !s->audio || !s->video) {
		return;
	}

	// if we have audio ...

	if ( !s->audio->valid || !s->video->valid )
		goto EXIT;
		
	if ( s->speed )
		goto EXIT;

	if ( s->slideshow )
		goto EXIT;
	
	// sync machine still working?
	if( (s->sync_audio || s->sync_video) && !stream_no_sync ) 
		goto EXIT;

	// audio and video time known?
	if ( s->audio_time <= 0 || s->video_time <= 0 )
		goto EXIT;
	
	// audio done?
	if ( s->audio_end )
		goto EXIT;
	
	// ... we calc the delay between audio and video frames and
	// try adjust it to zero
	int rdiff = _stream_av_diff( s, s->video_time, s->audio_time );
	// let's clamp to avoid big correction visual effect to the user and grant stability
	int clamp_ts = RST_TO_TS_DELTA( 250, int );
	int diff  = MAX( MIN( rdiff,  clamp_ts ), -clamp_ts );
	
 	if( !s->delay_valid ) {
DBGVY serprintf("(D %d)", diff );
		s->delay = diff;
	} else {
		// to avoid oscillations, we use moving average to allow convergence and grant stability
		// 900 s->delay_fb is used for a exponential moving average window and thus has no scale
		if (stream_use_xbmc_smoothing) {
			s->delay = stream_calc_lwma(diff, s->delay_history, &s->delay_history_count);
		} else {
			s->delay = (s->delay * s->delay_fb + diff * (1000 - s->delay_fb)) / 1000;
		}
	}
	s->delay_valid = 1;
	
DBGVY serprintf("(%3d|%3d|%3d)", rdiff, diff, s->delay );

	if ( stream_no_sync || (s->video_sink && s->video_sink->put_time) ) {
		// ANDROID: this is the android mode with a put_time function in the video sink which basically disables the sync logic
		goto EXIT;
	}
		 
	s->drop_B = 0;

	// --- A/V Sync Correction Logic ---
	// The decision to drop/double frames is made by comparing the A/V delay in the RST (real-world ms) domain
	// to ensure the sync window tolerance is constant regardless of playback speed.
	// The internal state variable s->delay remains in the TS (Time-Scaled) domain, and adjustments to it
	// must also be in the TS domain.

	// 1. Define the threshold in the RST domain for making decisions. stream_max_delay default value is 1 i.e. threshold is one frame duration.
	int threshold_rst = stream_max_delay * s->video->msPerFrame;
	int pdrop_threshold_rst = stream_pdrop_threshold; // default value is 0

	// 2. Define the adjustment value in the TS domain for modifying the state variable.
	int adjustment_ts = RST_TO_TS_DELTA(threshold_rst, int);

	// 3. Perform comparisons in the TS domain to avoid accumulating rounding errors from TS->RST conversion at variable speeds.
	if( s->delay > adjustment_ts ) {
		// video is too fast, we have to slow down
		s->drop = -1;
		// Adjust the TS-domain state variable by a TS-domain value for reactivity and do not go through the averaging smoothering
		s->delay -= adjustment_ts;
DBGVY serprintf( "_S(%3d)_", s->delay );
	} else if( s->delay < (-1 * adjustment_ts) ) {
		// video is late, check for P-frame drop condition (note that this is disabled by default	)
		if( stream_pdrop_threshold && TS_TO_RST_DELTA(rdiff, int) < (-1 * pdrop_threshold_rst) ) {
			// we are totally late, see if we can skip to next key frame
			int max_time = s->video_time - rdiff + 500;
			int key_time;
			int num;
			if( (num = stream_parser_find_key_frame( s, max_time, &key_time )) ) {
				int dropped = stream_parser_drop_video( s, key_time );
DBGVY serprintf("XX(%d %d %d) ", num, key_time, dropped );
				// set drop_P to 1 which means drop all you have and restart
				s->drop_P = 1;
				return;
			}
		}

		// video is late, hurry up (B-frame or P-frame drop)
		// Note: stream_bdrop_threshold is a frame-count multiplier, not a time duration.
		// The comparison uses s->delay (TS) and a threshold based on the frame duration converted to TS.
		if( stream_bdrop_threshold && s->delay < ( -1 * stream_bdrop_threshold * RST_TO_TS_DELTA(s->video->msPerFrame, int) ) ) {
			s->drop_B = 1;
		}
		s->drop = 1;
		// Adjust the TS-domain state variable by a TS-domain value for reactivity and do not go through the averaging smoothering
		s->delay += adjustment_ts;
DBGVY serprintf("_%s(%3d)_", s->drop_B ? "B" : "F", s->delay );
	} else {
		DBGVY serprintf( "  (   ) " );
	}
	return;
EXIT:
DBGV serprintf("  (---) " );
}

#ifdef DEBUG_MSG
void *AV_get_ctx( void );

static void _stream_delay_plus( int argc, char *argv[] )
{
	if( argc > 1 ) {
		stream_dbg_delay += atoi( argv[1] );
	} else {
		stream_dbg_delay += 20;
	}
	
	STREAM *s = AV_get_ctx();
	if( s ) {
serprintf("av_delay  %5d\n", stream_sync_av_delay( s ) );
	}
serprintf("dbg_delay %5d\n", stream_dbg_delay );
}

static void _stream_delay_minus( int argc, char *argv[] )
{
	if( argc > 1 ) {
		stream_dbg_delay -= atoi( argv[1] );
	} else {
		stream_dbg_delay -= 20;
	}
	STREAM *s = AV_get_ctx();
	if( s ) {
serprintf("av_delay  %5d\n", stream_sync_av_delay( s ) );
	}
serprintf("dbg_delay %5d\n", stream_dbg_delay );
}

static void _stream_delay_set( int argc, char *argv[] )
{
	if( argc > 1 ) {
		stream_dbg_delay = atoi( argv[1] );
	}
	STREAM *s = AV_get_ctx();
	if( s ) {
serprintf("av_delay  %5d\n", stream_sync_av_delay( s ) );
	}
serprintf("dbg_delay %5d\n", stream_dbg_delay );
}

DECLARE_DEBUG_COMMAND("sep", 	_stream_delay_plus   );
DECLARE_DEBUG_COMMAND("sem", 	_stream_delay_minus  );
DECLARE_DEBUG_COMMAND("ses", 	_stream_delay_set    );

static void _stream_toggle_xbmc( int argc, char *argv[] )
{
	stream_use_xbmc_smoothing = !stream_use_xbmc_smoothing;
	serprintf("stream_use_xbmc_smoothing: %d\n", stream_use_xbmc_smoothing );
}
DECLARE_DEBUG_COMMAND("sxbmc", _stream_toggle_xbmc );
#endif

#endif

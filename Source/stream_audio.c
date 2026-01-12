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
#include "debug.h"
#include "atime.h"
#include "util.h"
#include "file.h"

#include <string.h>
#include <math.h>

#define DBGS DBG_IF(Debug[DBG_STREAM])
#define DBGA DBG_IF(Debug[DBG_AUD])
#define DBGV DBG_IF(Debug[DBG_VID])

#define DBG DBG_IF(Debug[DBG_STREAM])
#define DBG2 DBG_IF(Debug[DBG_STREAM] > 1)
#define ERR if( 1 )

#ifdef CONFIG_STREAM

AV_PROPERTIES *stream_force_audio_props = NULL;

void stream_audio_props_changed( STREAM *s, STREAM_CDATA *cdata );
void stream_audio_samplerate_changed( STREAM *s );

static int zero_time = 200;
static int stream_audio_chunk = 4096;
static int ac3_sink_configured = 0;  // Track if sink is configured for AC3 passthrough
static int ac3_reconfigure_pending = 1;  // Force initial reconfiguration when AC3 recoding starts
static int audio_format_configured = -1;  // Track audio format to avoid redundant passthrough reconfigurations
extern int stream_audio_paused;
extern int libavos_get_ac3_recoding_enabled(void);
extern int libavos_get_max_pcm_channels(void);

static int pcm_channel_cap = 0;

static int stream_audio_format_supports_passthrough(int format)
{
	switch( format ) {
	case WAVE_FORMAT_AC3:
	case WAVE_FORMAT_EAC3:
	case WAVE_FORMAT_E_AC3_JOC:
	case WAVE_FORMAT_DTS:
	case WAVE_FORMAT_DTS_HD:
	case WAVE_FORMAT_DTS_HD_MA:
		return 1;
	default:
		return 0;
	}
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

#ifdef CONFIG_SPDIF
	// When passthrough is disabled and AC3 recoding is disabled,
	// audio will be decoded to PCM regardless of source format.
	// Force sink format to PCM to ensure AudioTrack is created with correct format.
	// (AC3 recoding paths will override this to WAVE_FORMAT_AC3 after calling this function)
	if( !spdif_is_passthrough_on() && !libavos_get_ac3_recoding_enabled() ) {
		if( sink->format != WAVE_FORMAT_PCM ) {
			sink->format = WAVE_FORMAT_PCM;
		}
	}
#endif
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

	// AC3 recoding: determine passthrough mode based on IEC61937 capability
	// Prefer Mode 1 (manual IEC wrapping) if IEC61937 is supported
	// Fall back to Mode 2 (codec-specific) if IEC61937 is not available (e.g., eARC without IEC)
	int passthrough_mode = spdif_is_passthrough_on();  // Default from libavos_set_passthrough
	extern int get_hdmi_supports_iec(void);
	if (libavos_get_ac3_recoding_enabled() && !get_hdmi_supports_iec()) {
		passthrough_mode = 2;  // Override to mode 2 if IEC not available
		spdif_set_passthrough(passthrough_mode); // Keep SPDIF encapsulation mode in sync
		serprintf("stream_audio_setup_ac3_sink: IEC61937 not available, using mode 2 (codec-specific) for AC3 recoding\n");
	} else {
		serprintf("stream_audio_setup_ac3_sink: using mode %d for AC3 recoding\n", passthrough_mode);
	}
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
	s->audio_time = time;
DBGA serprintf(" <<%d>> ", s->audio_time);
	stream_sync_audio( s, s->audio_time );
}

// ************************************************************
//
//	_add_audio_time
//
// ************************************************************
static void _add_audio_time( STREAM *s, int time )
{
	if( s->audio_time != -1 ) {
		s->audio_time += time;
DBGA serprintf(" <+%d> ", time);
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

extern int DEBUG_delay;
void _stream_resync( STREAM *s );

static void _wait( STREAM *s, int wait )
{
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
					return;
				}
				stream_yield_RT();
			}
			if( _abort( s ) ) {
				return;
			}
			_add_audio_time( s, RST_TO_TS_DELTA(to_wait, int) );

			s->audio_sink->write( s, &frame );
			wait -= to_wait;
		}
	}				
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
	}

	if( s->audio->valid && (!(s->paused || stream_audio_paused) || s->play_n_audio_frames ) ) {
		if( s->audio_sink && s->audio_preload ) {
			s->audio_preload = 0;
			// restuff the audio pipe! - unless this is a passthrough sink
			int passthrough = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
			if( s->audio_sink->syncable( s ) && !passthrough ) {
				s->audio_sink->flush( s );
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
				if( s->seek_epoch > 0 && s->audio_time < 0 &&
					s->video_time >= 0 && cdata.time != STREAM_NO_PTS_VALUE &&
					cdata.time + 1000 < s->video_time ) {
					// First audio after seek is far behind video; rebase to avoid freeze.
					DBG serprintf("AUDIO_PTS_BEHIND_VIDEO: time=%d video=%d\n",
						cdata.time, s->video_time);
					cdata.time = s->video_time;
				}
				if( s->seek_epoch > 0 && s->audio_time >= 0 &&
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

				if( s->sync_mode == STREAM_SYNC_SAMPLES && s->speed == STREAM_SPEED_NORMAL ) {
					if( s->audio_ref_time == -1 && cdata.time != STREAM_NO_PTS_VALUE ) {
						s->audio_ref_time = cdata.time;
						s->audio_samples  = 0;
						_set_audio_time( s, cdata.time );
DBGA serprintf(" [[%d]] ", s->audio_ref_time);
					}
				} else {
					if( cdata.time != STREAM_NO_PTS_VALUE ) {
						_set_audio_time( s, cdata.time );
					}
				}

				while( !_abort( s ) && stream_sync_audio( s, s->audio_time ) ) {
DBGS serprintf("~");
					msec_sleep( 10 );
					stream_yield_RT();
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
		int passthrough_supported = stream_audio_format_supports_passthrough( s->audio->format );
		int passthrough_active = passthrough && passthrough_supported;
		if( passthrough && !passthrough_supported ) {
			DBG serprintf("stream_audio: codec %04X not supported for passthrough, decoding as PCM\n",
				s->audio->format);
			if( s->audio_sink ) {
				s->audio_sink->set_passthrough( s, 0 );
			}
			passthrough_active = 0;
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

		// Save decoded bytes for audio time accounting AFTER filtering
		int decoded_bytes = decoded;

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

		if( s->audio_sink ) {
			AUDIO_PROPERTIES *sink_props = stream_audio_get_sink_props( s );
			if( !audio_frame.error ) {
				// Store original format and properties before filtering
				original_format = sink_props ? sink_props->format : s->audio->format;
				original_channels = s->audio->channels;
				original_rate = s->audio->samplesPerSec;
				original_bits = s->audio->bitsPerSample;

				DBG serprintf("stream_audio: decoded frame fmt=%04X size=%d passthrough=%d active=%d recoding=%d\n",
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
				if (!audio_interface_is_audio_speed_enabled()) {
					use_atempo = 0;  // Audio speed feature disabled
				}
				if (!audio_interface_is_using_atempo()) {
					use_atempo = 0;  // User chose AudioTrack-based speed
				}
				if (passthrough == 1 || passthrough == 2) {
					use_atempo = 0;  // Passthrough mode active
				}

				if (use_atempo && audio_frame.size > 0) {
					DBG serprintf("stream_audio: applying atempo filter\n");
					s->audio_filter_atempo->filter(s->audio_filter_atempo, &audio_frame);
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

				DBG serprintf("stream_audio: post-filter frame fmt=%04X size=%d\n",
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
#ifdef CONFIG_SPDIF
							int passthrough_mode = spdif_is_passthrough_on();
#else
							int passthrough_mode = 0;
#endif
							if( passthrough_mode == 0 ) {
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
								// AC3 recoding: determine passthrough mode based on IEC61937 capability
								// Prefer Mode 1 if IEC61937 supported, fallback to Mode 2 if not
								int passthrough_mode = spdif_is_passthrough_on();  // Default from libavos_set_passthrough
								extern int get_hdmi_supports_iec(void);
								if (libavos_get_ac3_recoding_enabled() && !get_hdmi_supports_iec()) {
									passthrough_mode = 2;  // Override to mode 2 if IEC not available
									spdif_set_passthrough(passthrough_mode); // Keep SPDIF encapsulation mode in sync
									serprintf("AC3 recoding reconfigure: IEC61937 not available, using mode 2\n");
								} else {
									DBG serprintf("AC3 recoding reconfigure: using mode %d\n", passthrough_mode);
								}
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
							// Set passthrough mode based on whether SPDIF passthrough is enabled
							int passthrough_mode = 0;
#ifdef CONFIG_SPDIF
							if(spdif_is_passthrough_on() && spdif_init(sink)) {
								passthrough_mode = spdif_is_passthrough_on();
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
							AUDIO_FRAME wrapped_frame = {0};
							int dummy_decoded = 0;
							AUDIO_PROPERTIES *spdif_props = stream_audio_get_sink_props( s );
							DBG2 serprintf("IEC wrap input: frame=%p size=%d pcm_fake=%d\n",
								audio_frame.data, audio_frame.size, pcm_fake_size);
							spdif_encapsulate( spdif_props, audio_frame.data, audio_frame.size, &wrapped_frame, &dummy_decoded );

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
				// Update audio time based on ACTUAL filtered output (not decoded bytes)
				// This is critical for atempo filter which changes audio duration
				if( s->sync_mode != STREAM_SYNC_SAMPLES ) {
					if( !s->audio->vbr && audio_frame.size > 0 ) {
						// Calculate time based on actual output size after filtering
						int bytes_per_sample = (audio_frame.bits ? audio_frame.bits : original_bits) / 8;
						int channels = audio_frame.channels ? audio_frame.channels : original_channels;
						int sample_rate = audio_frame.samplesPerSec ? audio_frame.samplesPerSec : original_rate;
						int prev_audio_time = s->audio_time;

						if( bytes_per_sample > 0 && channels > 0 && sample_rate > 0 ) {
							// For AC3 recoding, use fakeSize (represents PCM equivalent)
							int effective_size = (ac3_recoding && audio_frame.fakeSize > 0) ?
								audio_frame.fakeSize : audio_frame.size;
							int output_samples = effective_size / (bytes_per_sample * channels);
							int output_time_ms = (output_samples * 1000) / sample_rate;

							// Option B: atempo output is already in TS domain (physical playback time)
							// Don't double-scale by applying RST_TO_TS_DELTA when atempo is active
							// If filter exists, we're in Option B mode (timeline mapping enabled)
							if( use_atempo ) {
								// atempo output duration = physical samples @ 1.0x = TS domain
								_add_audio_time( s, output_time_ms );
								DBG serprintf("stream_audio: atempo output, audio_time +%d ms (TS domain, no scaling)\n",
									output_time_ms);
							} else {
								// Normal path: physical samples need RST→TS conversion
								_add_audio_time( s, RST_TO_TS_DELTA(output_time_ms, int) );
								DBG serprintf("stream_audio: normal output, audio_time +%d ms (RST→TS scaled)\n",
									RST_TO_TS_DELTA(output_time_ms, int));
							}
							DBG serprintf("stream_audio: audio_time update prev=%d now=%d using_atempo=%d speed=%.3f output_ms=%d bytes=%d bps=%d ch=%d rate=%d\n",
								prev_audio_time, s->audio_time, use_atempo, audio_interface_get_audio_speed(),
								output_time_ms, effective_size, bytes_per_sample, channels, sample_rate);
						} else if( s->audio->bytesPerSec ) {
							// Fallback: use decoded bytes (original behavior)
							_add_audio_time( s, RST_TO_TS_DELTA(decoded_bytes * 1000 / s->audio->bytesPerSec, int) );
							DBG serprintf("stream_audio: audio_time fallback prev=%d now=%d decoded_bytes=%d bytesPerSec=%d using_atempo=%d speed=%.3f\n",
								prev_audio_time, s->audio_time, decoded_bytes, s->audio->bytesPerSec,
								use_atempo, audio_interface_get_audio_speed());
						}
					}
				}

				// slowly drain the audio data we have, while updating the audio time...
				int size = audio_frame.size;
				while( size > 0 ) {
					if( _abort( s ) ) {
						return;
					}
					audio_frame.size = MIN( stream_audio_chunk * s->audio->channels, size );

					// no error, output PCM
					DBG serprintf("stream_audio: checking if sink can_write %d bytes\n", audio_frame.size);
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
					DBG serprintf("stream_audio: calling sink->write with frame fmt=%04X size=%d\n",
						audio_frame.format, audio_frame.size);
					if( s->audio_resume_pending ) {
						DBG serprintf("stream_audio: first audio output after resume (audio_time=%d video_time=%d seek_epoch=%d)\n",
							s->audio_time, s->video_time, s->seek_epoch);
						s->audio_resume_pending = 0;
					}
					int size_written = s->audio_sink->write( s, &audio_frame );
					DBG serprintf("stream_audio: sink->write returned %d\n", size_written);

					if( _abort( s ) ) {
						return;
					}
					if( size_written <= 0 ) {
						DBG serprintf("stream_audio: write failed (%d), dropping remainder\n", size_written);
						size = 0;
						break;
					}

					if( size_written > 0 && s->sync_mode == STREAM_SYNC_SAMPLES && audio_frame.size && s->audio_ref_time != -1 ) {
						// add the samples and calc new time
						if( s->audio->samplesPerSec ) {
							s->audio_samples += (passthrough_active ? audio_frame.fakeSize : size_written) / s->audio->bytesPerFrame;
							int delta = (UINT64)1000 * (UINT64)s->audio_samples / (UINT64)s->audio->samplesPerSec;
							int prev_audio_time = s->audio_time;

							// Check if atempo filter is active - output samples are already in TS domain (physical time)
							int use_atempo = (s->audio_filter_atempo != NULL);
							if (!audio_interface_is_audio_speed_enabled() || !audio_interface_is_using_atempo()) {
								use_atempo = 0;
							}
							if( use_atempo ) {
								// atempo output = physical samples @ 1.0x = TS domain, no scaling needed
								_set_audio_time( s, s->audio_ref_time + delta );
								DBG serprintf("stream_audio SAMPLES: atempo active, audio_time = %d + %d (no scaling)\n",
									s->audio_ref_time, delta);
							} else {
								// Normal path: samples in RST domain need RST→TS conversion
								_set_audio_time( s, s->audio_ref_time + RST_TO_TS_DELTA(delta, int) );
								DBG serprintf("stream_audio SAMPLES: normal, audio_time = %d + RST_TO_TS(%d)\n",
									s->audio_ref_time, delta);
							}
							DBG serprintf("stream_audio SAMPLES: audio_time update prev=%d now=%d using_atempo=%d speed=%.3f delta_ms=%d\n",
								prev_audio_time, s->audio_time, use_atempo, audio_interface_get_audio_speed(), delta);
							// if size_written < size, we don't want to go out of sync on passthrough
							audio_frame.fakeSize = 0;
						}
					}

					size             -= size_written;
					audio_frame.data += size_written;
				}
				
				if( s->audio_sink->syncable( s ) && s->audio_sink->can_write( s, size ) ) {
					if( !_abort( s ) ) {
						s->audio_yield = 0;
						return;
					}
				}
			
			} else if( s->audio_end ) {
				// end, signal to the sink that we are finished
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
DBGS serprintf("PID[%5d] stream_audio_thread::Starting\r\n", getpid() );

	// Reset AC3 sink configuration flag for new playback session
	ac3_sink_configured = 0;
	// Initialize with current format to prevent redundant reconfiguration on first audio thread loop iteration.
	// The sink was already configured by start() before the audio thread began, so we use the current format
	// to avoid a redundant set_passthrough call that would recreate the AudioTrack unnecessarily.
	audio_format_configured = stream_audio_get_sink_props( s )->format;

	while( thread_state_get( &s->audio_tstate ) != THREAD_EXIT ) {
		AUDIO_PROPERTIES *sink = stream_audio_get_sink_props( s );
		if( sink && sink->format != audio_format_configured ) {
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
			} else if(spdif_is_passthrough_on() && spdif_init(sink) && s->audio_sink) {
				// Only call spdif_init if passthrough is actually enabled to avoid unnecessary side effects
				s->audio_sink->set_passthrough(s, spdif_is_passthrough_on() );
				audio_format_configured = sink->format;
			} else
#endif
			{
				s->audio_sink->set_passthrough(s, 0);
				audio_format_configured = sink->format;
			}
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


#endif

#endif

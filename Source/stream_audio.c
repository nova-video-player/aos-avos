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

#define DBGS if(Debug[DBG_STREAM])
#define DBGA if(Debug[DBG_AUD])
#define DBGV if(Debug[DBG_VID])

#define DBG if(0)

#ifdef CONFIG_STREAM

AV_PROPERTIES *stream_force_audio_props = NULL;

void stream_audio_props_changed( STREAM *s, STREAM_CDATA *cdata );
void stream_audio_samplerate_changed( STREAM *s );

static int zero_time = 200;
static int stream_audio_chunk = 4096;
static int ac3_sink_configured = 0;  // Track if sink is configured for AC3 passthrough
extern int stream_audio_paused;
extern int libavos_get_ac3_recoding_enabled(void);

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
	
	if( s->audio->valid && (!(s->paused || stream_audio_paused) || s->play_n_audio_frames ) ) {
		if( s->audio_sink && s->audio_preload ) {
			s->audio_preload = 0;
			// restuff the audio pipe! - unless this is a passthrough sink
			int passthrough = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
			if( s->audio_sink->syncable( s ) && !passthrough ) {
				s->audio_sink->flush( s );
				s->audio_sink->preload( s );
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
			goto EXIT;
		} 

		int passthrough = s->audio_sink ? s->audio_sink->get_passthrough( s ) : 0;
		int ac3_recoding = libavos_get_ac3_recoding_enabled();

		AUDIO_FRAME audio_frame = { 0 };
		int decoded = 0;

		audio_frame.time = s->audio_time;

		// For AC3 recoding, always decode ALL formats (including AC3) to PCM to enable filters
		// This provides consistent audio boost/night mode support for all source formats
		if( s->audio_dec && (!passthrough || ac3_recoding) ) {
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
			s->audio->ctx = s;
			spdif_encapsulate( s->audio, s->audio_buffer, s->audio_buffer_size, &audio_frame, &decoded );
#endif
		}

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
							if( !s->audio->vbr && s->audio->bytesPerSec ) {
					_add_audio_time( s, RST_TO_TS_DELTA(decoded * 1000 / s->audio->bytesPerSec, int) );
				}
		}
		
		if( s->dump_pcm_fd > 0 ) {
			file_write( s->dump_pcm_fd, audio_frame.data, audio_frame.size );
		}

		if( s->audio_sink ) {
			if( !audio_frame.error ) {
				// Store original format before filtering
				int original_format = s->audio->format;
				int original_channels = s->audio->channels;
				int original_rate = s->audio->samplesPerSec;
				int original_bits = s->audio->bitsPerSample;

				DBG serprintf("stream_audio: decoded frame fmt=%04X size=%d passthrough=%d recoding=%d\n",
					audio_frame.format, audio_frame.size, passthrough, ac3_recoding);

				// For AC3 recoding, always run filters on ALL decoded formats
				// This provides consistent audio boost/night mode for all sources
				int run_filter = (!passthrough || ac3_recoding);
				if( run_filter ) {
					// Apply filters in order: compress -> AC3 -> JNI
					// 1. Compression/boost filter
					if( s->audio_filter_compress && audio_frame.size > 0 ) {
						DBG serprintf("stream_audio: applying compress filter\n");
						s->audio_filter_compress->filter( s->audio_filter_compress, &audio_frame );
					}
					// 2. AC3 encoding filter (only in AC3 recoding mode)
					if( s->audio_filter_ac3 && audio_frame.size > 0 ) {
						DBG serprintf("stream_audio: applying AC3 filter\n");
						s->audio_filter_ac3->filter( s->audio_filter_ac3, &audio_frame );
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
				int format_changed = audio_frame.format && audio_frame.format != original_format;
				int channels_changed = audio_frame.channels && audio_frame.channels != original_channels;
				int samplerate_changed = audio_frame.samplesPerSec && audio_frame.samplesPerSec != original_rate;
				int bits_changed = audio_frame.bits && audio_frame.bits != original_bits;

				// Track if sink is already configured for AC3 recoding to avoid redundant reconfigurations
				int is_ac3_recoding = ac3_recoding && audio_frame.format == WAVE_FORMAT_AC3;
				int need_reconfigure = (format_changed || channels_changed || samplerate_changed || bits_changed) &&
				                       (!is_ac3_recoding || !ac3_sink_configured);

				if( audio_frame.size > 0 && need_reconfigure ) {
DBG serprintf("audio format changed by filter: %04X -> %04X, reconfiguring sink\n", original_format, audio_frame.format);
					// Update audio properties with new format and layout
					// For AC3 recoding, DON'T update s->audio->format (keep source format)
					if( format_changed && !is_ac3_recoding ) {
						s->audio->format = audio_frame.format;
					}
					if( samplerate_changed ) {
						s->audio->samplesPerSec = audio_frame.samplesPerSec;
						s->audio->sourceSamples = audio_frame.samplesPerSec;
					}
					if( channels_changed && !is_ac3_recoding ) {
						s->audio->channels = audio_frame.channels;
						s->audio->sourceChannels = audio_frame.channels;
					}
					if( bits_changed && !is_ac3_recoding ) {
						s->audio->bitsPerSample = audio_frame.bits;
						s->audio->sourceBitsPerSample = audio_frame.bits;
					}
					if( s->audio->channels && s->audio->bitsPerSample ) {
						s->audio->bytesPerFrame = s->audio->channels * s->audio->bitsPerSample / 8;
					}
					// Reconfigure audio sink with new parameters
					if( s->audio_sink ) {
						if( s->audio_sink_open ) {
							s->audio_sink->stop( s );
						}
						if( is_ac3_recoding ) {
							DBG serprintf("AC3 recoding: configuring sink for AC3 passthrough (stereo IEC61937)\n");
							// Temporarily set s->audio to AC3 stereo for sink->start()
							// sink->start() reads from s->audio to configure audio interface
							int saved_format = s->audio->format;
							int saved_channels = s->audio->channels;
							int saved_bits = s->audio->bitsPerSample;

							s->audio->format = WAVE_FORMAT_AC3;
							s->audio->channels = 2;  // IEC61937 uses stereo container
							s->audio->bitsPerSample = 16;
							s->audio->bytesPerFrame = 2 * 16 / 8;

							// Call start() FIRST to update audio_ctx_t, THEN set_passthrough()
							if( s->audio_sink->start( s ) ) {
								DBG serprintf("failed to restart audio sink after AC3 recoding\n");
								s->audio_sink_open = 0;
								ac3_sink_configured = 0;
							} else {
								s->audio_sink_open = 1;
								// Now set_passthrough() can read the correct values from audio_ctx_t
								s->audio_sink->set_passthrough( s, 2 );
								ac3_sink_configured = 1;
							}

							// Restore original source format immediately
							s->audio->format = saved_format;
							s->audio->channels = saved_channels;
							s->audio->bitsPerSample = saved_bits;
							s->audio->bytesPerFrame = saved_channels * saved_bits / 8;
						} else {
							ac3_sink_configured = 0;
							s->audio_sink->set_passthrough( s, 0 );
							if( s->audio_sink->start( s ) ) {
								DBG serprintf("failed to restart audio sink after format change\n");
								s->audio_sink_open = 0;
							} else {
								s->audio_sink_open = 1;
							}
						}
					}
				}

				// slowly drain the audio data we have, while updating the audio time...
				int size = audio_frame.size;
				while( size > 0 ) {
					audio_frame.size = MIN( stream_audio_chunk * s->audio->channels, size );

					// no error, output PCM
					while( !s->audio_sink->can_write( s, audio_frame.size ) ) {
						if( _abort( s ) ) {
							return;
						}
						stream_yield_RT();
					}
					int size_written = s->audio_sink->write( s, &audio_frame );

					if( s->sync_mode == STREAM_SYNC_SAMPLES && audio_frame.size && s->audio_ref_time != -1 ) {
						// add the samples and calc new time
						if( s->audio->samplesPerSec ) {
							s->audio_samples += ((passthrough == 2)?audio_frame.fakeSize : size_written) / s->audio->bytesPerFrame;
							int delta = (UINT64)1000 * (UINT64)s->audio_samples / (UINT64)s->audio->samplesPerSec;
							_set_audio_time( s, s->audio_ref_time + RST_TO_TS_DELTA(delta, int) );
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
	}
EXIT:
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

	int audio_format = -1;
	while( thread_state_get( &s->audio_tstate ) != THREAD_EXIT ) {
		if(s->audio->format != audio_format) {
			audio_format = s->audio->format;
#ifdef CONFIG_SPDIF
			if(libavos_get_ac3_recoding_enabled()) {
				// AC3 recoding: ALL formats (including native AC3/EAC3) go through PCM decode -> filter -> AC3 recode
				// This allows filters (night mode, audio boost) to work on all audio sources
				if( s->audio_sink ) {
					DBG serprintf("AC3 recoding: all formats will recode through PCM\n");
					s->audio_sink->set_passthrough( s, 0 );
				}
			} else if(spdif_init(s->audio) && s->audio_sink) {
				s->audio_sink->set_passthrough(s, spdif_is_passthrough_on() );
			} else
#endif
			{
				s->audio_sink->set_passthrough(s, 0);
			}
		}

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

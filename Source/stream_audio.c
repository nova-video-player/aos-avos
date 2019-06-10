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

#ifdef CONFIG_STREAM

AV_PROPERTIES *stream_force_audio_props = NULL;

void stream_audio_props_changed( STREAM *s, STREAM_CDATA *cdata );
void stream_audio_samplerate_changed( STREAM *s );

static int zero_time = 200;
static int stream_audio_chunk = 4096;
extern int stream_audio_paused;

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
	if( s->audio_filter ) {
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
			_add_audio_time( s, to_wait );

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
				if( cdata.time < 0 ) {
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

				// check if the audio chunk is discontinuous and wait
				if( cdata.audio_skip_time ) {
serprintf("_wait!(%d)", cdata.audio_skip_time);
					_wait( s, cdata.audio_skip_time );
					cdata.audio_skip_time = 0;
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
					if( s->audio_ref_time == -1 && cdata.time != -1 ) {
						s->audio_ref_time = cdata.time;
						s->audio_samples  = 0;
						_set_audio_time( s, cdata.time );
DBGA serprintf(" [[%d]] ", s->audio_ref_time);
					}
				} else {
					if( cdata.time != -1 ) {
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

		AUDIO_FRAME audio_frame = { 0 };
		int decoded = 0;

		audio_frame.time = s->audio_time;

		if( s->audio_dec && !passthrough ) {
			// let the codec overwrite that
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
				_add_audio_time( s, decoded * 1000 / s->audio->bytesPerSec );
			}
		}
		
		if( s->dump_pcm_fd > 0 ) {
			file_write( s->dump_pcm_fd, audio_frame.data, audio_frame.size );
		}

		if( s->audio_sink ) {
			if( !audio_frame.error ) { 
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

					if( s->audio_filter && !passthrough) {
						s->audio_filter->filter( s->audio_filter, &audio_frame );
					}

					int size_written = s->audio_sink->write( s, &audio_frame );

					if( s->sync_mode == STREAM_SYNC_SAMPLES && audio_frame.size && s->audio_ref_time != -1 ) {
						// add the samples and calc new time
						if( s->audio->samplesPerSec ) {
							s->audio_samples += ((passthrough==2)?audio_frame.fakeSize:audio_frame.size) / s->audio->bytesPerFrame;
							int delta = (UINT64)1000 * (UINT64)s->audio_samples / (UINT64)s->audio->samplesPerSec;
							_set_audio_time( s, s->audio_ref_time + delta );
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
	
	int audio_format = -1;
	while( thread_state_get( &s->audio_tstate ) != THREAD_EXIT ) {
		if(s->audio->format != audio_format) {
			audio_format = s->audio->format;
#ifdef CONFIG_SPDIF
			if(spdif_init(s->audio) && s->audio_sink) {
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

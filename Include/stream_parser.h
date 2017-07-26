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

#ifndef _STREAM_PARSER_H
#define _STREAM_PARSER_H

struct STREAM_CHUNK;
struct STREAM_BUFFER;
struct STREAM;

typedef struct STREAM_PARSER_STATS {
	struct STREAM_BUFFER *buffer;
	int buffer_used;
	int buffer_size;

	struct STREAM_BUFFER *buffer2;
	int buffer2_used;
	int buffer2_size;

	int audio_chunks;
	int video_chunks;
	int sub_chunks;
	
	int atime_parsed;
	int vtime_parsed;
	
	int acurrent_rate;
	int vcurrent_rate;
			
} STREAM_PARSER_STATS;

STREAM_PARSER_STATS *stream_parser_get_stats( struct STREAM *s, struct STREAM_PARSER_STATS *stats );
int stream_parser_find_key_frame( struct STREAM *s, int max_time, int *time );
int stream_parser_drop_video( struct STREAM *s, int time );

#endif

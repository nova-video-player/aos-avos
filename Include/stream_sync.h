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

#ifndef _STREAM_SYNC_H
#define _STREAM_SYNC_H

int  stream_sync_init( STREAM *s, int time );
int  stream_sync_restart( STREAM *s );
int  stream_sync_av_delay( STREAM *s );
int  stream_sync_audio( STREAM *s, int audio_time );
int  stream_sync_video( STREAM *s, int video_time );
void stream_sync( STREAM *s );

#endif

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

#ifndef _STREAM_Q_H_
#define _STREAM_Q_H_

typedef struct STREAM_Q_STR STREAM_Q;

STREAM_Q *stream_q_new( int length, int entry_size );
void stream_q_delete( STREAM_Q **q );
void stream_q_flush( STREAM_Q *q );
int stream_q_put( STREAM_Q *q, void *entry ); 
int stream_q_get( STREAM_Q *q, void *entry );
int stream_q_get_wait( STREAM_Q *q, void *entry, int timeout );

#endif

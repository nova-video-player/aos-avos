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

#ifndef _FRAME_Q_
#define _FRAME_Q_

#include "av.h"

// generic video frame queue
typedef struct FRAME_Q {
	char tag[16];
	VIDEO_FRAME *head;
} FRAME_Q;

int  frame_q_init ( FRAME_Q *q, char *tag );
int  frame_q_put  ( FRAME_Q *q, VIDEO_FRAME *frame );
void frame_q_put_head( FRAME_Q *q, VIDEO_FRAME *frame );
void frame_q_flush( FRAME_Q *q );
int  frame_q_count( FRAME_Q *q );
void frame_q_dump ( FRAME_Q *q );
void frame_q_dump2( FRAME_Q *q );
VIDEO_FRAME *frame_q_peek( FRAME_Q *q );
VIDEO_FRAME *frame_q_get ( FRAME_Q *q );
VIDEO_FRAME *frame_q_get_unlocked( FRAME_Q *q );
VIDEO_FRAME *frame_q_get_index( FRAME_Q *q, int index );

#endif

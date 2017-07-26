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

#ifndef MESSAGE_QUEUE_H
#define MESSAGE_QUEUE_H

#include "message.h"
#include <types.h>


// Struct describing a queue based on a ringbuffer.
struct message_queue_str {
	message *data;
	int size;
	int putpos;
	int getpos;
	int num;
};

typedef struct message_queue_str message_queue;

void     MessageQueue_Clear( message_queue *q );
BOOL     MessageQueue_Put( message_queue *q, const message *msg );
BOOL     MessageQueue_Get( message_queue *q, message *msg );
void     MessageQueue_Dump( const message_queue *q );
message* MessageQueue_Peek( message_queue *q );
int 	 MessageQueue_GetCount( message_queue *q );

void     	MessageQueue_Destroy( message_queue *q );
message_queue  *MessageQueue_Create( int size );

#endif // MESSAGE_QUEUE_H

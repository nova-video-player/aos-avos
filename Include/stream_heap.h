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

#ifndef _STREAM_HEAP_H
#define _STREAM_HEAP_H

int  stream_heap_create( unsigned long size );
void stream_heap_destroy( void );
void *stream_heap_alloc( size_t size );
void *stream_heap_realloc( void *ptr, size_t size );
void stream_heap_free( void *ptr );

#endif

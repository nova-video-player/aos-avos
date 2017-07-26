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

#ifndef THUMB_STREAM
#define THUMB_STREAM

typedef struct  thumb_stream_t thumb_stream_t;

thumb_stream_t*	thumb_stream_create();
IMAGE*		thumb_stream_get_frame(thumb_stream_t *thumb_stream, STREAM_URL *src, int etype, int thumb_time, int colorspace, int *rotation);
void		thumb_stream_destroy(thumb_stream_t *thumb_stream);

#endif // THUMB_STREAM

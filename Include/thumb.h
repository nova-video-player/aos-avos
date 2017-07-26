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

#ifndef _THUMB_H
#define _THUMB_H

#include "image.h"
#include "stream_io.h"

IMAGE *thumb_get_image_from_url(STREAM_URL *src, int etype, int *stream_error, int thumb_time, int option);
IMAGE *thumb_get_image_from_frame(VIDEO_FRAME *frame);

#endif	// _THUMB_H

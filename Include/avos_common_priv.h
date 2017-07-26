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

#ifndef AVOS_COMMON_PRIV_H
#define AVOS_COMMON_PRIV_H

#include "avos_common.h"
#include "image.h"

avos_msg_t *avos_msg_new_str(uint32_t id, const char *text);
avos_msg_t *avos_msg_new_text_subtitle(uint32_t id, uint32_t position, uint32_t duration, const char *text);
avos_msg_t *avos_msg_new_bitmap_subtitle(uint32_t id, uint32_t position, uint32_t duration, IMAGE *img);

int avos_metadata_write_begin(metadata_buffer_t *buffer);
int avos_metadata_write_end(metadata_buffer_t *buffer); /* return 1 if metadata changed */
int avos_metadata_append_int(metadata_buffer_t *buffer, uint32_t id, int32_t val);
int avos_metadata_append_int64(metadata_buffer_t *buffer, uint32_t id, int64_t val);
int avos_metadata_append_bool(metadata_buffer_t *buffer, uint32_t id, int32_t val);
int avos_metadata_append_str(metadata_buffer_t *buffer, uint32_t id, const char *str);

int avos_metadata_read_begin(metadata_buffer_t *buffer);
int avos_metadata_read_end(metadata_buffer_t *buffer);
avos_msg_t *avos_metadata_read_next(metadata_buffer_t *buffer);
avos_msg_t *avos_metadata_get(metadata_buffer_t *buffer, uint32_t id);
ssize_t avos_metadata_copyfd(int fd, metadata_buffer_t *buffer);
ssize_t avos_metadata_readfd(int fd, metadata_buffer_t **pbuffer);
metadata_buffer_t *avos_metadata_dup(metadata_buffer_t *buffer);

const avos_metadata_handle_t *avos_metadata_get_handle();

metadata_buffer_t *avos_metadata_create();
void avos_metadata_destroy(metadata_buffer_t **buffer);

#endif // AVOS_COMMON_PRIV_H

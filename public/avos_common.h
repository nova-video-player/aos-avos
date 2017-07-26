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

#ifndef AVOS_COMMON_H
#define AVOS_COMMON_H

#include <stdint.h>

enum avos_error_type {
	AVOS_ERR_OK = 0,
	AVOS_ERR = -1,
	AVOS_ERR_CRITICAL = -2,
};

typedef struct avos_msg {
	uint32_t id;
	uint32_t type;
	uint32_t size;
	uint8_t data[];
} avos_msg_t __attribute__((aligned(1)));

typedef struct avos_bgra_bitmap {
	uint32_t width;
	uint32_t height;
	uint32_t linestep;
	uint32_t data_size;
	uint32_t rotation;
	uint8_t *data;
} avos_bgra_bitmap_t __attribute__((aligned(1)));

typedef struct avos_text_subtitle {
	uint32_t position;
	uint32_t duration;
	char text[];
} avos_text_subtitle_t __attribute__((aligned(1)));

typedef struct avos_bitmap_subtitle {
	avos_bgra_bitmap_t bitmap;
	uint32_t position;
	uint32_t duration;
	uint32_t orig_width;
	uint32_t orig_height;
	uint8_t data[];
} avos_bitmap_subtitle_t __attribute__((aligned(1)));

typedef struct avos_apic {
	uint32_t size;
	uint8_t data[];
} avos_apic_t __attribute__((aligned(1)));

enum {
	AVOS_MSG_TYPE_INT = 0,
	AVOS_MSG_TYPE_INT64,
	AVOS_MSG_TYPE_BOOL,
	AVOS_MSG_TYPE_STR,
	AVOS_MSG_TYPE_BYTE,
	AVOS_MSG_TYPE_BITMAP,
	AVOS_MSG_TYPE_TEXT_SUBTITLE,
	AVOS_MSG_TYPE_BITMAP_SUBTITLE,
};

typedef struct metadata_buffer metadata_buffer_t __attribute__((aligned(1)));

typedef struct avos_metadata_handle {
	int (*read_begin)		(metadata_buffer_t *buffer);
	int (*read_end)			(metadata_buffer_t *buffer);
	avos_msg_t *(*read_next)	(metadata_buffer_t *buffer);
	avos_msg_t *(*get)		(metadata_buffer_t *buffer, uint32_t id);
	void (*destroy)			(metadata_buffer_t **pbuffer);
	metadata_buffer_t *(*dup)	(metadata_buffer_t *buffer);
	ssize_t (*copyfd)		(int fd, metadata_buffer_t *buffer);
	ssize_t (*readfd)		(int fd, metadata_buffer_t **pbuffer);
	uint8_t *(*data)		(metadata_buffer_t *buffer);
	size_t (*size)			(metadata_buffer_t *buffer);
} avos_metadata_handle_t;

const avos_metadata_handle_t *avos_metadata_get_handle();

#endif // AVOS_COMMON_H

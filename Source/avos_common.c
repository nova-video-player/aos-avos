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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "global.h"
#include "debug.h"

#include "avos_common_priv.h"

#define METADATA_BUFFER_SIZE 1024

struct metadata_buffer {
	uint8_t *data;
	size_t data_size;
	size_t write_off;
	size_t read_off;
	uint8_t hash[16];
};

static avos_msg_t *avos_msg_new(uint32_t id, uint32_t type, uint32_t size)
{
	uint8_t *data = (uint8_t *)calloc(1, sizeof(avos_msg_t) + size);
	if (!data)
	    return NULL;
	avos_msg_t *msg = (avos_msg_t *) data;
	msg->id = id;
	msg->type = type;
	msg->size = size;
	return msg;
}

avos_msg_t *avos_msg_new_str(uint32_t id, const char *text)
{
	avos_msg_t *msg = avos_msg_new(id, AVOS_MSG_TYPE_STR, strlen(text) + 1);
	if (!msg)
		return NULL;
	memcpy(msg->data, text, strlen(text) + 1);
	return msg;
}

avos_msg_t *avos_msg_new_text_subtitle(uint32_t id, uint32_t position, uint32_t duration, const char *text)
{
	avos_msg_t *msg;
	avos_text_subtitle_t *sub;

	msg = avos_msg_new(id, AVOS_MSG_TYPE_TEXT_SUBTITLE, sizeof(avos_text_subtitle_t) + strlen(text) + 1);
	if (!msg)
		return NULL;
	sub = (avos_text_subtitle_t *) msg->data;
	sub->position = position;
	sub->duration = duration;
	memcpy(sub->text, text, strlen(text) + 1);
	return msg;
}

static void grayscale_to_argb888(int8_t *src, uint32_t src_width, uint32_t src_height, uint32_t src_linestep, int *dst)
{
    int8_t *src_end = src + src_linestep * src_height;
    int val;
    while (src < src_end) {
        int8_t *line = src;
	uint32_t y;
        for (y = 0; y < src_width; ++y) {
            val = *line < 0 ? *line + 0xff : *line;
            *(dst++) = val == 0 ? 0 : ((0xff)<<24)|(val<<16)|(val<<8)|val;
            ++line;
        }
        src += src_linestep;
    }
}

avos_msg_t *avos_msg_new_bitmap_subtitle(uint32_t id, uint32_t position, uint32_t duration, IMAGE *img)
{
	avos_msg_t *msg;
	avos_bitmap_subtitle_t *sub;
	IMAGE cropped = image_crop(img, &img->window);
	int rgba_size = cropped.width * cropped.height * 4;

	msg = avos_msg_new(0, AVOS_MSG_TYPE_BITMAP_SUBTITLE, sizeof(avos_bitmap_subtitle_t) + rgba_size);
	if (!msg)
		return NULL;
	sub = (avos_bitmap_subtitle_t *) msg->data;
	sub->position = position;
	sub->duration = duration;
	sub->orig_width = img->width;
	sub->orig_height = img->height;
	sub->bitmap.width = cropped.width;
	sub->bitmap.height = cropped.height;
	sub->bitmap.linestep = cropped.width;
	sub->bitmap.data_size = rgba_size;
	grayscale_to_argb888((int8_t *)cropped.data[0], cropped.width, cropped.height, cropped.linestep[0], (int *)sub->data);
	sub->bitmap.data = sub->data;
	return msg;
}

static int avos_metadata_buffer_realloc(metadata_buffer_t *buffer, size_t size)
{
	if (buffer->write_off + size > buffer->data_size) {
		buffer->data_size += METADATA_BUFFER_SIZE;
		buffer->data = realloc(buffer->data, buffer->data_size);
		if (!buffer->data) {
			buffer->data_size = 0;
			buffer->write_off = 0;
			return -1;
		}
	}
	return 0;
}

static int avos_metadata_write(metadata_buffer_t *buffer, uint32_t id, uint32_t type, uint8_t *data, size_t data_size)
{
	if (avos_metadata_buffer_realloc(buffer, sizeof(avos_msg_t) + data_size) == -1)
		return -1;
	avos_msg_t *metadata = (avos_msg_t *)(buffer->data + buffer->write_off);

	metadata->id = id;
	metadata->type = type;
	metadata->size = data_size;
	memcpy(metadata->data, data, data_size);
	buffer->write_off += sizeof(avos_msg_t) + data_size;
	return 0;
}

int avos_metadata_append_int(metadata_buffer_t *buffer, uint32_t id, int32_t val)
{
	return avos_metadata_write(buffer, id, AVOS_MSG_TYPE_INT, (uint8_t *)&val, sizeof(int32_t));
}
int avos_metadata_append_int64(metadata_buffer_t *buffer, uint32_t id, int64_t val)
{
	return avos_metadata_write(buffer, id, AVOS_MSG_TYPE_INT64, (uint8_t *)&val, sizeof(int64_t));
}
int avos_metadata_append_bool(metadata_buffer_t *buffer, uint32_t id, int32_t val)
{
	return avos_metadata_write(buffer, id, AVOS_MSG_TYPE_BOOL, (uint8_t *)&val, sizeof(int32_t));
}

int avos_metadata_append_str(metadata_buffer_t *buffer, uint32_t id, const char *str)
{
	return str && str[0] ? avos_metadata_write(buffer, id, AVOS_MSG_TYPE_STR, (uint8_t *)str, strlen(str) + 1) : 0;
}

int avos_metadata_write_begin(metadata_buffer_t *buffer)
{
	buffer->write_off = 0;
	return 0;
}

int avos_metadata_write_end(metadata_buffer_t *buffer)
{
	return 1;
}

int avos_metadata_read_begin(metadata_buffer_t *buffer)
{
	buffer->read_off = 0;
	return 0;
}

int avos_metadata_read_end(metadata_buffer_t *buffer)
{
	return buffer->read_off;
}

avos_msg_t *avos_metadata_get(metadata_buffer_t *buffer, uint32_t id)
{
	avos_msg_t *msg = NULL;
	uint8_t *cur = buffer->data, *end = buffer->data + buffer->write_off;

	while (cur < end) {
		avos_msg_t *msg_cur = (avos_msg_t *) cur;
		if (msg_cur->id == id) {
			msg = msg_cur;
			break;
		}
		cur += sizeof(avos_msg_t) + msg_cur->size;
	}
	return msg;
}

avos_msg_t *avos_metadata_read_next(metadata_buffer_t *buffer)
{
	if (buffer->read_off >= buffer->write_off)
		return NULL;
	avos_msg_t *msg = (avos_msg_t *)(buffer->data + buffer->read_off);
	buffer->read_off += msg->size + sizeof(avos_msg_t);
	return msg;
}

ssize_t avos_metadata_copyfd(int fd, metadata_buffer_t *buffer)
{
	ssize_t len = 0, ret;

	ret = write(fd, buffer, sizeof(metadata_buffer_t));
	if (ret == -1)
		return -1;
	len += ret;
	ret = write(fd, buffer->data, buffer->data_size);
	if (ret == -1)
		return -1;
	len += ret;
	return len;
}

ssize_t avos_metadata_readfd(int fd, metadata_buffer_t **pbuffer)
{
	ssize_t len = 0, ret;
	metadata_buffer_t *buffer;
	
	buffer = malloc(sizeof(metadata_buffer_t));
	ret = read(fd, buffer, sizeof(metadata_buffer_t));
	if (ret == -1 || buffer->data_size == 0)
		goto err;
	len += ret;
	buffer->data = malloc(buffer->data_size);
	if (!buffer->data)
		goto err;
	ret = read(fd, buffer->data, buffer->data_size);
	if (ret == -1)
		goto err;
	len += ret;

	*pbuffer = buffer;
	return len;
err:
	if (buffer) {
		if (buffer->data)
			free(buffer->data);
		free(buffer);
	}
	return -1;
}

metadata_buffer_t *avos_metadata_dup(metadata_buffer_t *buffer)
{
	metadata_buffer_t *dup;
	
	dup = malloc(sizeof(metadata_buffer_t));
	if (!dup)
		goto err;
	memcpy(dup, buffer, sizeof(metadata_buffer_t));
	dup->data = malloc(buffer->data_size);
	if (!dup->data)
		goto err;
	memcpy(dup->data, buffer->data, buffer->data_size);
	return dup;
err:
	if (dup) {
		if (dup->data)
			free(dup->data);
		free(dup);
	}
	return NULL;
}

uint8_t *avos_metadata_data(metadata_buffer_t *buffer)
{
	return buffer->data;
}

size_t avos_metadata_size(metadata_buffer_t *buffer)
{
	return buffer->write_off;
}

metadata_buffer_t *avos_metadata_create()
{
	return calloc(1, sizeof(metadata_buffer_t));
}

void avos_metadata_destroy(metadata_buffer_t **pbuffer)
{
	metadata_buffer_t *buffer = *pbuffer;
	*pbuffer = NULL;
	if (buffer->data)
		free(buffer->data);
	free(buffer);
}

static const avos_metadata_handle_t avos_metadata_handle = {
	.read_begin = avos_metadata_read_begin,
	.read_end = avos_metadata_read_end,
	.read_next = avos_metadata_read_next,
	.get = avos_metadata_get,
	.destroy = avos_metadata_destroy,
	.dup = avos_metadata_dup,
	.copyfd = avos_metadata_copyfd,
	.readfd = avos_metadata_readfd,
	.data = avos_metadata_data,
	.size = avos_metadata_size
};

const avos_metadata_handle_t *avos_metadata_get_handle()
{
	return &avos_metadata_handle;
}

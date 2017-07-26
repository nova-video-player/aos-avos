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

#ifndef MEDIA_SERVER_H
#define MEDIA_SERVER_H

#include <stdint.h>
#include <sys/queue.h>
#include <pthread.h>

#include "dataevent.h"
#include "file_info.h"
#include "stream_io.h"

#define UPNP_FUSE_TO_HTTP

#define CLIENT_FLAG_DISCONNECTED	0x01
#define CLIENT_FLAG_CLOSING		0x02

struct media_task;
typedef int (*media_task_cb)(struct media_task *task);

struct media_client_timer {
	struct media_client	*client;
	int			timer;
	media_task_cb		cb;
	TAILQ_ENTRY(media_client_timer)	next;
};
typedef TAILQ_HEAD(media_client_timer_queue, media_client_timer) MEDIA_CLIENT_TIMER_QUEUE_HEAD;

struct media_player_client;
struct media_retriever_client;

struct media_client {
	uint32_t			type;
	data_event_t			event;
	int				timer;
	pthread_mutex_t			flag_mutex;
	int				flag;
	int				shared_fd;
	STREAM_URL			src;
	pthread_mutex_t			send_mutex;
	TAILQ_ENTRY(media_client)	next;
	int				is_shared;
	uint32_t			shared_id;
	void				*media;
};
typedef TAILQ_HEAD(media_client_queue, media_client) MEDIA_CLIENT_QUEUE_HEAD;

struct media_task {
	struct media_client		*client;
	media_task_cb			cb;
	struct avos_msg_header		*header;
	uint8_t				*data;
	int				cancel;
	TAILQ_ENTRY(media_task)		next;
};
typedef TAILQ_HEAD(media_task_queue, media_task) MEDIA_TASK_QUEUE_HEAD;

struct metadata_buffer {
	uint8_t *data;
	size_t size;
};

int media_client_set_data_source(struct media_task *task);
int media_client_send_msg(struct media_client *client, struct avos_msg_header *header, uint8_t *data);
int media_client_add_timeout_cb(struct media_client *client, media_task_cb cb, int timeout);
int media_client_remove_timeout(struct media_client *client, int *id);
int media_client_is_aborting(struct media_client *client);
int media_client_is_disconnected(struct media_client *client);

#ifdef UPNP_FUSE_TO_HTTP
int upnp_fuse_to_http(struct media_client *client, const char *file_url, char *http_url, int max);
#endif
#endif

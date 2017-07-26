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

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include "threadcom.h"
#include "debug.h"
#include "astdlib.h"
#include "dataevent.h"
#include "global.h"

#define ERR	if (1)
#define DBG	if (0)

struct threadcom_link {
	data_event_t		de;
	int 			pipe[2];
	threadcom_event_t	event;
};

static void _event_callback(void* context) 
{
	threadcom_link_t* link = context;
	link->event(link);
}

static inline int _post_event(threadcom_link_t* link, const void* data, ssize_t len)
{
	int ret;
	
	/* write the length first */
	ret = write(link->pipe[1], &len, sizeof(len));
	if (ret < 0) {
ERR		serprintf("threadcom_post_event: write failed (%s)\n", strerror(errno));
		return 1;
	}
		
	/* then write the data portion */
	ret = write(link->pipe[1], data, len);
	if (ret < 0) {
ERR		serprintf("threadcom_post_event: write failed (%s)\n", strerror(errno));
		return 1;
	}

	return 0;
}

static inline int _get_event(threadcom_link_t* link, void* data, ssize_t len)
{
	int ret;
	size_t msg_len;
	
	/* 
	 * read the length of the message. The read is non-blocking, 
	 * so that we can exit without blocking if no message is in the pipe.
	 * however, the rest of the function relies on blocking reads, so we
	 * undo the O_NONBLOCK immediately.
	 */
	fcntl(link->pipe[0], F_SETFL, fcntl(link->pipe[0], F_GETFL)|O_NONBLOCK);
	ret = read(link->pipe[0], &msg_len, sizeof(msg_len));
	fcntl(link->pipe[0], F_SETFL, fcntl(link->pipe[0], F_GETFL)& ~O_NONBLOCK);
	if (ret < 0) {
		/* if the pipe is empty, exit immediately, however this is not an
		 * error condition.*/
		if (errno == EAGAIN)
			return 1;

ERR		serprintf("threadcom_get_event: read failed (%s)\n", strerror(errno));
		return 1;
	}

	/* compare the sizes */
	if (len < msg_len) {
		char tmp;
ERR		serprintf("threadcom_get_event: insufficient space for event data (%i < %i)\n",
				len, msg_len);
				
		/* sigh. but it's better than falling out of format */
		while (msg_len--)
			read(link->pipe[0], &tmp, 1);

		return 1;
	}
	
	ret = read(link->pipe[0], data, msg_len);
	if (ret < 0) {
ERR		serprintf("threadcom_get_event: read failed (%s)\n", strerror(errno));
		return 1;
	}

	return 0;
}

/**
 *	__threadcom_init
 */
threadcom_link_t* __threadcom_init(threadcom_link_t* link, threadcom_event_t event, data_event_t *data_events, const char* from)
{
	memset(link, 0, sizeof(*link));
	
	if (pipe(link->pipe) < 0) {
ERR		serprintf("threadcom_create: error creating pipe handles: %s\n", strerror(errno));
		goto out;
	}
	
	/* we've got the pipe set up, register the data events now */
	link->de.fd = link->pipe[0];
	link->de.read_func = _event_callback;
	link->event = event;
	if (__register_data_event(data_events, &link->de, link, from)) {
ERR		serprintf("threadcom_create: error registering data event\n");
		goto out_close;
	}

	return link;

 out_close:
 	close(link->pipe[0]);
 	close(link->pipe[1]);
 out:
 	return NULL;
}

/**
 *	threadcom_create
 */
threadcom_link_t* __threadcom_create(threadcom_event_t event, data_event_t *data_events, const char* from)
{
	threadcom_link_t* link = amalloc(sizeof(*link));
	if (link == NULL) {
ERR		serprintf("threadcom_create: cannot allocate memory for link\n");
		return NULL;
	}

	if (__threadcom_init(link, event, data_events, from) == NULL) {
		afree(link);
		return NULL;
	}

	return link;
}

/**
 *	__threadcom_release
 */
int __threadcom_release(threadcom_link_t* link)
{
	if (link == NULL)
		return 1;

	unregister_data_event(&link->de);
	
	close(link->pipe[0]);
	close(link->pipe[1]);

	return 0;
}

/**
 *	threadcom_destroy
 */
int threadcom_destroy(threadcom_link_t* link)
{
	if (__threadcom_release(link))
		return 1;
		
	afree(link);
	
	return 0;
}

/**
 *	threadcom_post_event
 */
int threadcom_post_event(threadcom_link_t* link, const void* data, ssize_t length)
{
DBG	serprintf("threadcom_post_event(link:%p data:%p len:%i)\n",
			link, data, length);

	if (!link || !data || !length) {
DBG		serprintf("threadcom_post_event: no link or data\n");
DBG		Trace();
		return 1;
	}

	return _post_event(link, data, length);
}

/**
 *	threadcom_get_event
 */
int threadcom_get_event(threadcom_link_t* link, void* data, ssize_t length)
{
DBG	serprintf("threadcom_get_event(link:%p, data:%p, len:%i)\n",
			link, data, length);

	if (!link || !data || !length) {
DBG		serprintf("threadcom_get_event: no link or data\n");
DBG		Trace();
		return 1;
	}

	return _get_event(link, data, length);
}

/**
 *	threadcom_post_string
 */
int threadcom_post_string(threadcom_link_t* link, const char* s)
{
DBG	serprintf("threadcom_post_string(link:%p, s:%p)\n", link, s);

	if (!link || !s) {
DBG		serprintf("threadcom_post_string: no link or data\n");
		return 1;
	}
	
	return _post_event(link, s, strlen(s)+1);
}

/**
 *	threadcom_get_string
 */
int threadcom_get_string(threadcom_link_t* link, char* s, ssize_t maxlen)
{
DBG	serprintf("threadcom_get_string(link:%p, s:%p, len:%i)\n", link, s, maxlen);

	if (!link || !s || !maxlen) {
DBG		serprintf("threadcom_get_string: no link or data\n");
		return 1;
	}
	
	return _get_event(link, s, maxlen);
}

/* below: test code only */
#ifdef CONFIG_DEBUG_THREADCOM
#include <pthread.h>
#include <stdio.h>
#include "athread.h"
static threadcom_link_t* test_link;
static pthread_t test_thread_handle;

static void test_event(threadcom_link_t* link)
{
	char data[256];

	threadcom_get_string(link, data, sizeof(data));
	serprintf("test_event: %s\n", data);
	
	if (!strcmp(data, "EOF")) {
		apthread_join(test_thread_handle, NULL);
		threadcom_destroy(test_link);
		serprintf("test_event: threadcom destroyed\n");
	}
}

static void* test_thread(void* dummy)
{
	int i;
	char data[256];

	serprintf("test_thread enter\n");
	
	for (i = 0; i < 100; i++) {
		serprintf("test_thread %i\n", i);
		sprintf(data, "%i", i);
		threadcom_post_string(test_link, data);
	}
	threadcom_post_string(test_link, "EOF");
	
	// thread dies here
	serprintf("test_thread_exit\n");

	return NULL;
}

void threadcom_test(void)
{
	test_link = threadcom_create(test_event);
	apthread_create(&test_thread_handle, NULL, test_thread, NULL, "threadcom_test_thread");
}

#endif // CONFIG_DEBUG_THREADCOM

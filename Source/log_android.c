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

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>

#include <unistd.h>
#include "global.h"
#include "log.h"
#include "androidndk_utils.h"

#ifdef DEBUG_MSG

#define LOG_TAG "avos"

#define BUF_SIZE 1024

static const char *tag = NULL;
static int fildes[2] = {-1, -1};
static FILE *out = NULL, *in = NULL;
static int old_stdout = -1, old_stderr = -1;
static pthread_t thread;

static void *log_thread(void *arg)
{
	size_t len, tocpy;
	const char *ln;
	char buf[BUF_SIZE + 1];

	/* send log line by line to android */
	while (in && (ln = fgetln(in, &len)) != NULL) {
		while (len > 0) {
			tocpy = len > BUF_SIZE ? BUF_SIZE : len;
			memcpy(buf, ln, tocpy);
			buf[tocpy] = '\0';
			android_log_print(ANDROID_LOG_DEBUG, tag, "%s", buf);
			len -= tocpy;
			ln += tocpy;
		}
	}
	return NULL;
}

void LOG_open_name(const char *name)
{
	if (pipe(fildes) == -1)
		return;
	tag = name;
	/*
	 * when run from android app, stdout and stdin are /dev/null
	 * in debug: also print stderr and stdout (make external libs more verbose)
	 */
	old_stdout = dup(STDOUT_FILENO);
	old_stderr = dup(STDERR_FILENO);
	dup2(fildes[1], STDOUT_FILENO);
	dup2(fildes[1], STDERR_FILENO);
	out = fdopen(fildes[1], "a");
	in = fdopen(fildes[0], "r");
	pthread_create(&thread, NULL, log_thread, NULL);
}

void LOG_open()
{
	LOG_open_name(NULL);
}

void LOG_close(void)
{
	if (old_stdout != -1 && old_stderr != -1) {
		dup2(old_stdout, STDOUT_FILENO);
		dup2(old_stderr, STDERR_FILENO);
		close(old_stdout);
		close(old_stderr);
	}
	fclose(out);
	fclose(in);
	fildes[0] = fildes[1] = old_stdout = old_stderr = -1;
	out = in = NULL;
	pthread_join(thread, NULL);
}

int vserprintf(const char *fmt, va_list va)
{
	int ret;

	if (!out)
		return 0;
	ret = vfprintf(out, fmt, va);
	fflush(out);
	return ret;
}

int serprintf(const char *fmt, ...)
{
	int ret;

	va_list ap;

	va_start (ap, fmt);
	ret = vserprintf(fmt, ap);
	va_end (ap);

	return ret;
}
#endif

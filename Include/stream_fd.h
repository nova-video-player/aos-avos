/*
 * Copyright 2026 Courville Software
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

#ifndef STREAM_FD_H
#define STREAM_FD_H

#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>

// Callers retain ownership of fd. Validate using subtraction so large ranges
// cannot overflow, and commit a new descriptor only after duplication succeeds.
static inline int stream_fd_duplicate(int fd, int64_t offset, int64_t *length)
{
	struct stat st;
	if (fd < 0 || offset < 0 || !length || *length < 0) {
		errno = EINVAL;
		return -1;
	}
	if (fstat(fd, &st)) return -1;
	if (!S_ISREG(st.st_mode) || st.st_size < 0 || offset > st.st_size) {
		errno = EINVAL;
		return -1;
	}
	int64_t remaining = (int64_t)st.st_size - offset;
	if (!*length || *length > remaining) *length = remaining;
	return dup(fd);
}

static inline int stream_fd_parse_url(const char *url, int *fd, int64_t *offset, int64_t *length)
{
	if (!url || strncmp(url, "fd://", 5)) return -1;
	const char *p = url + 5;
	int64_t values[3];
	for (int i = 0; i < 3; ++i) {
		if (*p < '0' || *p > '9') return -1;
		char *end;
		errno = 0;
		intmax_t value = strtoimax(p, &end, 10);
		if (errno || value < 0 || value > INT64_MAX ||
		    (i < 2 ? *end != ':' : *end != '\0')) return -1;
		values[i] = value;
		p = end + (i < 2);
	}
	if (values[0] > INT_MAX || values[1] > INT64_MAX - values[2]) return -1;
	*fd = values[0];
	*offset = values[1];
	*length = values[2];
	return 0;
}
#endif

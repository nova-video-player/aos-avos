/*
 * Copyright 2026 Courville Software
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "types.h"
#include "stream_io.h"

int main(void)
{
	char long_url[2049];
	const char *keys[] = { "Authorization", "Referer", NULL };
	const char *values[] = { "Bearer test-token", "https://example.test/", NULL };
	STREAM_URL source = STREAM_URL_INITIALIZER;
	STREAM_URL copy = STREAM_URL_INITIALIZER;

	memset(long_url, 'a', sizeof(long_url));
	memcpy(long_url, "https://example.test/", strlen("https://example.test/"));
	long_url[sizeof(long_url) - 1] = '\0';

	assert(stream_url_cpy_url_name_headers(&source, long_url, "Long URL", keys, values) == 0);
	assert(strlen(source.url) == strlen(long_url));
	assert(strcmp(source.extra_list[0], keys[0]) == 0);
	assert(strcmp(source.extra_list[1], values[0]) == 0);

	assert(stream_url_cpy(&copy, &source) == 0);
	assert(strcmp(copy.url, source.url) == 0);
	assert(copy.url != source.url);
	assert(copy.extra_list != source.extra_list);

	stream_url_clear(&source);
	assert(strlen(copy.url) == strlen(long_url));
	assert(strcmp(copy.extra_list[3], values[1]) == 0);
	stream_url_clear(&copy);

	puts("stream_url: OK");
	return 0;
}

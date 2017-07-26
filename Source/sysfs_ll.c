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

#include "global.h"
#include "debug.h"
#include "limits.h"
#include "types.h"

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>

#define DBG if (0)
#define ERR if (1)

int sysfs_ll_write_str(const char *param_path, const char *str)
{
	int fd, size = 0;
	int max = strnlen(str, 255);
	
DBG serprintf("%s: open \"%s\"\n", __FUNCTION__, param_path);
	if ((fd = open(param_path, O_WRONLY)) < 0) {
ERR serprintf("%s: open \"%s\" failed\n", __FUNCTION__, param_path);
		return -1;
	}
	
	while (size < max) {
		int len = write(fd, str+size, max-size);
		if (len < 0) {
			close(fd);
ERR serprintf("%s: write \"%s\" failed: %d(%s)\n", __FUNCTION__, param_path, errno, strerror(errno));
			return -1;
		} else if (len == 0)
			break;
	
		size += len;
	}
	
	close(fd);
	return size;
}

int sysfs_ll_read_str(const char *param_path, char* str, int max)
{
	int fd, size = 0;
DBG serprintf("%s: open \"%s\"\n", __FUNCTION__, param_path);
	memset( str, 0, max );

	if ((fd = open(param_path, O_RDONLY)) < 0) {
#ifndef SIM
ERR serprintf("%s: open \"%s\" failed: %d(%s)\n", __FUNCTION__, param_path, errno, strerror(errno));
#endif
		return -1;
	}
	
	for (;;) {
		int len = read(fd, str + size, max - size);
		if (len < 0) {
			close(fd);
ERR serprintf("%s: read \"%s\" failed\n", __FUNCTION__, param_path);
			return -1;
		} else if (len == 0) {
			break;
		}
		size += len;
	} 
	
	str[max - 1] = 0;

	close(fd);
	
	return size;
}

int sysfs_ll_write_int(const char *param_path, int value)
{
	char str[255+1];
	
DBG serprintf("%s, %s: %d\n", __FUNCTION__, param_path, value);

	(void)snprintf(str, 255, "%d", value);
	str[255] = 0;
	
	return sysfs_ll_write_str(param_path, str);
}

int sysfs_ll_read_int(const char *param_path, int *value)
{
	char str[255+1];
		
	if (sysfs_ll_read_str(param_path, str, 255) < 0)
		return -1;
		
	*value = strtol(str, NULL, 10);
DBG serprintf("%s, %s: %d\n", __FUNCTION__, param_path, *value);

	return 0;
}

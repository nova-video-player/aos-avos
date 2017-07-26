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

#ifndef _UEVENT_H
#define _UEVENT_H

#include <stdlib.h>

#define NETLINK_KOBJECT_UEVENT		15
#define HOTPLUG_BUFFER_SIZE		1024
#define OBJECT_SIZE			512
#define MAX_KOBJ_REF			5
#define HOTPLUG_NUM_ENVP		32

struct uevent_buffer {
	char		data[HOTPLUG_BUFFER_SIZE + OBJECT_SIZE];
	const char	*devpath;			// Sysfs path
	const char	*action;			// action
	const char	*envp[HOTPLUG_NUM_ENVP];	// Environment
	ssize_t 	buflen;
};

int  uevent_open( void );
int  uevent_close( void );

#endif // _UEVENT_H

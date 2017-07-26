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

#ifndef _ANDROID_UTILS_H
#define _ANDROID_UTILS_H

#include <cpu-features.h>
#include <android/log.h>
#include <sys/system_properties.h>

#define android_log_print __android_log_print
#define android_log_vprint __android_log_vprint
#define android_cpu_count android_getCpuCount

static inline int
android_property_get(const char *key, char *value, const char *default_value)
{
#ifdef HAVE_ANDROID_SYSTEM_PROP
	int ret = __system_property_get(key, value);
	
	if (!ret)
		 strncpy(value, default_value, PROP_VALUE_MAX);
	return ret;
#else
	return -1;
#endif
}

static inline int
android_has_property_get()
{
#ifdef HAVE_ANDROID_SYSTEM_PROP
	return 1;
#else
	return 0;
#endif
}
#endif // _ANDROID_UTILS_H

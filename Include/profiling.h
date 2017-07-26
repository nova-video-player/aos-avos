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

#ifndef __PROFILING_H
#define __PROFILING_H

#ifdef PROFILING

#define PROFILE_DECLARE(key) \
	static struct { \
		unsigned long time; \
		int enabled; \
	} __profile_##key; \
	static void __toggle_profile_##key(void) \
	{\
		__profile_##key.enabled = !__profile_##key.enabled; \
		serprintf("**PROFILE " #key " %s\n", __profile_##key.enabled?"ON":"OFF"); \
	}\
	DECLARE_DEBUG_COMMAND_VOID("pr_" #key, __toggle_profile_##key)
	
#define PROFILE_START(key) if (__profile_##key.enabled) { serprintf("<"#key); __profile_##key.time = atime(); }
#define PROFILE_STOP(key)  if (__profile_##key.enabled) { serprintf("[%lu]"#key">\n", atime() - __profile_##key.time); }
#define PROFILE_STOP_NOLF(key)  if (__profile_##key.enabled) { serprintf("[%lu]"#key">", atime() - __profile_##key.time); }
#define PROFILE_IF(key)    if (__profile_##key.enabled)

#else

#define PROFILE_DECLARE(key)
#define PROFILE_START(key) do {} while (0)
#define PROFILE_STOP(key) do {} while (0)
#define PROFILE_IF(key) if (0)
#define PROFILE_STOP_NOLF(key) do {} while (0)

#endif

#endif

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

#ifndef DLHELPER_HEADER
#error DLHELPER_HEADER not defined
#endif

#include <pthread.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define DLHELPER_NOCRIT 0
#define DLHELPER_CRIT 1

#undef LOG
#define LOG(fmt, ...) do { \
	printf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
	fflush(stdout); \
} while (0)

#define DLHELPER_CFG
#include DLHELPER_HEADER
#undef DLHELPER_CFG

#if __cplusplus
extern "C" {
#endif

#define DLHELPER_REG(crit, function, ret, args, ...) \
typedef ret (* function##_t) args
#define DLHELPER_REG_VAR(crit, name, type, ...)
#include DLHELPER_HEADER
#undef DLHELPER_REG
#undef DLHELPER_REG_VAR

static struct {
	pthread_mutex_t lock;
	int state;
	void *handle;
#define DLHELPER_REG(crit, function, ret, args, ...) \
	function##_t function
#define DLHELPER_REG_VAR(crit, name, type, ...) \
	type name
#include DLHELPER_HEADER
#undef DLHELPER_REG
#undef DLHELPER_REG_VAR
} DLHELPER_STRUCT = {
	PTHREAD_MUTEX_INITIALIZER,
	-1,
	NULL,
};

#ifndef DLHELPER_DLSYM
static int
dlhelper_dlsym(void *handle, void **pdst, int numsyms, ...)
{
	va_list ap;
	void *sym = NULL;
	const char *err = NULL;

	dlerror();

	va_start(ap, numsyms);
	while (numsyms--) {
		const char *arg = va_arg(ap, const char *);
		sym = dlsym(handle, arg);
		err = dlerror();
		if (!err)
			break;
		LOG("dlhelper_dlsym failed: %s", err);
	}
	va_end(ap);
	*pdst = sym;
	return err != NULL ? -1 : 0;
}
#define DLHELPER_DLSYM
#endif

#define DLHELPER_CLEANUP() do { \
	if (DLHELPER_STRUCT.handle != RTLD_DEFAULT && DLHELPER_STRUCT.handle != NULL) \
		dlclose(DLHELPER_STRUCT.handle); \
	memset(&DLHELPER_STRUCT, 0, sizeof(DLHELPER_STRUCT)); \
} while(0)

static int DLHELPER_INIT()
{
	pthread_mutex_lock(&DLHELPER_STRUCT.lock);
	if (DLHELPER_STRUCT.state != -1)
		goto end;

#ifdef DLHELPER_HANDLE
	DLHELPER_STRUCT.handle = dlopen(DLHELPER_HANDLE, RTLD_NOW);
	if (!DLHELPER_STRUCT.handle) {
		LOG("sym_init: dlopen failed: %s", DLHELPER_HANDLE);
		goto end;
	}
#else
	DLHELPER_STRUCT.handle = RTLD_DEFAULT;
#endif

#define NUMARGS(...)  (sizeof((const void *[]){__VA_ARGS__})/sizeof(const void*))
#define DLHELPER_REG(crit, function, ret, args, ...) do { \
	if (dlhelper_dlsym(DLHELPER_STRUCT.handle, (void **)&DLHELPER_STRUCT.function, NUMARGS(__VA_ARGS__), __VA_ARGS__)) { \
		LOG("sym_init: dlsym failed: %s\n", #function); \
		if (crit == DLHELPER_CRIT) { \
			DLHELPER_CLEANUP(); \
			goto end; \
		} \
	} \
} while(0)

#define DLHELPER_REG_VAR(crit, name, type, ...) do { \
	if (dlhelper_dlsym(DLHELPER_STRUCT.handle, (void **)&DLHELPER_STRUCT.name, NUMARGS(__VA_ARGS__), __VA_ARGS__)) { \
		LOG("sym_init: dlsym failed: %s\n", #name); \
		if (crit == DLHELPER_CRIT) { \
			DLHELPER_CLEANUP(); \
			goto end; \
		} \
	} \
} while(0)

#include DLHELPER_HEADER
#undef DLHELPER_REG
#undef DLHELPER_REG_VAR

	DLHELPER_STRUCT.state = 1;
end:
	pthread_mutex_unlock(&DLHELPER_STRUCT.lock);
	return DLHELPER_STRUCT.state != 1;
}

__attribute__((unused)) static void DLHELPER_DEINIT()
{
	pthread_mutex_lock(&DLHELPER_STRUCT.lock);
	DLHELPER_CLEANUP();
	DLHELPER_STRUCT.state = -1;
	pthread_mutex_unlock(&DLHELPER_STRUCT.lock);
}
#if __cplusplus
}
#endif

#undef DLHELPER_HEADER
#undef DLHELPER_INIT
#undef DLHELPER_DEINIT
#undef DLHELPER_STRUCT
#undef DLHELPER_HANDLE

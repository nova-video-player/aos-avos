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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

#include <hardware/gralloc.h>
#include <system/window.h>

#include "android_buffer.h"

#include <android/native_window.h>

#define NO_ERROR 0
typedef int32_t status_t;

#define MEM_OPTIM_HACK
#define SET_TILER_OPTIM_SYM "_ZN7android21SurfaceComposerClient20setTilerOptimisationEi"

#define GRALLOC_USAGE_FIX_VIDEO_ASPECT 0x04000000

#define AVOSLOG(fmt, ...) do { printf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); fflush(stdout); } while (0)

#define CHECK_ERR() do { if (err != NO_ERROR) { AVOSLOG("error in %s  line %d\n", __FUNCTION__, __LINE__ ); return -1; } } while (0)

struct android_surface {
	ANativeWindow *anw;
	ANativeWindow_Buffer *anbp;
	int buffer_type;
	int hal_format;
	int has_archos_enhancement;
	int usage;
	gralloc_module_t const* gralloc;
#ifdef MEM_OPTIM_HACK
	int mem_optim_hack;
#endif
};

#ifdef MEM_OPTIM_HACK
/*
 * hack that deactivate Tiler Optimsation with UI in order to have more space
 * available for HD video.
 * we catch crash/kill signals in order to restore the default behavior in case of crash.
 */

#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t tiler_optim_hack_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct {
	int ref_cnt;
	struct sigaction saterm;
	struct sigaction sasegv;
	struct sigaction safpe;
	struct sigaction sapipe;
	struct sigaction saill;
	struct sigaction sabus;
	int (*set_tiler_optim)(int);
} tiler_optim_hack;

static void mem_optim_hack_disable_locked()
{
	sigaction(SIGTERM, &tiler_optim_hack.saterm, NULL);
	sigaction(SIGSEGV, &tiler_optim_hack.sasegv, NULL);
	sigaction(SIGFPE, &tiler_optim_hack.safpe, NULL);
	sigaction(SIGPIPE, &tiler_optim_hack.sapipe, NULL);
	sigaction(SIGILL, &tiler_optim_hack.saill, NULL);
	sigaction(SIGBUS, &tiler_optim_hack.sabus, NULL);
	tiler_optim_hack.set_tiler_optim(1);
}

/*
 * proxy signal handler that restore tiler optim, restore previous signals
 * handler and rekill itself
 */
static void mem_optim_hack_signal_handler(const int signo)
{
	mem_optim_hack_disable_locked();
	kill(getpid(), signo);
}

static void mem_optim_hack_disable(android_surface_t *as)
{
	if (as->mem_optim_hack) {
		pthread_mutex_lock(&tiler_optim_hack_mtx);
		if (--tiler_optim_hack.ref_cnt == 0)
			mem_optim_hack_disable_locked();
		pthread_mutex_unlock(&tiler_optim_hack_mtx);
	}
}

static void mem_optim_hack_enable(android_surface_t *as)
{
	pthread_mutex_lock(&tiler_optim_hack_mtx);

	if (!tiler_optim_hack.set_tiler_optim) {
		tiler_optim_hack.set_tiler_optim = (int (*)(int))dlsym(RTLD_DEFAULT, SET_TILER_OPTIM_SYM);
		if (!tiler_optim_hack.set_tiler_optim) {
			pthread_mutex_unlock(&tiler_optim_hack_mtx);
			return;
		}
	}

	if (tiler_optim_hack.ref_cnt++ == 0) {
		struct sigaction sa;

		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sa.sa_handler = mem_optim_hack_signal_handler;
		if (sigaction(SIGTERM, &sa, &tiler_optim_hack.saterm) == 0 &&
				sigaction(SIGSEGV, &sa, &tiler_optim_hack.sasegv) == 0 &&
				sigaction(SIGFPE, &sa, &tiler_optim_hack.safpe) == 0 &&
				sigaction(SIGPIPE, &sa, &tiler_optim_hack.sapipe) == 0 &&
				sigaction(SIGILL, &sa, &tiler_optim_hack.saill) == 0 &&
				sigaction(SIGBUS, &sa, &tiler_optim_hack.sabus) == 0) {
			tiler_optim_hack.set_tiler_optim(0);
			as->mem_optim_hack = 1;
		}
	}
	pthread_mutex_unlock(&tiler_optim_hack_mtx);
	usleep(80000); // 80ms
}
#endif


android_surface_t *android_surface_create(void *surface_handle, int has_archos_enhancement)
{
	android_surface_t *as = NULL;
	ANativeWindow *anw = (ANativeWindow *)surface_handle;
	AVOSLOG("surface_handle: %p, anw: %p, archos_enhancement: %d", surface_handle, anw, has_archos_enhancement);

	as = (android_surface_t *) calloc(1, sizeof(android_surface_t));
	if (!as)
		goto err;

	as->anw = anw;
	as->has_archos_enhancement = has_archos_enhancement;
	if (as->anw->common.magic != ANDROID_NATIVE_WINDOW_MAGIC &&
			as->anw->common.version != sizeof(ANativeWindow))
		goto err;

	hw_module_t const* module;
	int (*hw_get_module) (const char *id, const struct hw_module_t **module) = NULL;
	dlerror();
	hw_get_module = dlsym(RTLD_DEFAULT, "hw_get_module");
	if (!hw_get_module) {
		AVOSLOG("hw_get_module not resolved: dlsym error: %s\n", dlerror());
		goto err;
	}

	if ((*hw_get_module)(GRALLOC_HARDWARE_MODULE_ID, &module) != 0)
		goto err;
	as->gralloc = (gralloc_module_t const *) module;
err:
	if (!as->gralloc)
		AVOSLOG("no gralloc: using public native window\n");
	return as;
}

void android_surface_destroy(android_surface_t **pas)
{
	android_surface_t *as = *pas;

#ifdef MEM_OPTIM_HACK
	mem_optim_hack_disable(as);
#endif
	free(as);
	*pas = NULL;
}

int android_surface_check_gralloc( void *surface_handle ) 
{
	android_surface_t *as;
	int gralloc = 0;
	
	as = android_surface_create(surface_handle, 0);
	if (!as) {
AVOSLOG("check_gralloc: android_surface_create failed");
		return gralloc;
	}
	
	if( as->gralloc ) {
AVOSLOG("check_gralloc: has gralloc!");
		gralloc = 1;
	}
	android_surface_destroy(&as);
	
	return gralloc;
}

int android_buffer_setup(android_surface_t *as, int w, int h, int buffer_type, int hal_format, int hw_usage, int *num_frames, int *min_undequeued)
{
	status_t err = NO_ERROR;
	int concrete_type;
	int usage = 0;
	int force_undequeued = *min_undequeued;
	
	if (as->gralloc && native_window_api_connect(as->anw, NATIVE_WINDOW_API_MEDIA) != 0) {
		AVOSLOG("native_window_api_connect FAIL");
		return -1;
	}
	as->buffer_type = buffer_type;
	as->hal_format = hal_format;

	if (buffer_type == BUFFER_TYPE_SW)
		usage |= GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;
	else
		usage |= hw_usage | GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_EXTERNAL_DISP;

	as->usage = usage;
#ifdef ARCHOS_ENHANCEMENT
	if (as->has_archos_enhancement) {
		if (buffer_type == BUFFER_TYPE_HW) {
#ifdef MEM_OPTIM_HACK
			if (w >= 1920 && *num_frames > 16) {
				AVOSLOG("mem_optim_hack_enable");
				mem_optim_hack_enable(as);
			}
#endif
		}
		usage |= GRALLOC_USAGE_FIX_VIDEO_ASPECT;
	}
#endif

	if (as->gralloc) {
		err = native_window_set_usage(as->anw, usage);
		CHECK_ERR();
		err = native_window_set_buffers_format(as->anw, hal_format);
		CHECK_ERR();
#if AVOS_ANDROID_API >= 18
		err = native_window_set_buffers_user_dimensions(as->anw, w, h);
#else
		err = native_window_set_buffers_dimensions(as->anw, w, h);
#endif
		CHECK_ERR();
		err = native_window_set_scaling_mode(as->anw, NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
		CHECK_ERR();
		as->anw->query(as->anw, NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, min_undequeued);
		CHECK_ERR();
		if( force_undequeued ) {
			*min_undequeued = force_undequeued;
		}
		*num_frames += *min_undequeued;
		err = native_window_set_buffer_count(as->anw, *num_frames);
		CHECK_ERR();

		as->anw->query(as->anw, NATIVE_WINDOW_CONCRETE_TYPE, &concrete_type);
		AVOSLOG("NATIVE_WINDOW_CONCRETE_TYPE: %d", concrete_type);
	} else {
		err = ANativeWindow_setBuffersGeometry(as->anw, w, h, hal_format);
		CHECK_ERR();
	}
	return 0;
}

int android_buffer_close(android_surface_t *as)
{
	if (as->gralloc)
		native_window_api_disconnect(as->anw, NATIVE_WINDOW_API_MEDIA);
	return 0;
}

int android_buffer_setcrop(android_surface_t *as, int ofs_x, int ofs_y, int w, int h)
{
	android_native_rect_t crop;

	if (as->gralloc) {
		crop.left = ofs_x;
		crop.top = ofs_y;
		crop.right = ofs_x + w;
		crop.bottom = ofs_y + h;
		return native_window_set_crop(as->anw, &crop);
	}
	return 0;
}

static int android_buffer_lock_data(android_surface_t *as, ANativeWindowBuffer_t *anb, void **pdata)
{
	if (as->gralloc && (as->buffer_type == BUFFER_TYPE_SW || as->has_archos_enhancement)) {
		void *data;

		status_t err = as->gralloc->lock(as->gralloc, anb->handle, as->usage, 0, 0, anb->width, anb->height, &data);
		CHECK_ERR();

		if (pdata)
			*pdata = data;
	}

	return 0;
}

static int android_buffer_unlock_data(android_surface_t *as, ANativeWindowBuffer_t *anb)
{
	if (as->gralloc && (as->buffer_type == BUFFER_TYPE_SW || as->has_archos_enhancement)) {
		status_t err = as->gralloc->unlock(as->gralloc, anb->handle);
		CHECK_ERR();
	}

	return 0;
}

int android_buffer_dequeue(android_surface_t *as, void **handle)
{
	status_t err = NO_ERROR;

	if (as->gralloc) {
		ANativeWindowBuffer_t *anb;
		#if AVOS_ANDROID_API >= 18
		err = as->anw->dequeueBuffer_DEPRECATED(as->anw, &anb);
		#else
		err = as->anw->dequeueBuffer(as->anw, &anb);
		#endif
		CHECK_ERR();
		#if AVOS_ANDROID_API >= 18
		err = as->anw->lockBuffer_DEPRECATED(as->anw, anb);
		#else
		err = as->anw->lockBuffer(as->anw, anb);
		#endif
		CHECK_ERR();
		*handle = anb;

		err = android_buffer_lock_data(as, anb, NULL);
		CHECK_ERR();
	} else {
		err = ANativeWindow_lock(as->anw, as->anbp, NULL);
		CHECK_ERR();
		*handle = as->anbp;
	}
	return 0;
}

int android_buffer_dequeue_with_buffer(android_surface_t *as, void **handle, android_buffer_t *buffer)
{
	status_t err = NO_ERROR;

	if (as->gralloc) {
		ANativeWindowBuffer_t *anb;
#if AVOS_ANDROID_API >= 18
		err = as->anw->dequeueBuffer_DEPRECATED(as->anw, &anb);
#else
		err = as->anw->dequeueBuffer(as->anw, &anb);
#endif
		CHECK_ERR();
#if AVOS_ANDROID_API >= 18
		err = as->anw->lockBuffer_DEPRECATED(as->anw, anb);
#else
		err = as->anw->lockBuffer(as->anw, anb);
#endif
		CHECK_ERR();
		*handle = anb;

		err = android_buffer_lock_data(as, anb, NULL);
		CHECK_ERR();
	} else {
		err = ANativeWindow_lock(as->anw, as->anbp, NULL);
		CHECK_ERR();
		*handle = as->anbp;

		buffer->type = as->buffer_type;
		buffer->hal_format = as->hal_format;
		buffer->handle = as->anbp;
		buffer->img_handle = as->anbp; //todo check
		buffer->data = (uint8_t *) as->anbp->bits;
		buffer->width = as->anbp->width;
		buffer->height = as->anbp->height;
		buffer->stride = as->anbp->stride;
		buffer->size = buffer->height * buffer->stride * 3 / 2;

	}
	return 0;
}

int android_buffer_queue(android_surface_t *as, void *handle)
{
	status_t err = NO_ERROR;
	ANativeWindowBuffer_t *anb = (ANativeWindowBuffer_t *)handle;

	if (as->gralloc) {
		err = android_buffer_unlock_data(as, anb);
		CHECK_ERR();
		#if AVOS_ANDROID_API >= 18
		err = as->anw->queueBuffer_DEPRECATED(as->anw, anb);
		#else
		err = as->anw->queueBuffer(as->anw, anb);
		#endif
		CHECK_ERR();
	} else {
		err = ANativeWindow_unlockAndPost(as->anw);
		CHECK_ERR();
	}
	return 0;
}

int android_buffer_alloc(android_surface_t *as, android_buffer_t *buffer)
{
	status_t err = NO_ERROR;

	buffer->type = as->buffer_type;
	buffer->hal_format = as->hal_format;

	if (as->gralloc) {
		ANativeWindowBuffer_t *anb = NULL;
		void *data = NULL;
		#if AVOS_ANDROID_API >= 18
		err = as->anw->dequeueBuffer_DEPRECATED(as->anw, &anb);
		#else
		err = as->anw->dequeueBuffer(as->anw, &anb);
		#endif
		CHECK_ERR();
		#if AVOS_ANDROID_API >= 18
		err = as->anw->lockBuffer_DEPRECATED(as->anw, anb);
		#else
		err = as->anw->lockBuffer(as->anw, anb);
		#endif
		CHECK_ERR();
		buffer->handle = anb;
		buffer->img_handle = anb->handle;
	
		err = android_buffer_lock_data(as, anb, &data);
		CHECK_ERR();
		buffer->data = (uint8_t *) data;
		buffer->width = anb->width;
		buffer->height = anb->height;
		buffer->stride = anb->stride;
	} else {
		if (!as->anbp)
			as->anbp = calloc(1, sizeof(ANativeWindow_Buffer));
		err = ANativeWindow_lock(as->anw, as->anbp, NULL);
		CHECK_ERR();
		buffer->handle = as->anbp;
		buffer->img_handle = as->anbp; //todo check
		buffer->data = (uint8_t *) as->anbp->bits;
		buffer->width = as->anbp->width;
		buffer->height = as->anbp->height;
		buffer->stride = as->anbp->stride;
	}

	buffer->size = buffer->height * buffer->stride * 3 / 2;
	return 0;
}

int android_buffer_cancel(android_surface_t *as, void *handle)
{
	status_t err = NO_ERROR;
	ANativeWindowBuffer_t *anb = (ANativeWindowBuffer_t *)handle;

	if (as->gralloc) {
		err = android_buffer_unlock_data(as, anb);
		CHECK_ERR();

		#if AVOS_ANDROID_API >= 18
		err = as->anw->cancelBuffer_DEPRECATED(as->anw, anb);
		#else
		err = as->anw->cancelBuffer(as->anw, anb);
		#endif
		CHECK_ERR();
	} else {
		AVOSLOG("using public native window\n");
		err = ANativeWindow_unlockAndPost(as->anw);
		CHECK_ERR();
		free(as->anbp);
		as->anbp = NULL;
	}

	return 0;
}

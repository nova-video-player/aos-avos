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

#define GRALLOC_USAGE_FIX_VIDEO_ASPECT 0x04000000

#define AVOSLOG(fmt, ...) do { printf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); fflush(stdout); } while (0)

#define CHECK_ERR() do { if (err != NO_ERROR) { AVOSLOG("error in %s  line %d\n", __FUNCTION__, __LINE__ ); return -1; } } while (0)

struct android_surface {
	ANativeWindow *anw;
	ANativeWindow_Buffer *anbp;
	int buffer_type;
	int hal_format;
	int usage;
	gralloc_module_t const* gralloc;
};

android_surface_t *android_surface_create(void *surface_handle)
{
	android_surface_t *as = NULL;
	ANativeWindow *anw = (ANativeWindow *)surface_handle;
	AVOSLOG("surface_handle: %p, anw: %p", surface_handle, anw);

	as = (android_surface_t *) calloc(1, sizeof(android_surface_t));
	if (!as)
		goto err;

	as->anw = anw;
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
	
	as = android_surface_create(surface_handle);
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

	if (as->gralloc) {
		err = native_window_set_usage(as->anw, usage);
		CHECK_ERR();
		err = native_window_set_buffers_format(as->anw, hal_format);
		CHECK_ERR();
		err = native_window_set_buffers_user_dimensions(as->anw, w, h);
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
	if (as->gralloc && (as->buffer_type == BUFFER_TYPE_SW)) {
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
	if (as->gralloc && (as->buffer_type == BUFFER_TYPE_SW)) {
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
		err = as->anw->dequeueBuffer_DEPRECATED(as->anw, &anb);
		CHECK_ERR();
		err = as->anw->lockBuffer_DEPRECATED(as->anw, anb);
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
		err = as->anw->dequeueBuffer_DEPRECATED(as->anw, &anb);
		CHECK_ERR();
		err = as->anw->lockBuffer_DEPRECATED(as->anw, anb);
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
		err = as->anw->queueBuffer_DEPRECATED(as->anw, anb);
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
		err = as->anw->dequeueBuffer_DEPRECATED(as->anw, &anb);
		CHECK_ERR();
		err = as->anw->lockBuffer_DEPRECATED(as->anw, anb);
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

		err = as->anw->cancelBuffer_DEPRECATED(as->anw, anb);
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

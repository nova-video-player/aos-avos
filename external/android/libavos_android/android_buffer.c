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
#include <limits.h>
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
	int public_buffer_locked;
	int buffer_type;
	int hal_format;
	int usage;
	gralloc_module_t const* gralloc;
};

/* Stride is in pixels for packed RGB. YV12 has separately aligned chroma
 * rows. Vendor gralloc layouts retain their historical 4:2:0 size estimate;
 * do not apply that estimate to an unknown public-window format. */
static int android_buffer_size(int width, int height, int stride, int format,
		int vendor_layout, size_t *size)
{
	*size = 0;
	if (width <= 0 || height <= 0 || stride < width)
		return -1;

	uint64_t pixels = (uint64_t)stride * height;
	uint64_t bytes;
	switch (format) {
	case HAL_PIXEL_FORMAT_RGBA_8888:
	case HAL_PIXEL_FORMAT_RGBX_8888:
	case HAL_PIXEL_FORMAT_BGRA_8888:
		bytes = pixels * 4;
		break;
	case HAL_PIXEL_FORMAT_RGB_888:
		bytes = pixels * 3;
		break;
	case HAL_PIXEL_FORMAT_RGB_565:
		bytes = pixels * 2;
		break;
	case HAL_PIXEL_FORMAT_YV12:
		if ((width & 1) || (height & 1) || (stride & 15))
			return -1;
		bytes = pixels + (((uint64_t)stride / 2 + 15) & ~UINT64_C(15)) * height;
		break;
	case HAL_PIXEL_FORMAT_YCrCb_420_SP:
		if ((width & 1) || (height & 1) || (stride & 1))
			return -1;
		bytes = pixels + pixels / 2;
		break;
	default:
		if (!vendor_layout)
			return -1;
		bytes = pixels * 3 / 2;
		break;
	}
	/* VIDEO_FRAME stores capacity in signed int fields, also on 64-bit. */
	if (bytes > INT_MAX)
		return -1;
	*size = (size_t)bytes;
	return 0;
}

/* Public windows allow one CPU-locked buffer. Keep its descriptor stable for
 * the lifetime of the surface: the sink retains this address as its handle. */
static int android_public_buffer_lock(android_surface_t *as)
{
	if (as->public_buffer_locked)
		return -1;
	if (!as->anbp) {
		as->anbp = calloc(1, sizeof(*as->anbp));
		if (!as->anbp)
			return -1;
	}
	int err = ANativeWindow_lock(as->anw, as->anbp, NULL);
	if (!err)
		as->public_buffer_locked = 1;
	return err;
}

static int android_public_buffer_release(android_surface_t *as, void *handle)
{
	if (!as->public_buffer_locked || handle != as->anbp)
		return -1;
	/* There is no public cancel API. Even cancellation must post the buffer.
	 * unlockAndPost consumes the lock, including when queueing fails; do not
	 * try to unlock it a second time from the caller's error cleanup. */
	as->public_buffer_locked = 0;
	return ANativeWindow_unlockAndPost(as->anw);
}

android_surface_t *android_surface_create(void *surface_handle)
{
	android_surface_t *as = NULL;
	ANativeWindow *anw = (ANativeWindow *)surface_handle;
	if (!anw || anw->common.magic != ANDROID_NATIVE_WINDOW_MAGIC)
		return NULL;
	AVOSLOG("surface_handle: %p, anw: %p", surface_handle, anw);

	as = (android_surface_t *) calloc(1, sizeof(android_surface_t));
	if (!as)
		return NULL;

	as->anw = anw;

	hw_module_t const* module = NULL;
	int (*hw_get_module) (const char *id, const struct hw_module_t **module) = NULL;
	dlerror();
	hw_get_module = dlsym(RTLD_DEFAULT, "hw_get_module");
	if (!hw_get_module) {
		AVOSLOG("hw_get_module not resolved: dlsym error: %s\n", dlerror());
		goto err;
	}

	if ((*hw_get_module)(GRALLOC_HARDWARE_MODULE_ID, &module) != 0)
		goto err;
	/* Inspect only the common header until the module is known to be gralloc0.
	 * Gralloc1 and later interfaces do not have gralloc0's callback layout. */
	if (!module || module->tag != HARDWARE_MODULE_TAG || !module->id ||
	    strcmp(module->id, GRALLOC_HARDWARE_MODULE_ID)) {
		AVOSLOG("invalid gralloc module header");
		goto err;
	}
	if ((module->module_api_version >> 8) != 0) {
		AVOSLOG("unsupported gralloc module API version 0x%04x",
			(unsigned int)module->module_api_version);
		goto err;
	}
	/* Keep all legacy 0.x minor versions; only the synchronous lock/unlock
	 * callbacks are used here, not the optional YCbCr or fence extensions. */
	gralloc_module_t const* gralloc = (gralloc_module_t const *) module;
	if (!gralloc->lock || !gralloc->unlock) {
		AVOSLOG("gralloc0 module missing lock/unlock callbacks");
		goto err;
	}
	as->gralloc = gralloc;
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
	if (!as->gralloc && as->public_buffer_locked)
		android_public_buffer_release(as, as->anbp);
	free(as->anbp);
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
		int queried_min_undequeued = 0;
		err = as->anw->query(as->anw, NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS,
			&queried_min_undequeued);
		CHECK_ERR();
		*min_undequeued = force_undequeued ? force_undequeued : queried_min_undequeued;
		*num_frames += *min_undequeued;
		err = native_window_set_buffer_count(as->anw, *num_frames);
		CHECK_ERR();

		/* This query is diagnostic only; failure must not reject a usable window. */
		if (!as->anw->query(as->anw, NATIVE_WINDOW_CONCRETE_TYPE, &concrete_type))
			AVOSLOG("NATIVE_WINDOW_CONCRETE_TYPE: %d", concrete_type);
		else
			AVOSLOG("NATIVE_WINDOW_CONCRETE_TYPE unavailable");
	} else {
		err = ANativeWindow_setBuffersGeometry(as->anw, w, h, hal_format);
		CHECK_ERR();
	}
	return 0;
}

int android_buffer_close(android_surface_t *as)
{
	if (!as || !as->anw)
		return -1;
	if (as->gralloc)
		native_window_api_disconnect(as->anw, NATIVE_WINDOW_API_MEDIA);
	else if (as->public_buffer_locked)
		return android_public_buffer_release(as, as->anbp);
	return 0;
}

int android_buffer_setcrop(android_surface_t *as, int ofs_x, int ofs_y, int w, int h)
{
	android_native_rect_t crop;

	if (!as || !as->anw)
		return -1;

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

	if (!as || !as->anw) {
		AVOSLOG("error: surface or window is NULL");
		return -1;
	}

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
		err = android_public_buffer_lock(as);
		CHECK_ERR();
		*handle = as->anbp;
	}
	return 0;
}

int android_buffer_dequeue_with_buffer(android_surface_t *as, void **handle, android_buffer_t *buffer)
{
	status_t err = NO_ERROR;

	if (!as || !as->anw) {
		AVOSLOG("error: surface or window is NULL");
		return -1;
	}

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
		err = android_public_buffer_lock(as);
		CHECK_ERR();
		*handle = as->anbp;

		buffer->type = as->buffer_type;
		buffer->hal_format = as->anbp->format;
		buffer->handle = as->anbp;
		buffer->img_handle = as->anbp; //todo check
		buffer->data = (uint8_t *) as->anbp->bits;
		buffer->width = as->anbp->width;
		buffer->height = as->anbp->height;
		buffer->stride = as->anbp->stride;
		if (android_buffer_size(as->anbp->width, as->anbp->height,
		    as->anbp->stride, as->anbp->format, 0, &buffer->size)) {
			android_public_buffer_release(as, as->anbp);
			memset(buffer, 0, sizeof(*buffer));
			*handle = NULL;
			return -1;
		}

	}
	return 0;
}

int android_buffer_queue(android_surface_t *as, void *handle)
{
	status_t err = NO_ERROR;
	ANativeWindowBuffer_t *anb = (ANativeWindowBuffer_t *)handle;

	if (!as || !as->anw) {
		AVOSLOG("error: surface or window is NULL");
		return -1;
	}

	if (as->gralloc) {
		err = android_buffer_unlock_data(as, anb);
		CHECK_ERR();
		err = as->anw->queueBuffer_DEPRECATED(as->anw, anb);
		CHECK_ERR();
	} else {
		err = android_public_buffer_release(as, handle);
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
		buffer->hal_format = anb->format;
		buffer->width = anb->width;
		buffer->height = anb->height;
		buffer->stride = anb->stride;
	} else {
		err = android_public_buffer_lock(as);
		CHECK_ERR();
		buffer->hal_format = as->anbp->format;
		buffer->handle = as->anbp;
		buffer->img_handle = as->anbp; //todo check
		buffer->data = (uint8_t *) as->anbp->bits;
		buffer->width = as->anbp->width;
		buffer->height = as->anbp->height;
		buffer->stride = as->anbp->stride;
	}

	if (android_buffer_size(buffer->width, buffer->height, buffer->stride,
	    buffer->hal_format, as->gralloc != NULL, &buffer->size)) {
		android_buffer_cancel(as, buffer->handle);
		memset(buffer, 0, sizeof(*buffer));
		return -1;
	}
	return 0;
}

int android_buffer_cancel(android_surface_t *as, void *handle)
{
	status_t err = NO_ERROR;
	ANativeWindowBuffer_t *anb = (ANativeWindowBuffer_t *)handle;

	if (!as) {
		AVOSLOG("error: as is NULL");
		return -1;
	}

	if (!as->anw) {
		AVOSLOG("error: as->anw is NULL");
		return -1;
	}

	if (as->gralloc) {
		err = android_buffer_unlock_data(as, anb);
		CHECK_ERR();

		err = as->anw->cancelBuffer_DEPRECATED(as->anw, anb);
		CHECK_ERR();
	} else {
		AVOSLOG("using public native window\n");
		err = android_public_buffer_release(as, handle);
		CHECK_ERR();
	}

	return 0;
}

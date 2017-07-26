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

#ifndef ANDROID_BUFFER_H
#define ANDROID_BUFFER_H

#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

enum {
    BUFFER_TYPE_SW,
    BUFFER_TYPE_HW,
};


typedef struct android_surface android_surface_t;

typedef struct android_buffer {
    void *handle;
    const void *img_handle;
    int type;
    int hal_format;

    uint8_t *data;
    size_t  width;
    size_t  height;
    size_t  stride;
    size_t size;
} android_buffer_t;

android_surface_t *android_surface_create(void *surface_handle, int has_archos_enhancement);
void android_surface_destroy(android_surface_t **as);
int  android_surface_check_gralloc(void *surface_handle);

int android_buffer_setup(android_surface_t *as, int w, int h, int buffer_type, int hal_format, int special_usage, int *num_frames, int *min_undequeued);
int android_buffer_close(android_surface_t *as);
int android_buffer_setcrop(android_surface_t *as, int ofs_x, int ofs_y, int w, int h);

int android_buffer_dequeue(android_surface_t *as, void **handle);
int android_buffer_dequeue_with_buffer(android_surface_t *as, void **handle, android_buffer_t *buffer);
int android_buffer_queue(android_surface_t *as, void *handle);
int android_buffer_alloc(android_surface_t *as, android_buffer_t *buffer);
int android_buffer_cancel(android_surface_t *as, void *handle);

#if __cplusplus
}
#endif

#endif

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

#ifndef AVOS_MR_H
#define AVOS_MR_H

#include <stdint.h>
#include "avos_common.h"

#if __cplusplus
extern "C" {
#endif

typedef struct avos_mr avos_mr_t;

typedef struct avos_mr_handle {
	avos_mr_t *(*create)	();
	int (*destroy)		(avos_mr_t *mr);

	int (*setdatasource)	(avos_mr_t *mr, const char *path, const char **keys, const char **values);
	int (*setdatasource_fd)	(avos_mr_t *mr, int fd, int64_t offset, int64_t length);
	int (*getmetadata)	(avos_mr_t *mr, metadata_buffer_t **buffer);
	const char *(*extractmetadata) (avos_mr_t *mr, uint32_t id);
	int (*getframe)		(avos_mr_t *mr, int time_ms, avos_bgra_bitmap_t **pframe);
	int (*getapic)		(avos_mr_t *mr, avos_apic_t **papic);
} avos_mr_handle_t;

const avos_mr_handle_t *avos_mr_get_handle();

#if __cplusplus
}
#endif

#endif // AVOS_MR_H

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

#ifndef _DMALLOC_H
#define _DMALLOC_H

#ifdef __cplusplus
extern "C" {
#endif

extern int   DMALLOC_debug(int level);
extern int   DMALLOC_init(void);
extern void* DMALLOC_alloc(size_t size);
extern int   DMALLOC_free(void* addr);
extern int   DMALLOC_exit(void);

extern void* DMALLOC_alloc_cached(size_t size);
extern int   DMALLOC_free_cached(void* addr);
extern int   DMALLOC_wb_cached(void* addr, size_t size);
extern int   DMALLOC_inv_cached(void* addr, size_t size);
extern int   DMALLOC_wb_inv_cached(void* addr, size_t size);

#ifdef __cplusplus
}
#endif

#endif

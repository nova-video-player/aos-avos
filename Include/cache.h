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

#ifndef CACHE_H
#define CACHE_H

// A very simple cache. Only suitable for small amounts of data.
#include "types.h"

struct Cache_str;
typedef struct Cache_str Cache;


Cache* Cache_create(int size);
void   Cache_destroy(Cache *obj);
void*  Cache_addEntry(Cache *obj, int id, void *data);	// returns the displaced object or NULL
void*  Cache_getEntry(Cache *obj, int id);		// enumerate all entries - returns null on unused indices
void*  Cache_getEntryAt(Cache *obj, int index);		// returns NULL on a miss
int    Cache_getIdAt(Cache *obj, int index);
void   Cache_invalidateEntry(Cache *obj, int id);
void   Cache_invalidateEntryAt(Cache *obj, int index);
int    Cache_getSize(const Cache *obj);
BOOL   Cache_isFull(const Cache *obj);

#endif // CACHE_H

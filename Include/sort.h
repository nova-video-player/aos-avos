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

#ifndef SORT_H
#define SORT_H

void quickersort(int  (*comp) (int, int), void (*swap) (int, int), int lower, int upper);
void quicksort  (int  (*comp) (int, int), void (*swap) (int, int), int lower, int upper);

typedef int (*i_fp_i_i_vp) (int, int, void*);
typedef void (*v_fp_i_i_vp) (int, int, void*);

typedef struct {
	i_fp_i_i_vp qs_comp;
	v_fp_i_i_vp qs_swap;
	void *data;
} sort_context_t;

void quicksort_r(sort_context_t *cntx, int lower, int upper);

#endif

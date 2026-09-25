/*
 * Copyright 2026 Courville Software
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

#ifndef STREAM_BUFFER_LIMITS_H
#define STREAM_BUFFER_LIMITS_H

#include <limits.h>
#include <stdint.h>

#define STREAM_MIB (1024 * 1024)

// Account for the legacy ring overlap or the CBE's second copy before narrowing.
static inline int stream_buffer_mib_valid(int mib, int copies, int overlap)
{
	int64_t bytes = (int64_t)mib * STREAM_MIB;
	return mib >= 0 && copies > 0 && overlap >= 0 &&
		bytes <= (INT_MAX - (int64_t)overlap) / copies;
}

static inline int stream_buffer_mib_bytes(int mib)
{
	return stream_buffer_mib_valid(mib, 1, 0) ? (int)((int64_t)mib * STREAM_MIB) : -1;
}

#endif

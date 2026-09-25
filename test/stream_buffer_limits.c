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

#include "stream_buffer_limits.h"
#include <assert.h>

int main(void)
{
	assert(stream_buffer_mib_bytes(24) == 24 * STREAM_MIB);
	assert(stream_buffer_mib_bytes(0) == 0);
	assert(stream_buffer_mib_bytes(-1) == -1);
	assert(stream_buffer_mib_bytes(INT_MAX) == -1);
	assert(stream_buffer_mib_valid(2041, 1, 6 * STREAM_MIB));
	assert(!stream_buffer_mib_valid(2042, 1, 6 * STREAM_MIB));
	assert(stream_buffer_mib_valid(1023, 2, 0));
	assert(!stream_buffer_mib_valid(1024, 2, 0));
	assert(!stream_buffer_mib_valid(INT_MAX, 2, 0));
	return 0;
}

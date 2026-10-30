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

#include <system/window.h>

int android_window_set_buffers_rotation(void *anw, int rotation)
{
	uint32_t transform;

	if (!anw || ((ANativeWindow *)anw)->common.magic != ANDROID_NATIVE_WINDOW_MAGIC)
		return -1;

	switch (rotation) {
		case 0: transform = 0; break;
		case 90: transform = HAL_TRANSFORM_ROT_90; break;
		case 180: transform = HAL_TRANSFORM_ROT_180; break;
		case 270: transform = HAL_TRANSFORM_ROT_270; break;
		default: return 0; /* Preserve the no-op for unsupported angles. */
	}

	/* Zero must clear any transform left on a reused surface. */
	return native_window_set_buffers_transform((ANativeWindow*)anw, transform);
}

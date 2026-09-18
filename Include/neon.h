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

#ifndef _NEON_H_
#define _NEON_H_

#include "color.h"
#include <stdint.h>

void neon_memcpy(uint8_t *dst, uint8_t *src, int size);
void neon_memset(uint8_t *dst, uint8_t val, int size);

void neon_memset16(uint16_t *dst, uint16_t value, int count);
void neon_memset32(uint32_t *dst, uint32_t value, int count);


#endif	// _NEON_H_

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

#ifndef _DOWNMIX_H
#define _DOWNMIX_H

#include <stdint.h>

enum {
	CH_UNMAPPED = 0,
	CH_FL,
	CH_FR,
	CH_CTR,
	CH_SUB,
	CH_BL,
	CH_BR,
	CH_SL,
	CH_SR
};

void downmix             ( uint16_t *pcm, uint8_t *src,   int samples, int channels, int bits, int *map );
void downmix_planar      ( uint16_t *pcm, uint8_t *src[], int samples, int channels, int bits, int *map );
void downmix_float       ( uint16_t *pcm, uint8_t *src,   int samples, int channels, int bits, int *map );
void downmix_float_planar( uint16_t *pcm, uint8_t *src[], int samples, int channels, int bits, int *map );

#endif

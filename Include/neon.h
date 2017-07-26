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

/*
	neon_nv12_to_BGRA32
	neon_nv12_to_RGBX32
	neon_yuv420_to_BGRA32
	neon_yuv420_to_RGBX32
	neon_yuv422_to_BGRA32
	neon_yuv422_to_RGBX32
	neon_yuv420_to_YUYV
	neon_yuv420_to_UYVY
	
	yuv	16 pixel aligned YUV 422 16 bit pointer
	y	16 pixel aligned Y 8 bit pointer
	u	16 pixel aligned U 8 bit pointer
	v	16 pixel aligned V 8 bit pointer
	
	w	width in pixel (multiple of 16!)
	h	height in pixel
	yw	Y linestep in bytes
	cw	U/V linestep in bytes
	dw	dest. linesstep in bytes
*/
void neon_nv12_to_BGRA32(uint32_t *bgra, uint8_t *y1, uint8_t *y2,uint8_t *uv, int remaining_width, int linesize);
void neon_nv12_to_RGBX32(uint32_t *bgra, uint8_t *y1, uint8_t *y2,uint8_t *uv, int remaining_width, int linesize);
void neon_yuv422_to_BGRA32(uint32_t *bgra, uint8_t *y1, uint8_t *u, uint8_t *v, int remaining_width);
void neon_yuv422_to_RGBX32(uint32_t *bgra, uint8_t *y1, uint8_t *u, uint8_t *v, int remaining_width);
void neon_yuv420_to_BGRA32(uint32_t *bgra, uint8_t *y1, uint8_t *y2,uint8_t *u, uint8_t *v, int remaining_width, int linesize);
void neon_yuv420_to_RGBX32(uint32_t *bgra, uint8_t *y1, uint8_t *y2,uint8_t *u, uint8_t *v, int remaining_width, int linesize);
void neon_yuv42010b_to_BGRA32(uint32_t *bgra, uint16_t *y1, uint16_t *y2,uint16_t *u, uint16_t *v, int remaining_width, int linesize);
void neon_yuv42010b_to_RGBX32(uint32_t *bgra, uint16_t *y1, uint16_t *y2,uint16_t *u, uint16_t *v, int remaining_width, int linesize);
void neon_yuv420_to_YUYV(uint16_t *yuv, uint8_t *y, uint8_t *u, uint8_t *v, int w, int h, int yw, int cw, int dw);
void neon_yuv420_to_UYVY(uint16_t *yuv, uint8_t *y, uint8_t *u, uint8_t *v, int w, int h, int yw, int cw, int dw);

void neon_yuv422_to_UYVY(uint16_t *yuv, uint8_t *y, uint8_t *u, uint8_t *v, int w, int h, int yw, int cw, int dw);

void neon_memcpy_shift(uint8_t *dst, const uint16_t *src, const int length);
void neon_scale_420P10b_YUV(uint8_t *dst, const uint16_t *src, const int length);
void neon_copy_pack_shift(uint16_t *dst, const uint16_t *srcV, const uint16_t *srcU, const int length);

/*
	neon_rgba8888_to_rgb565
	
	src	16 pixel aligned RGBA 32 bit pointer
	dst	16 pixel aligned RGB565 16 bit pointer
	w	width in pixel (multiple of 16!)
	h	height in pixel
	sw	source linestep in bytes
	dw	dest. linesstep in bytes

	alpha will be ignored!
*/
void neon_rgba8888_to_rgb565(uint32_t *src, uint16_t *dst, int w, int h, int sw, int dw);

/*
	neon_rgb565_to_rgba8888
	
	src	16 pixel aligned RGB565 16 bit pointer
	dst	16 pixel aligned RGBA 32 bit pointer
	w	width in pixel (multiple of 16!)
	h	height in pixel
	sw	source linestep in bytes
	dw	dest. linesstep in bytes
	alpha 	global alpha for all dest. pixels
*/
void neon_rgb565_to_rgba8888(uint16_t *src, uint32_t *dst, int w, int h, int sw, int dw, int alpha);

void neon_memcpy(uint8_t *dst, uint8_t *src, int size);
void neon_memset(uint8_t *dst, uint8_t val, int size);

void neon_memset16(uint16_t *dst, uint16_t value, int count);
void neon_memset32(uint32_t *dst, uint32_t value, int count);

void neon_copy_aligned32(rgba *src, rgba *dst, int length, int alpha);
void neon_copy_aligned32_rotated_CW270(rgba *src, rgba *dst, int h, int w, int sw, int dw, int alpha);
void neon_copy_yuv_rotated_4(int *src0,int *src1,int *src2,int *src3, int *dst0, int *dst1, int darker);

#endif	// _NEON_H_

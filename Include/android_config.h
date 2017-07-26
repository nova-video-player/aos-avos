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

#ifndef ANDROID_CONFIG_H
#define ANDROID_CONFIG_H

#include "android_buffer.h"

// from system/graphics.h
enum {
	HAL_PIXEL_FORMAT_RGBA_8888          = 1,
	HAL_PIXEL_FORMAT_RGBX_8888          = 2,
	HAL_PIXEL_FORMAT_RGB_888            = 3,
	HAL_PIXEL_FORMAT_RGB_565            = 4,
	HAL_PIXEL_FORMAT_BGRA_8888          = 5,
	HAL_PIXEL_FORMAT_RGBA_5551          = 6,
	HAL_PIXEL_FORMAT_RGBA_4444          = 7,
	HAL_PIXEL_FORMAT_YV12   = 0x32315659, // YCrCb 4:2:0 Planar
};

/*
 * Hardware specific hal pixel formats
 */

#define HAL_PIXEL_FORMAT_NV12_SGX 0x100 // XXX Not used

/* qcom */
#define HAL_PIXEL_FORMAT_YCrCb_420_SP_ADRENO 0x7FA30C01
#define HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED 0x7FA30C03 // QOMX_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka
#define HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED_ICS 0x108 // QOMX_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka
#define HAL_PIXEL_FORMAT_YCbCr_420_SP 0x109
#define HAL_OMX_QCOM_COLOR_FormatYVU420SemiPlanar 0x7FA30C00 // XXX Not used

/* rk29 */
#define HAL_PIXEL_FORMAT_NV12_RK 0x20 // OMX_RK_COLOR_FormatYUV420SemiPlanar

/* Exynos 4 */
#define HAL_PIXEL_FORMAT_YV12_ZERO_COPY_EX 0x101
#define HAL_PIXEL_FORMAT_NV12_ZERO_COPY_EX 0x105

/* Exynos 5 */
#define HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED_EX 0x107 // Exynos_OMX_Color_FormatYUV420_SP_Tiler

/* samsung */
#define HAL_PIXEL_FORMAT_NV12_SAMSUNG  0x100

/* omap4 */
#define HAL_PIXEL_FORMAT_NV12_TI 0x100 // OMX_TI_COLOR_FormatYUV420PackedSemiPlanar

/* rk30 */
#define HAL_OMX_RK_COLOR_FormatBGRA 0x1

/* tegra2 */
#define HAL_OMX_NV_COLOR_FormatYUV420Planar 0x32315659

/* ST Ericson */
#define OMX_STE_COLOR_FormatYUV420PackedSemiPlanarMB 0x7FA00000
#define HAL_PIXEL_FORMAT_YCBCR42XMBN 0xE

/*
 * get android hal format according to avos color and buffer type
 * return 0 if hal_format is not found
 */
int android_get_hal_format(int av_color, int buffer_type);
int android_get_hal_format_unknown(int av_color, int buffer_type);

int android_get_av_color(int buffer_type);
int android_get_av_color_unknown(int buffer_type);

int android_can_hw_run_dec(int cpu_type);

#endif

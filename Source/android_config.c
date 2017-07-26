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

#include "android_config.h"
#include "av.h"
#include "device_config.h"
#include "stream_config.h"
#include "debug.h"

int no_hw_buf = 0;

#ifdef DEBUG_MSG
DECLARE_DEBUG_TOGGLE("nhb", no_hw_buf );
#endif

typedef struct av_hal_format {
	int av_color;
	int hal_format;
	int buffer_type;
	DEVICE_ANDROID_VERSION min_version;
} av_hal_format_t;

typedef struct hw_config {
	DEVICE_HW_TYPE hw_type;
	CPU_TYPE cpu_type;
	av_hal_format_t omx_color_table[];
} hw_config_t;

#define SFDEC_DEFAULT SFDEC_MEDIACODEC

#define DEFAULT_COLOR \
{ \
	{ AV_IMAGE_RGBX_32, HAL_PIXEL_FORMAT_RGBX_8888, BUFFER_TYPE_SW, ANDROID_VERSION_UNKNOWN }, \
	{ -1, 0, -1, ANDROID_VERSION_UNKNOWN }, \
}

#define DEFAULT_YUV_COLOR \
{ \
	{ AV_IMAGE_YV12, HAL_PIXEL_FORMAT_YV12, BUFFER_TYPE_SW, ANDROID_VERSION_UNKNOWN }, \
	{ -1, 0, -1, ANDROID_VERSION_UNKNOWN }, \
}

static hw_config_t hw_config_unknown = {
	HW_TYPE_UNKNOWN,
	SFDEC_DEFAULT,
	DEFAULT_COLOR
};

static hw_config_t hw_config_default_kk = {
	HW_TYPE_DEFAULT_KK,
	SFDEC_DEFAULT,
	DEFAULT_COLOR
};

static hw_config_t hw_config_omap4 = {
	HW_TYPE_OMAP4,
	SFDEC_DEFAULT,
	{
		{ AV_IMAGE_BGRA_32, HAL_PIXEL_FORMAT_BGRA_8888, BUFFER_TYPE_SW, ANDROID_VERSION_UNKNOWN },
		{ AV_IMAGE_NV12,    HAL_PIXEL_FORMAT_NV12_TI,   BUFFER_TYPE_HW, ANDROID_VERSION_UNKNOWN },
		{ -1, 0, -1, ANDROID_VERSION_UNKNOWN },
	}
};

static hw_config_t hw_config_archos_omap4 = {
	HW_TYPE_ARCHOS_OMAP4,
	OMXHW,
	{
		{ AV_IMAGE_BGRA_32, HAL_PIXEL_FORMAT_BGRA_8888, BUFFER_TYPE_SW, ANDROID_VERSION_UNKNOWN },
		{ AV_IMAGE_NV12,    HAL_PIXEL_FORMAT_NV12_TI,   BUFFER_TYPE_HW, ANDROID_VERSION_UNKNOWN },
		{ -1, 0, -1, ANDROID_VERSION_UNKNOWN },
	}
};

static hw_config_t hw_config_rk29 = {
	HW_TYPE_RK29,
	SFDEC_OMXCODEC,
	{
		{ AV_IMAGE_NV12, HAL_PIXEL_FORMAT_NV12_RK, BUFFER_TYPE_SW, ANDROID_VERSION_UNKNOWN },
		{ AV_IMAGE_NV12, HAL_PIXEL_FORMAT_NV12_RK, BUFFER_TYPE_HW, ANDROID_VERSION_UNKNOWN },
		{ -1, 0, -1, ANDROID_VERSION_UNKNOWN },
	}
};

static hw_config_t hw_config_rk30 = {
	HW_TYPE_RK30,
	SFDEC_OMXCODEC,
	{
		{ AV_IMAGE_YV12, HAL_PIXEL_FORMAT_YV12, BUFFER_TYPE_SW, ANDROID_VERSION_UNKNOWN },
		// rk wants RGB565 for output (the conv is done in surfaceflinger
		{ AV_IMAGE_BGRA_32, HAL_PIXEL_FORMAT_RGBA_8888, BUFFER_TYPE_HW, ANDROID_VERSION_UNKNOWN },
		{ -1, 0, -1, ANDROID_VERSION_UNKNOWN },
	}
};

static hw_config_t hw_config_tegra3 = {
	HW_TYPE_TEGRA3,
	SFDEC_DEFAULT,
	{
		{ AV_IMAGE_YV12, HAL_PIXEL_FORMAT_YV12, BUFFER_TYPE_SW, ANDROID_VERSION_UNKNOWN },
		{ AV_IMAGE_YV12, HAL_PIXEL_FORMAT_YV12, BUFFER_TYPE_HW, ANDROID_VERSION_UNKNOWN },
		{ -1, 0, -1, ANDROID_VERSION_UNKNOWN },
	}
};

static hw_config_t hw_config_tegra2 = {
	HW_TYPE_TEGRA2,
	OMXHW,
	{
		{ AV_IMAGE_YV12, HAL_PIXEL_FORMAT_YV12, BUFFER_TYPE_SW, ANDROID_VERSION_UNKNOWN },
		{ AV_IMAGE_YV12, HAL_PIXEL_FORMAT_YV12, BUFFER_TYPE_HW, ANDROID_VERSION_UNKNOWN },
		{ -1, 0, -1, ANDROID_VERSION_UNKNOWN },
	}
};

static hw_config_t hw_config_qcom_s1 = {
	HW_TYPE_QCOM_S1,
	LIBAV,
	DEFAULT_YUV_COLOR
};

static hw_config_t hw_config_qcom_s2 = {
	HW_TYPE_QCOM_S2,
	OMXHW,
	{
		{ AV_IMAGE_QCOM_NV12_TILED, HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED, BUFFER_TYPE_HW, ANDROID_VERSION_UNKNOWN },
		{ AV_IMAGE_NV12, HAL_PIXEL_FORMAT_YCbCr_420_SP , BUFFER_TYPE_HW, ANDROID_VERSION_UNKNOWN },
		{ -1, 0, -1, ANDROID_VERSION_UNKNOWN },
	}
};

static hw_config_t hw_config_qcom_s3 = {
	HW_TYPE_QCOM_S3,
	OMXHW,
	{
		{ AV_IMAGE_NV12, HAL_PIXEL_FORMAT_YCbCr_420_SP , BUFFER_TYPE_SW, ANDROID_VERSION_UNKNOWN },
		{ AV_IMAGE_QCOM_NV12_TILED, HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED_ICS, BUFFER_TYPE_HW, ANDROID_VERSION_UNKNOWN},
		{ AV_IMAGE_QCOM_NV12_TILED, HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED, BUFFER_TYPE_HW, ANDROID_VERSION_JB},
		{ -1, 0, -1, ANDROID_VERSION_UNKNOWN },
	}
};

static hw_config_t hw_config_qcom_s4 = {
	HW_TYPE_QCOM_S4,
	SFDEC_DEFAULT,
	{
		{ AV_IMAGE_NV12, HAL_PIXEL_FORMAT_YCbCr_420_SP , BUFFER_TYPE_SW, ANDROID_VERSION_UNKNOWN },
		{ AV_IMAGE_QCOM_NV12_TILED, HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED_ICS, BUFFER_TYPE_HW, ANDROID_VERSION_UNKNOWN},
		{ AV_IMAGE_QCOM_NV12_TILED, HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED, BUFFER_TYPE_HW, ANDROID_VERSION_JB},
		{ -1, 0, -1, ANDROID_VERSION_UNKNOWN },
	}
};

static hw_config_t hw_config_exynos3 = {
	HW_TYPE_EXYNOS3,
	OMXHW,
	{
		{ AV_IMAGE_YV12, HAL_PIXEL_FORMAT_YV12, BUFFER_TYPE_SW, ANDROID_VERSION_UNKNOWN },
		{ AV_IMAGE_NV12, HAL_PIXEL_FORMAT_NV12_SAMSUNG, BUFFER_TYPE_HW, ANDROID_VERSION_UNKNOWN },
		{ -1, 0, -1, ANDROID_VERSION_UNKNOWN },
	}
};

static hw_config_t hw_config_exynos4 = {
	HW_TYPE_EXYNOS4,
	OMXHW,
	{
		{ AV_IMAGE_BGRA_32, HAL_PIXEL_FORMAT_BGRA_8888, BUFFER_TYPE_SW, ANDROID_VERSION_UNKNOWN },
		{ AV_IMAGE_NV12, HAL_PIXEL_FORMAT_NV12_ZERO_COPY_EX, BUFFER_TYPE_HW, ANDROID_VERSION_UNKNOWN },
		{ AV_IMAGE_YV12, HAL_PIXEL_FORMAT_YV12_ZERO_COPY_EX, BUFFER_TYPE_HW, ANDROID_VERSION_UNKNOWN },
		{ -1, 0, -1, ANDROID_VERSION_UNKNOWN },
	}
};

static hw_config_t hw_config_exynos5 = {
	HW_TYPE_EXYNOS5,
	OMXHW,
	{
		{ AV_IMAGE_YV12, HAL_PIXEL_FORMAT_YV12, BUFFER_TYPE_SW, ANDROID_VERSION_UNKNOWN },
		{ AV_IMAGE_EXYNOS_NV12_TILED, HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED_EX, BUFFER_TYPE_HW, ANDROID_VERSION_UNKNOWN },
		{ -1, 0, -1, ANDROID_VERSION_UNKNOWN },
	}
};

static hw_config_t hw_config_montblanc = {
	HW_TYPE_MONTBLANC,
	OMXHW,
	{
		{ AV_IMAGE_YV12, HAL_PIXEL_FORMAT_YV12, BUFFER_TYPE_SW, ANDROID_VERSION_UNKNOWN },
		{ AV_IMAGE_NV12, HAL_PIXEL_FORMAT_YCBCR42XMBN, BUFFER_TYPE_HW, ANDROID_VERSION_UNKNOWN },
		{ -1, 0, -1, ANDROID_VERSION_UNKNOWN },
	}
};

static hw_config_t hw_config_allwinner_a31 = {
	HW_TYPE_ALLWINER_A31,
	OMXHW,
	{
		{ AV_IMAGE_YV12, HAL_PIXEL_FORMAT_YV12, BUFFER_TYPE_SW, ANDROID_VERSION_UNKNOWN },
		{ AV_IMAGE_YV12, HAL_PIXEL_FORMAT_YV12, BUFFER_TYPE_HW, ANDROID_VERSION_UNKNOWN },
		{ -1, 0, -1, ANDROID_VERSION_UNKNOWN },
	}
};

static hw_config_t hw_config_mtk = {
	HW_TYPE_MTK,
	SFDEC_DEFAULT,
	DEFAULT_YUV_COLOR
};

static hw_config_t *hw_config_table [] = {
	&hw_config_unknown,
	&hw_config_default_kk,
	&hw_config_omap4,
	&hw_config_archos_omap4,
	&hw_config_rk29,
	&hw_config_rk30,
	&hw_config_tegra2,
	&hw_config_tegra3,
	&hw_config_qcom_s1,
	&hw_config_qcom_s2,
	&hw_config_qcom_s3,
	&hw_config_qcom_s4,
	&hw_config_exynos3,
	&hw_config_exynos4,
	&hw_config_exynos5,
	&hw_config_montblanc,
	&hw_config_allwinner_a31,
	&hw_config_mtk,
	NULL
};

static hw_config_t *get_hw_config(DEVICE_HW_TYPE hw_type)
{
	int i;

	for (i = 0; hw_config_table[i] != NULL; i++) {
		if (hw_config_table[i]->hw_type == hw_type)
			return hw_config_table[i];
	}
	return &hw_config_unknown;
}

static int android_get_hal_format_hw(int av_color, int buffer_type, DEVICE_HW_TYPE hw_type)
{
	int i = 0;
	int hal_index = -1;
	hw_config_t *hw_config;

	if (av_color == -1)
		return 0;

	hw_config = get_hw_config(hw_type);

	if (!hw_config)
		return 0;

	if( no_hw_buf && buffer_type == BUFFER_TYPE_HW ) {
		return 0;
	}
	while (hw_config->omx_color_table[i].av_color != -1 &&
	    (av_color != hw_config->omx_color_table[i].av_color ||
	    buffer_type != hw_config->omx_color_table[i].buffer_type ||
	    device_get_android_version() >= hw_config->omx_color_table[i].min_version
	    )) {
		if (hw_config->omx_color_table[i].av_color == av_color &&
		    buffer_type == hw_config->omx_color_table[i].buffer_type &&
		    device_get_android_version() >= hw_config->omx_color_table[i].min_version)
			hal_index = i;
		++i;
	}
	hal_index = (hal_index == -1)?i:hal_index;
	return hw_config->omx_color_table[hal_index].hal_format;
}

int android_get_hal_format(int av_color, int buffer_type) {
	return android_get_hal_format_hw(av_color, buffer_type, device_get_hw_type());
}

int android_get_hal_format_unknown(int av_color, int buffer_type) {
	return android_get_hal_format_hw(av_color, buffer_type, HW_TYPE_UNKNOWN);
}

static int android_get_av_color_hw(int buffer_type, DEVICE_HW_TYPE hw_type)
{
	int i = 0;
	hw_config_t *hw_config;

	hw_config = get_hw_config(hw_type);

	if (!hw_config)
		return -1;
	while (hw_config->omx_color_table[i].av_color != -1 &&
	    buffer_type != hw_config->omx_color_table[i].buffer_type)
		++i;
	return hw_config->omx_color_table[i].av_color;
}

int android_get_av_color(int buffer_type) {
	return android_get_av_color_hw(buffer_type, device_get_hw_type());
}

int android_get_av_color_unknown(int buffer_type) {
	return android_get_av_color_hw(buffer_type, HW_TYPE_UNKNOWN);
}

int android_can_hw_run_dec(int cpu_type)
{
	hw_config_t *hw_config;

	hw_config = get_hw_config(device_get_hw_type());
	if (!hw_config)
		return 0;
	return cpu_type <= hw_config->cpu_type;
}

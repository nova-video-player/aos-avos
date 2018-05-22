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

#include "avos_lifetime.h"
#include "debug.h"
#include "device_config.h"
#include "i18n.h"
#include "file.h"
#include "av.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#ifdef CONFIG_ANDROID
#include "androidndk_utils.h"
int acodecs_is_supported(int format, int is_video, int is_sw_allowed);
#else
#define PROP_VALUE_MAX  128
#endif



static int has_cpu = 0;
static int has_hdd = 0;
static int has_dsp = 1;
static int has_dsp_overdrive = 0;
static int has_archos_enhancement = 0;
static int zone = 0;
static DEVICE_HW_TYPE hw_type = HW_TYPE_UNKNOWN;
static DEVICE_ANDROID_VERSION android_version = ANDROID_VERSION_UNKNOWN;
static int android_api = 0;
static char android_pkg_name[256] = { 0 };
static char brand[PROP_VALUE_MAX] = { 0 };
static int has_pluginlib = 0;
static char *subtitle_path = NULL;
static int mp_decoder = MP_DECODER_ANY;
static int output_sample_rate = -1;

static const char *hw_type_names[] = DEVICE_HW_TYPE_NAMES;

DECLARE_DEBUG_TOGGLE( "hdd", has_hdd );
DECLARE_DEBUG_TOGGLE( "dsp", has_dsp );
DECLARE_DEBUG_PARAM ( "cpu", has_cpu );
DECLARE_DEBUG_TOGGLE( "dspod", has_dsp_overdrive );
DECLARE_DEBUG_PARAM(  "zone", zone );

int device_has_hdd()
{
	return has_hdd;
}

int device_has_dsp()
{
	return has_dsp;
}

int device_has_dsp_overdrive()
{
	static int dspod = -1;

	// forced by flag?
	if( has_dsp_overdrive )
		return 1;

	// check the fs
	if( dspod == -1 ) {
		STAT st;
		if( !file_stat( HDD_ROOT"dspod", &st ) ) {
serprintf("dspod!\n");
			dspod = 1;
		} else {
			dspod = 0;
		}
	}
	return dspod;
}

int device_zone()
{
	return zone;
}

int device_has_archos_enhancement()
{
	return has_archos_enhancement;
}

DEVICE_HW_TYPE device_get_hw_type()
{
	return hw_type;
}

const char *device_get_hw_type_name()
{
	if (hw_type >= (sizeof(hw_type_names)/sizeof(const char *)))
		return "[err: device idx not found]";
	return hw_type_names[hw_type];
}

DEVICE_ANDROID_VERSION device_get_android_version()
{
	return android_version;
}

int device_get_android_api()
{
	return android_api;
}

void device_config_init()
{
#ifdef CONFIG_ANDROID
	char value[PROP_VALUE_MAX];

#ifndef BIONIC
	__system_properties_init();
#endif
	if (!android_has_property_get()) {
		// no more property_get on newer platform
		// It's ok since we don't need it for newer devices
		hw_type = HW_TYPE_DEFAULT_KK;
		android_version = ANDROID_VERSION_L;
		goto end;
	}

	android_property_get("ro.board.has_hdd", value, "no");
	has_hdd = strcmp(value, "yes") == 0;

	android_property_get("ro.board.zone", value, "0");
	zone = atoi(value);
	android_property_get("ro.hardware", value, "0");
	has_archos_enhancement = strcmp(value, "archos") == 0;

	android_property_get("ro.board.platform", value, "0");

	// test ro.board.platform
	if (strcmp(value, "omap4") == 0) {
		hw_type = has_archos_enhancement ? HW_TYPE_ARCHOS_OMAP4 : HW_TYPE_OMAP4;
	}
	else if (strcmp(value, "rockchip") == 0)
		hw_type = HW_TYPE_RK29;
	else if (strncmp(value, "rk29", strlen("rk29")) == 0)
		hw_type = HW_TYPE_RK30;
	else if (strncmp(value, "rk30", strlen("rk30")) == 0)
		hw_type = HW_TYPE_RK30;
	else if (strncmp(value, "rk31", strlen("rk31")) == 0)
		hw_type = HW_TYPE_RK30;
	else if (strncmp(value, "rk32", strlen("rk32")) == 0)
		hw_type = HW_TYPE_RK32;
	else if (strcmp(value, "tegra3") == 0)
		hw_type = HW_TYPE_TEGRA3;
	else if (strcmp(value, "tegra") == 0)
		hw_type = HW_TYPE_TEGRA2;
	else if (strcmp(value, "msm7627a") == 0)
	        hw_type = HW_TYPE_QCOM_S1;
	else if (strcmp(value, "msm8960") == 0)
		hw_type = HW_TYPE_QCOM_S4;
	else if (strcmp(value, "msm8660") == 0)
		hw_type = HW_TYPE_QCOM_S3;
	else if (strncmp(value, "msm7630", strlen("msm7630")) == 0) {
		char value2[PROP_VALUE_MAX];

		hw_type = HW_TYPE_QCOM_S2;

		/*
		 * Unknow error with SEMC using hw buffers.
		 */
		android_property_get("ro.product.brand", value2, "0");
		if (strcmp(value2, "SEMC") == 0)
			hw_type = HW_TYPE_UNKNOWN;

	} else if (strcmp(value, "exynos5") == 0)
		hw_type = HW_TYPE_EXYNOS5;
	else if (strcmp(value, "exdroid") == 0)
		hw_type = HW_TYPE_ALLWINER_A31;
	else if (strcmp(value, "exDroid") == 0) {
		// test ro.hardware
		android_property_get("ro.hardware", value, "0");

		if (strcmp(value, "sun5i") == 0) {
			hw_type = HW_TYPE_ALLWINER_A13;
		} else {
			hw_type = HW_TYPE_ALLWINER_A31;
		}
	} else if (strcmp(value, "exynos4") == 0)
		hw_type = HW_TYPE_EXYNOS4;
	else if (strcmp(value, "s5pc110") == 0)
		hw_type = HW_TYPE_EXYNOS3;
	else if (strcmp(value, "montblanc") == 0)
		hw_type = HW_TYPE_MONTBLANC;
	else if(strcmp(value, "meson8") == 0)
		hw_type = HW_TYPE_AMLOGIC;
	else {
		// test ro.hardware
		android_property_get("ro.hardware", value, "0");

		if (strncmp(value, "mt65", strlen("mt65")) == 0 ||
		    strncmp(value, "mt83", strlen("mt83")) == 0)
			hw_type = HW_TYPE_MTK;
		else if(strcmp(value, "fbx6lc") == 0)
			hw_type = HW_TYPE_FBX;
	}

	android_property_get("ro.build.version.sdk", value, "0");
	android_api = atoi(value);
	if (android_api > 13 && android_api <= 15)
		android_version = ANDROID_VERSION_ICS;
	else if (android_api == 16)
		android_version = ANDROID_VERSION_JB;
	else if (android_api == 17)
		android_version = ANDROID_VERSION_JB_4_2;
	else if (android_api == 18)
		android_version = ANDROID_VERSION_JB_4_3;
	else if (android_api > 18)
		android_version = ANDROID_VERSION_KK;
	if (android_api > 18) {
		if (hw_type == HW_TYPE_RK30 || hw_type == HW_TYPE_RK29 ||
		    hw_type == HW_TYPE_EXYNOS3 || hw_type == HW_TYPE_EXYNOS4 || hw_type == HW_TYPE_EXYNOS5)
			hw_type = HW_TYPE_DEFAULT_KK;
	}

	android_property_get("ro.product.brand", brand, "cravatek");
#else
	struct stat st;
	if( !stat( "/sys/devices/platform/jm20329", &st ) ) {
		has_hdd = 1;
	} else {
		has_hdd = 0;
	}
	has_archos_enhancement = 1;
#endif
#ifdef CONFIG_ANDROID
end:
#endif
	serprintf("device_config.has_hdd     %d\n", has_hdd);
	serprintf("device_config.has_dsp     %d\n", has_dsp);
	serprintf("device_config.has_dsp_od  %d\n", has_dsp);
	serprintf("device_config.zone        %d\n", zone);
	serprintf("device_config.has_archos_enhancement %d\n", has_archos_enhancement);
	serprintf("device_config.hw_type     %s\n", device_get_hw_type_name());
	serprintf("device_config.cpu_count   %d\n", device_get_cpu_count());
}

void device_config_set_android_pkg_name(const char *pkg_name)
{
	strncpy(android_pkg_name, pkg_name, 256);
	android_pkg_name[255] = '\0';
}

const char *device_config_get_android_pkg_name()
{
	return android_pkg_name;
}

const char *device_config_get_brand()
{
	return brand;
}

void device_config_set_pluginlib(int pluginlib)
{
	has_pluginlib = pluginlib;
}

int device_config_has_pluginlib()
{
#ifdef CONFIG_ANDROID
	return has_pluginlib;
#else
	return 1;
#endif
}


int device_get_cpu_count(void)
{
	if( has_cpu )
		return has_cpu;
#ifdef CONFIG_ANDROID
	return android_cpu_count();
#else
	return 1;
#endif
}

void device_config_set_subtitlepath(const char *path)
{
	if (subtitle_path)
		free(subtitle_path);
	subtitle_path = strdup(path);
}

const char *device_config_get_subtitlepath(void)
{
	return subtitle_path;
}

void device_config_set_decoder(int decoder)
{
	mp_decoder = decoder;
	serprintf("device_config.mp_decoder  %d\n", mp_decoder);
}

int device_config_get_decoder(void)
{
	return mp_decoder;
}

void device_config_set_output_sample_rate(int sample_rate)
{
	output_sample_rate = sample_rate;
	serprintf("device_config.output_sample_rate  %d\n", output_sample_rate);
}

int device_config_get_output_sample_rate()
{
	return output_sample_rate;
}

static int is_supported(int format, int is_video)
{
#ifdef CONFIG_ANDROID
	if (device_config_has_pluginlib())
		return 1;
	if (acodecs_is_supported(format, is_video, 1))
		return 1;
	return 1;
#else
	return 1;
#endif
}

int device_config_is_audio_format_supported(int format)
{
	switch (format) {
	case WAVE_FORMAT_MSAUDIO_LOSSLESS:
	case WAVE_FORMAT_MSAUDIO1:
	case WAVE_FORMAT_MSAUDIO2:
	case WAVE_FORMAT_MSAUDIO3:
	case WAVE_FORMAT_MSAUDIO_SPEECH:
	case WAVE_FORMAT_DTS:
	case WAVE_FORMAT_MS_DTS:
	case WAVE_FORMAT_AC3:
	case WAVE_FORMAT_DOLBY_AC3_SPDIF:
	case WAVE_FORMAT_ESST_AC3:
	case WAVE_FORMAT_DNET:
		return is_supported(format, 0);
	default:
		return 1;
	}
}

int device_config_is_video_format_supported(int format)
{
	switch (format) {
	case VIDEO_FORMAT_MPEG:
	case VIDEO_FORMAT_WMV1:
	case VIDEO_FORMAT_WMV2:
	case VIDEO_FORMAT_WMV3:
	case VIDEO_FORMAT_WMV3B:
	case VIDEO_FORMAT_VC1:
	case VIDEO_FORMAT_HEVC:
		return is_supported(format, 1);
	default:
		return 1;
	}
}

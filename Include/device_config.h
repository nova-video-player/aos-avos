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

#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

typedef enum { // don't forget to add string in DEVICE_HW_TYPE_NAMES
	HW_TYPE_UNKNOWN,
	HW_TYPE_DEFAULT_KK, // default on android 4.4 and above
	HW_TYPE_OMAP4,
	HW_TYPE_ARCHOS_OMAP4,
	HW_TYPE_RK29,
	HW_TYPE_RK30,
	HW_TYPE_RK32,
	HW_TYPE_TEGRA2,
	HW_TYPE_TEGRA3,
	HW_TYPE_QCOM_S1,
	HW_TYPE_QCOM_S2,
	HW_TYPE_QCOM_S3,
	HW_TYPE_QCOM_S4,
	HW_TYPE_EXYNOS3,
	HW_TYPE_EXYNOS4,
	HW_TYPE_EXYNOS5,
	HW_TYPE_MONTBLANC,
	HW_TYPE_ALLWINER_A31,
	HW_TYPE_ALLWINER_A13 = HW_TYPE_ALLWINER_A31,
	HW_TYPE_MTK,
	HW_TYPE_AMLOGIC,
	HW_TYPE_FBX,
} DEVICE_HW_TYPE;

#define DEVICE_HW_TYPE_NAMES { \
	"unknown", \
	"default_kk", \
	"omap4", \
	"omap4_archos", \
	"rk29", \
	"rk30", \
	"rk32", \
	"tegra2", \
	"tegra3", \
	"qcom_s1", \
	"qcom_s2", \
	"qcom_s3", \
	"qcom_s4", \
	"s5pc110", \
	"exynos4", \
	"exynos5", \
	"montblanc", \
	"allWinner_a31", \
	"mtk", \
	"amlogic", \
	"freebox" \
}

// keep in sync with: lgf/mydroid/frameworks/base/services/java/com/android/server/PackageManagerService.java
typedef enum {
	ZONE_REST_OF_THE_WORLD = 0,
	ZONE_GOOGLE_BOOKS_AND_MUSIC,
	ZONE_NO_YOUTUBE,
	ZONE_NO_GOOGLE_APPS,
} DEVICE_ZONE;

typedef enum {
	ANDROID_VERSION_UNKNOWN,
	ANDROID_VERSION_ICS,
	ANDROID_VERSION_JB,
	ANDROID_VERSION_JB_4_2,
	ANDROID_VERSION_JB_4_3,
	ANDROID_VERSION_KK,
	ANDROID_VERSION_L
} DEVICE_ANDROID_VERSION;

// in sync with MediaLib/src/com/archos/medialib/LibAvos.java

enum mp_decoder_type {
	MP_DECODER_ANY,
	MP_DECODER_SW,
	MP_DECODER_HW_OMX,
	MP_DECODER_HW_SFDEC_OMXCODEC,
	MP_DECODER_HW_SFDEC_MEDIACODEC,
	MP_DECODER_HW_OMXPLUS,
};

int device_has_hdd( void );
int device_has_dsp( void );
int device_has_dsp_overdrive( void );
int device_zone( void );
int device_has_archos_enhancement( void );
DEVICE_HW_TYPE device_get_hw_type( void );
const char *device_get_hw_type_name( void );
DEVICE_ANDROID_VERSION device_get_android_version( void );
int device_get_android_api( void );
const char *device_config_get_android_pkg_name();
const char *device_config_get_android_pkg_name();
const char *device_config_get_brand();
int device_config_has_pluginlib();
int device_get_cpu_count(void);
const char *device_config_get_subtitlepath(void);
int device_config_get_decoder(void);
int device_config_get_output_sample_rate(void);
int device_config_is_audio_format_supported(int format);
int device_config_is_video_format_supported(int format);
#endif

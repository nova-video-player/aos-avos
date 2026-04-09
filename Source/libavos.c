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

#include <stdlib.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "global.h"
#include "log.h"
#include "av.h"
#include "avos_lifetime.h"
#include "audio_interface.h"
#include "debug.h"
#include "mainloop.h"
#include "pthread.h"
#include "timers.h"
#include "file.h"
#include "device_config.h"
#include "i18n.h"
#include "audio_spdif.h"
#include "stream.h"

#ifdef CONFIG_ANDROID
#include "jni.h"

JavaVM *myVm = NULL;
jobject myClassLoader;
jmethodID myFindClassMethod;
#endif

void device_config_init();
void device_config_set_android_pkg_name(const char *pkg_name);
void device_config_set_pluginlib(int pluginlib);
void device_config_set_subtitlepath(const char *path);
void device_config_set_decoder(int decoder);
void device_config_set_audio_interface(int audio_interface);
void device_config_set_audio_decoder(int audio_decoder);
void device_config_set_mediacodec_audio_capabilities(int64_t capabilities);
void device_config_set_spatializer_capabilities(int capabilities);
void device_config_set_spatializer_enabled(int enabled);
void device_config_set_output_sample_rate(int sample_rate);
static pthread_t mainloop_thread;

static long hdmi_audio_codecs_flag = 0;
static int *pcm_channel_masks = NULL;
static int pcm_channel_masks_count = 0;

//#define DUMP_OMX

void libavos_init(const char *name, const char *pkg_name, int has_pluginlib)
{
	LOG_open_name(name);
	serprintf("libavos_init\n");
	device_config_init();
	device_config_set_android_pkg_name(pkg_name);
	device_config_set_pluginlib(has_pluginlib);
	audio_interface_init();
#ifdef DUMP_OMX
	debug_do_cmd("dumpomx");
#endif
}

void libavos_exit()
{
	serprintf("libavos_exit");
	LOG_close();
	audio_interface_exit();
}

static void *mainloop_routine(void *ctx)
{
	serprintf("creating mainloop (for debug only)\n");
	avos_init(AVOS_RUNLEVEL_PLATFORM);

	Timers_init( &gui_timers );

	avos_init(AVOS_RUNLEVEL_BC);
	avos_init(AVOS_RUNLEVEL_APP);
	mainloop_init();
	mainloop_enter();
	mainloop_deinit();

	serprintf("leaving  mainloop (for debug only)\n");
	return NULL;
}

void libavos_debug_init()
{
	pthread_create(&mainloop_thread, NULL, mainloop_routine, NULL);
}

void libavos_debug_exit()
{
	avos_exit(AVOS_RUNLEVEL_APP);
	avos_exit(AVOS_RUNLEVEL_BC);
	avos_exit(AVOS_RUNLEVEL_PLATFORM);
	mainloop_exit();
	pthread_join(mainloop_thread, NULL);
	avos_clean_files();
}

void libavos_avsh(const char *cmd)
{
#ifdef DEBUG_MSG
	serprintf("\n-------------------------------------------------------\n");
	debug_do_cmd(cmd);
	serprintf("-------------------------------------------------------\n");
#endif
}

static int ac3_recoding_enabled = 0;
static int pcm_output_max_channels = 0;

static void log_audio_capabilities64(const char *label, int64_t flags)
{
	int first = 1;
	serprintf("%s: flags=0x%" PRIx64 " codecs=", label, flags);
	if( flags & ((int64_t)1 << 5) ) {
		serprintf("%sAC3", first ? "" : ",");
		first = 0;
	}
	if( flags & ((int64_t)1 << 6) ) {
		serprintf("%sE_AC3", first ? "" : ",");
		first = 0;
	}
	if( flags & ((int64_t)1 << 7) ) {
		serprintf("%sDTS", first ? "" : ",");
		first = 0;
	}
	if( flags & ((int64_t)1 << 8) ) {
		serprintf("%sDTS_HD", first ? "" : ",");
		first = 0;
	}
	if( flags & ((int64_t)1 << 9) ) {
		serprintf("%sMP3", first ? "" : ",");
		first = 0;
	}
	if( flags & ((int64_t)1 << 10) ) {
		serprintf("%sAAC_LC", first ? "" : ",");
		first = 0;
	}
	if( flags & ((int64_t)1 << 14) ) {
		serprintf("%sTRUEHD", first ? "" : ",");
		first = 0;
	}
	if( flags & ((int64_t)1 << 18) ) {
		serprintf("%sE_AC3_JOC", first ? "" : ",");
		first = 0;
	}
	if( flags & ((int64_t)1 << 20) ) {
		serprintf("%sOPUS", first ? "" : ",");
		first = 0;
	}
	if( flags & ((int64_t)1 << 29) ) {
		serprintf("%sDTS_HD_MA", first ? "" : ",");
		first = 0;
	}
	if( first ) {
		serprintf("<none>");
	}
	serprintf("\n");
}

static void log_spatializer_capabilities(const char *label, int capabilities)
{
	int first = 1;
	serprintf("%s: flags=0x%x state=", label, capabilities);
	if( capabilities & 1 ) {
		serprintf("%ssupported", first ? "" : ",");
		first = 0;
	}
	if( capabilities & (1 << 1) ) {
		serprintf("%savailable", first ? "" : ",");
		first = 0;
	}
	if( capabilities & (1 << 2) ) {
		serprintf("%senabled", first ? "" : ",");
		first = 0;
	}
	if( first ) {
		serprintf("<none>");
	}
	serprintf("\n");
}

void libavos_set_subtitlepath(const char *path)
{
	device_config_set_subtitlepath(path);
}

void libavos_set_decoder(int decoder)
{
	device_config_set_decoder(decoder);
}

void libavos_set_audio_interface(int audio_interface)
{
	device_config_set_audio_interface(audio_interface);
}

void libavos_set_audio_decoder(int audio_decoder)
{
	device_config_set_audio_decoder(audio_decoder);
}

void libavos_set_mediacodec_audio_capabilities(int64_t capabilities)
{
	log_audio_capabilities64("libavos_set_mediacodec_audio_capabilities", capabilities);
	device_config_set_mediacodec_audio_capabilities(capabilities);
}

void libavos_set_spatializer_capabilities(int capabilities)
{
	log_spatializer_capabilities("libavos_set_spatializer_capabilities", capabilities);
	device_config_set_spatializer_capabilities(capabilities);
}

void libavos_set_spatializer_enabled(int enabled)
{
	serprintf("libavos_set_spatializer_enabled: %d\n", enabled);
	device_config_set_spatializer_enabled(enabled);
}

void libavos_set_codepage(int codepage)
{
	I18N_set_codepage(codepage);
}

void libavos_set_output_sample_rate(int sample_rate)
{
	device_config_set_output_sample_rate(sample_rate);
}

int libavos_get_ac3_recoding_enabled(void)
{
	return ac3_recoding_enabled;
}

void libavos_set_passthrough(int force_passthrough)
{
	serprintf("libavos_set_passthrough: mode=%d\n", force_passthrough);
#ifdef CONFIG_SPDIF
	audio_interface_exit();
	// Mode 3 is AC3 recoding: enable AC3 filter
	// The actual passthrough mode (1 or 2) will be determined at sink creation time
	// based on current HDMI capability state via get_hdmi_supports_iec()
	if (force_passthrough == 3) {
		serprintf("libavos_set_passthrough: enabling AC3 recoding (will use Mode 1 or 2 based on IEC61937 capability at sink creation)\n");
		ac3_recoding_enabled = 1;
		spdif_set_passthrough(1);  // Default to mode 1; stream_audio.c will override if IEC unavailable
	} else {
		serprintf("libavos_set_passthrough: disabling AC3 recoding\n");
		ac3_recoding_enabled = 0;
		spdif_set_passthrough(force_passthrough);
	}
	audio_interface_init();
#endif
}

void libavos_set_hdmi_supported_audio_codecs(long flag)
{
#ifdef CONFIG_ANDROID
	log_audio_capabilities64("libavos_set_hdmi_supported_audio_codecs", flag);
	set_hdmi_supported_audio_codecs(flag);
#endif
}

void libavos_set_max_pcm_channels(int max_channels)
{
	if( max_channels < 0 )
		max_channels = 0;
	pcm_output_max_channels = max_channels;
	serprintf("libavos_set_max_pcm_channels: %d\n", pcm_output_max_channels);
}

int libavos_get_max_pcm_channels(void)
{
	return pcm_output_max_channels;
}

void libavos_set_pcm_channel_masks(const int *masks, int count)
{
	if (pcm_channel_masks) {
		free(pcm_channel_masks);
		pcm_channel_masks = NULL;
		pcm_channel_masks_count = 0;
	}
	if (!masks || count <= 0) {
		return;
	}
	pcm_channel_masks = (int *)malloc(sizeof(int) * count);
	if (!pcm_channel_masks) {
		serprintf("libavos_set_pcm_channel_masks: OOM for %d masks\n", count);
		return;
	}
	memcpy(pcm_channel_masks, masks, sizeof(int) * count);
	pcm_channel_masks_count = count;
	serprintf("libavos_set_pcm_channel_masks: %d\n", pcm_channel_masks_count);
}

int libavos_pcm_channel_mask_supported(int mask)
{
	int i;
	if (!pcm_channel_masks || pcm_channel_masks_count <= 0) {
		return -1;
	}
	for (i = 0; i < pcm_channel_masks_count; i++) {
		if (pcm_channel_masks[i] == mask) {
			return 1;
		}
	}
	return 0;
}

void libavos_set_audio_speed(float speed)
{
	audio_interface_set_audio_speed(speed);
}

void libavos_enable_audio_speed(int enable)
{
	audio_interface_enable_audio_speed(enable);
}

void libavos_disable_atempo_filter(int disable)
{
	audio_interface_set_using_atempo(disable ? 0 : 1);
	stream_disable_atempo_filter(disable);
}

void libavos_set_parser_sync_mode(int mode)
{
	stream_parser_set_sync_mode(mode);
}

void libavos_set_downmix(int downmix)
{
	stream_set_audio_downmix(downmix);
}

void libavos_set_default_stream_buffer_size(int size)
{
	define_default_stream_buffer_size(size);
}

void libavos_set_default_stream_max_iframe_size(int size)
{
	define_default_stream_max_iframe_size(size);
}

int (*libavos_transform_audio)(float* buf, int nsamples);
void libavos_set_audio_transform(int (*transformer)(float* buf, int nsamples)) {
	libavos_transform_audio = transformer;
}

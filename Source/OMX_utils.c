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

#include <pthread.h>
#include <dlfcn.h>

#include "global.h"
#include "types.h"
#include "debug.h"
#include "av.h"
#include "util.h"
#include "OMX_utils.h"
#include "android_config.h"

#define OMXLOG(fmt, ...) serprintf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__)

#if defined(CONFIG_OMX_IOMX)
#define LIB_OMX_NAME "libiomx.so"
#define OMX_PREFIX "I"
#else
#define LIB_OMX_NAME "libMtkOmxCore.so"
#define OMX_PREFIX "Mtk_"
//#define LIB_OMX_NAME "libnvomx.so"
#endif

static pthread_mutex_t libOMX_mtx = PTHREAD_MUTEX_INITIALIZER;
static void *libOMX_handle = NULL;

static OMX_ERRORTYPE libOMX_Init(void);

OMX_ERRORTYPE (*fp_OMX_Init) (void) = libOMX_Init;
OMX_ERRORTYPE (*fp_OMX_Deinit) (void);
OMX_ERRORTYPE (*fp_OMX_GetHandle) (OMX_HANDLETYPE *, OMX_STRING, OMX_PTR, OMX_CALLBACKTYPE *);
OMX_ERRORTYPE (*fp_OMX_FreeHandle) (OMX_HANDLETYPE);
OMX_ERRORTYPE (*fp_OMX_ComponentNameEnum)(OMX_STRING, OMX_U32, OMX_U32);
OMX_ERRORTYPE (*fp_OMX_GetRolesOfComponent)(OMX_STRING, OMX_U32 *, OMX_U8 **);
OMX_ERRORTYPE (*fp_OMXAndroid_EnableGraphicBuffers)(OMX_HANDLETYPE component, OMX_U32 port_index, OMX_BOOL enable);
OMX_ERRORTYPE (*fp_OMXAndroid_GetGraphicBufferUsage)(OMX_HANDLETYPE component, OMX_U32 port_index, OMX_U32* usage);

#ifdef CONFIG_OMX_CORE
OMX_ERRORTYPE COMXAndroid_EnableGraphicBuffers(OMX_HANDLETYPE omx_handle, OMX_U32 port_index, OMX_BOOL enable)
{
	OMX_INDEXTYPE index;
	OMX_ERRORTYPE err;
	EnableAndroidNativeBuffersParams params;

	err = OMX_GetExtensionIndex(omx_handle, "OMX.google.android.index.enableAndroidNativeBuffers", &index);
	if (err != OMX_ErrorNone)
		return err;

	OMX_INIT_STRUCTURE(params);
	params.nPortIndex = port_index;
	params.bEnable = enable;

	err = OMX_SetParameter(omx_handle, index, &params);

	return err;
}

OMX_ERRORTYPE COMXAndroid_GetGraphicBufferUsage(OMX_HANDLETYPE omx_handle, OMX_U32 port_index, OMX_U32 *usage)
{
	OMX_INDEXTYPE index;
	OMX_ERRORTYPE err;
	GetAndroidNativeBufferUsageParams params;

	err = OMX_GetExtensionIndex(omx_handle, "OMX.google.android.index.getAndroidNativeBufferUsage", &index);
	if (err != OMX_ErrorNone)
		return err;

	OMX_INIT_STRUCTURE(params);
	params.nPortIndex = port_index;

	err = OMX_GetParameter(omx_handle, index, &params);
	*usage = params.nUsage;

	return err;
}
#endif

static OMX_ERRORTYPE libOMX_Init(void)
{
#define dll_sym(x) do { \
	fp_##x = dlsym(libOMX_handle, OMX_PREFIX #x); \
	if (!fp_##x) { \
		OMXLOG("error: no %s in '%s'", #x, LIB_OMX_NAME); \
		goto err; \
	} \
} while (0)

	pthread_mutex_lock(&libOMX_mtx);

	if (libOMX_handle)
		goto end;

#if defined(CONFIG_OMX_IOMX)
	libOMX_handle = RTLD_DEFAULT;
#else
	libOMX_handle = dlopen(LIB_OMX_NAME, RTLD_NOW);
	if (!libOMX_handle) {
		OMXLOG("dlopen(%s) failed", LIB_OMX_NAME);
		goto err;
	}
#endif

	dll_sym(OMX_Init);
	dll_sym(OMX_Deinit);
	dll_sym(OMX_GetHandle);
	dll_sym(OMX_FreeHandle);
	dll_sym(OMX_ComponentNameEnum);
	dll_sym(OMX_GetRolesOfComponent);
#ifdef CONFIG_OMX_CORE
	fp_OMXAndroid_EnableGraphicBuffers = COMXAndroid_EnableGraphicBuffers;
	fp_OMXAndroid_GetGraphicBufferUsage = COMXAndroid_GetGraphicBufferUsage;
#else
	dll_sym(OMXAndroid_EnableGraphicBuffers);
	dll_sym(OMXAndroid_GetGraphicBufferUsage);
#endif
end:
	pthread_mutex_unlock(&libOMX_mtx);
	return fp_OMX_Init();
err:
	libOMX_handle = NULL;
	fp_OMX_Init = libOMX_Init;
	pthread_mutex_unlock(&libOMX_mtx);
	return OMX_ErrorUndefined;
}

static const struct {
	int                  colorspace;
	OMX_COLOR_FORMATTYPE omx_format;
	unsigned int         size_mul;
	unsigned int         line_mul;
	unsigned int         line_chroma_div;
	unsigned int         specific_stride;
	const char           *name;
} chroma_format_table [] = {
	{ AV_IMAGE_YUV_422, OMX_COLOR_FormatYUV422Planar,		4, 2, 2, 0, "YUV_422" },
	{ AV_IMAGE_NV12,    OMX_COLOR_FormatYUV420SemiPlanar,		3, 1, 1, 0, "NV12" },
	{ AV_IMAGE_YV12,    OMX_COLOR_FormatYUV420Planar,		3, 1, 0, 0, "YV12" },
	/* HAL HW specific colors */
	{ AV_IMAGE_NV12,    HAL_PIXEL_FORMAT_NV12_TI,	3, 1, 1, 4096, "NV12(TI)" },
	{ AV_IMAGE_NV12,    OMX_TI_COLOR_FormatYUV420PackedSemiPlanar,	3, 1, 1, 4096, "NV12(TI)" },
	{ AV_IMAGE_NV21,    HAL_OMX_QCOM_COLOR_FormatYVU420SemiPlanar, 3, 1, 1, 0, "NV21(QCOM)"},
	{ AV_IMAGE_QCOM_NV12_TILED, HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED, 3, 1, 1, 0, "NV12_TILED(QCOM)" },
	{ AV_IMAGE_NV12,    HAL_PIXEL_FORMAT_NV12_RK,        3, 1, 1, 0, "NV12(RK)" },
	{ AV_IMAGE_EXYNOS_NV12_TILED, HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED_EX,	3, 1, 1, 0, "NV12(Exynos)" },
	{ AV_IMAGE_YV12,    HAL_OMX_NV_COLOR_FormatYUV420Planar,		3, 1, 0, 0, "YV12(TEGRA)" },
	{ AV_IMAGE_BGRA_32, HAL_OMX_RK_COLOR_FormatBGRA,			3, 1, 0, 0, "BGRA(RK)" },
	{ AV_IMAGE_NV12,    OMX_STE_COLOR_FormatYUV420PackedSemiPlanarMB,	3, 1, 0, 0, "NV12(STE)" },
	{ -1,               OMX_COLOR_FormatUnused,			-1, 0, 0, 0, "unused" }
};

static inline int omx_get_format_index(OMX_COLOR_FORMATTYPE format)
{
	int i = 0;

	while (chroma_format_table[i].omx_format != OMX_COLOR_FormatUnused &&
	    format != chroma_format_table[i].omx_format)
		++i;
	return i;
}

void omx_get_format_sizes(OMX_COLOR_FORMATTYPE format, int width, int height, OMX_U32 *psize, OMX_U32 *pstride)
{
	int i = omx_get_format_index(format);
	OMX_U32 stride;

	width  = (width  + 15) & ~0xF;
	height = (height + 15) & ~0xF;

	stride = chroma_format_table[i].specific_stride;
	if (stride == 0)
		stride = width * chroma_format_table[i].line_mul;

	if (pstride)
		*pstride = stride;
	if (psize) {
		*psize = stride * height * chroma_format_table[i].size_mul / 2;
	}
}

OMX_COLOR_FORMATTYPE omx_get_colorspace(OMX_COLOR_FORMATTYPE format)
{
	int i = omx_get_format_index(format);

	return chroma_format_table[i].colorspace;
}

const char *omx_get_format_name(OMX_COLOR_FORMATTYPE format)
{
	int i = omx_get_format_index(format);

	return chroma_format_table[i].name;
}

struct format_table {
	int format;
	int omx_codec;
	const char *role_list[3];
};

static const struct format_table video_format_table[] = {
	{ VIDEO_FORMAT_MPEG, OMX_VIDEO_CodingMPEG2, { "video_decoder.mpeg2", "video_decoder.m2v", NULL } },
	{ VIDEO_FORMAT_MPG4, OMX_VIDEO_CodingMPEG4, { "video_decoder.mpeg4", "video_decoder.m4v", NULL } },
	{ VIDEO_FORMAT_H264, OMX_VIDEO_CodingAVC,   { "video_decoder.avc", NULL } },
	{ VIDEO_FORMAT_HEVC, OMX_VIDEO_CodingHEVC,  { "video_decoder.hevc", NULL } },
	{ VIDEO_FORMAT_H263, OMX_VIDEO_CodingH263,  { "video_decoder.h263", NULL } },
	{ VIDEO_FORMAT_WMV1, OMX_VIDEO_CodingWMV,   { "video_decoder.wmv", NULL } },
	{ VIDEO_FORMAT_WMV2, OMX_VIDEO_CodingWMV,   { "video_decoder.wmv", NULL } },
	{ VIDEO_FORMAT_WMV3, OMX_VIDEO_CodingWMV,   { "video_decoder.wmv", NULL } },
	{ VIDEO_FORMAT_WMV3B, OMX_VIDEO_CodingWMV,   { "video_decoder.wmv", NULL } },
	{ VIDEO_FORMAT_VC1,  OMX_VIDEO_CodingWMV,   { "video_decoder.wmv", NULL } },
	{ VIDEO_FORMAT_VC1,  OMX_VIDEO_CodingWMV,   { "video_decoder.vc1", NULL } },
	{ VIDEO_FORMAT_MJPG, OMX_VIDEO_CodingMJPEG, { "video_decoder.jpeg", NULL } },
	{ VIDEO_FORMAT_RV10, OMX_VIDEO_CodingRV,    { "video_decoder.rv", NULL } },
	{ VIDEO_FORMAT_RV20, OMX_VIDEO_CodingRV,    { "video_decoder.rv", NULL } },
	{ VIDEO_FORMAT_RV30, OMX_VIDEO_CodingRV,    { "video_decoder.rv", NULL } },
	{ VIDEO_FORMAT_RV40, OMX_VIDEO_CodingRV,    { "video_decoder.rv", NULL } },
	{ -1, OMX_VIDEO_CodingUnused, { "unused", NULL } }
};

static const struct format_table audio_format_table[] = {
        { WAVE_FORMAT_MPEGLAYER3, OMX_AUDIO_CodingMP3, "audio_decoder.mp3", NULL },
        { WAVE_FORMAT_MPEG, OMX_AUDIO_CodingMP3, "audio_decoder.mp1", NULL },
        { WAVE_FORMAT_MPEG, OMX_AUDIO_CodingMP3, "audio_decoder.mp2", NULL },
        { WAVE_FORMAT_VOICEAGE_AMR, OMX_AUDIO_CodingAMR, "audio_decoder.amrnb", NULL },
        { WAVE_FORMAT_VOICEAGE_AMR_WB, OMX_AUDIO_CodingAMR, "audio_decoder.amrwb", NULL },
        { WAVE_FORMAT_AAC , OMX_AUDIO_CodingAAC, "audio_decoder.aac", NULL },
        { WAVE_FORMAT_OGG1, OMX_AUDIO_CodingVORBIS, "audio_decoder.vorbis", NULL },
        { WAVE_FORMAT_OGG2, OMX_AUDIO_CodingVORBIS, "audio_decoder.vorbis", NULL },
        { WAVE_FORMAT_OGG3, OMX_AUDIO_CodingVORBIS, "audio_decoder.vorbis", NULL },
        { WAVE_FORMAT_OGG1_PLUS, OMX_AUDIO_CodingVORBIS, "audio_decoder.vorbis", NULL },
        { WAVE_FORMAT_OGG2_PLUS, OMX_AUDIO_CodingVORBIS, "audio_decoder.vorbis", NULL },
        { WAVE_FORMAT_OGG3_PLUS, OMX_AUDIO_CodingVORBIS, "audio_decoder.vorbis", NULL },
	{ WAVE_FORMAT_MSAUDIO_LOSSLESS, OMX_AUDIO_CodingWMA, { "audio_decoder.wma", NULL } },
	{ WAVE_FORMAT_MSAUDIO1, OMX_AUDIO_CodingWMA, { "audio_decoder.wma", NULL } },
	{ WAVE_FORMAT_MSAUDIO2, OMX_AUDIO_CodingWMA, { "audio_decoder.wma", NULL } },
	{ WAVE_FORMAT_MSAUDIO3, OMX_AUDIO_CodingWMA, { "audio_decoder.wma", NULL } },
	{ WAVE_FORMAT_MSAUDIO_SPEECH, OMX_AUDIO_CodingWMA,   { "audio_decoder.wma", NULL } },
	{ -1, OMX_AUDIO_CodingUnused, { "unused", NULL } }
};

static const char **omx_get_role_list(int format, int is_video)
{
	int i = 0;
	const struct format_table *format_table = is_video ? video_format_table : audio_format_table;

	while (format_table[i].format != -1 && format != format_table[i].format)
		++i;
	return (const char **)format_table[i].role_list;
}

int omx_get_codec(int format, int is_video)
{
	int i = 0;
	const struct format_table *format_table = is_video ? video_format_table : audio_format_table;

	while (format_table[i].format != -1 && format != format_table[i].format)
		++i;
	return format_table[i].omx_codec;
}

#define COMPONENT_SW_PREFIX "OMX.google."

char **omx_get_components(int format, int is_video, const char **prole)
{
	int i, j, role_found, component_idx = 0;
	char name[OMX_MAX_STRINGNAME_SIZE];
	const char **role_list;
	char **component_list = NULL;
	OMX_ERRORTYPE err;
	OMX_U32 role_nb;
	OMX_U8 **comp_role_list = NULL;

	role_list = omx_get_role_list(format, is_video);
	if (!role_list)
		return NULL;
	for (i = 0; ; ++i) {
		err = OMX_ComponentNameEnum(name, OMX_MAX_STRINGNAME_SIZE, i);
		if (err != OMX_ErrorNone)
			break;

		err = OMX_GetRolesOfComponent(name, &role_nb, NULL);
		if (err != OMX_ErrorNone || !role_nb)
			continue;

		comp_role_list = realloc(comp_role_list, role_nb * (sizeof(OMX_U8*) + OMX_MAX_STRINGNAME_SIZE));
		if(!comp_role_list)
			break;
		for (j = 0; j < role_nb; j++)
			comp_role_list[j] = ((OMX_U8 *)(&comp_role_list[role_nb])) + j * OMX_MAX_STRINGNAME_SIZE;

		err = OMX_GetRolesOfComponent(name, &role_nb, comp_role_list);
		if (err != OMX_ErrorNone || !role_nb)
			continue;

		role_found = 0;
		for (j = 0; j < role_nb; j++) {
			const char **role_it = role_list;
			const char *role;

			while ((role = *(role_it++)) != NULL) {
				if (strcmp(role, comp_role_list[j]) == 0) {
					role_found = 1;
					*prole = role;
					break;
				}
			}
		}
		if (!role_found)
			continue;
		component_list = realloc(component_list, (component_idx + 2)* sizeof(char*));
		if (!component_list)
			break;
		component_list[component_idx] = strdup(name);
		if (!component_list[component_idx])
			break;
		component_idx++;
	}
	if (comp_role_list)
		free(comp_role_list);

	if (component_list)
		component_list[component_idx] = NULL;
	return component_list;
}

static const char *event_names[] = {
	"OMX_EventCmdComplete", "OMX_EventError", "OMX_EventMark",
	"OMX_EventPortSettingsChanged", "OMX_EventBufferFlag",
	"OMX_EventResourcesAcquired", "OMX_EventComponentResumed",
	"OMX_EventDynamicResourcesAvailable", "OMX_EventPortFormatDetected",
	"WARNING: Unknown OMX_Event"
};

static const char *command_names[] = {
	"OMX_CommandStateSet", "OMX_CommandFlush", "OMX_CommandPortDisable",
	"OMX_CommandPortEnable", "OMX_CommandMarkBuffer",
	"WARNING: Unknown OMX_Command"
};

static const char *state_names[] = {
	"OMX_StateInvalid", "OMX_StateLoaded", "OMX_StateIdle",
	"OMX_StateExecuting", "OMX_StatePause", "OMX_StateWaitForResources",
	"WARNING: Unknown OMX_State"
};

static const char *error_names[] = {
	"OMX_ErrorInsufficientResources", "OMX_ErrorUndefined",
	"OMX_ErrorInvalidComponentName", "OMX_ErrorComponentNotFound",
	"OMX_ErrorInvalidComponent", "OMX_ErrorBadParameter",
	"OMX_ErrorNotImplemented", "OMX_ErrorUnderflow",
	"OMX_ErrorOverflow", "OMX_ErrorHardware", "OMX_ErrorInvalidState",
	"OMX_ErrorStreamCorrupt", "OMX_ErrorPortsNotCompatible",
	"OMX_ErrorResourcesLost", "OMX_ErrorNoMore", "OMX_ErrorVersionMismatch",
	"OMX_ErrorNotReady", "OMX_ErrorTimeout", "OMX_ErrorSameState",
	"OMX_ErrorResourcesPreempted", "OMX_ErrorPortUnresponsiveDuringAllocation",
	"OMX_ErrorPortUnresponsiveDuringDeallocation",
	"OMX_ErrorPortUnresponsiveDuringStop", "OMX_ErrorIncorrectStateTransition",
	"OMX_ErrorIncorrectStateOperation", "OMX_ErrorUnsupportedSetting",
	"OMX_ErrorUnsupportedIndex", "OMX_ErrorBadPortIndex",
	"OMX_ErrorPortUnpopulated", "OMX_ErrorComponentSuspended",
	"OMX_ErrorDynamicResourcesUnavailable", "OMX_ErrorMbErrorsInFrame",
	"OMX_ErrorFormatNotDetected", "OMX_ErrorContentPipeOpenFailed",
	"OMX_ErrorContentPipeCreationFailed", "OMX_ErrorSeperateTablesUsed",
	"OMX_ErrorTunnelingUnsupported",
	"WARNING: Unknown OMX_Error"
};

const char *omx_get_event_name(OMX_EVENTTYPE event)
{
	if ((unsigned int)event > sizeof(event_names)/sizeof(const char*)-1)
		event = (OMX_EVENTTYPE)(sizeof(event_names)/sizeof(const char*)-1);
	return event_names[event];
}

const char *omx_get_command_name(OMX_COMMANDTYPE command)
{
	if ((unsigned int)command > sizeof(command_names)/sizeof(const char*)-1)
		command = (OMX_COMMANDTYPE)(sizeof(command_names)/sizeof(const char*)-1);
	return command_names[command];
}

const char *omx_get_state_name(OMX_STATETYPE state)
{
	if ((unsigned int)state > sizeof(state_names)/sizeof(const char*)-1)
		state = (OMX_STATETYPE)(sizeof(state_names)/sizeof(const char*)-1);
	return state_names[state];
}

const char *omx_get_error_name(OMX_ERRORTYPE error)
{
	if(error == OMX_ErrorNone)
		return "OMX_ErrorNone";

	error -= OMX_ErrorInsufficientResources;

	if ((unsigned int)error > sizeof(error_names)/sizeof(const char*)-1)
		error = (OMX_ERRORTYPE)(sizeof(error_names)/sizeof(const char*)-1);
	return error_names[error];
}

#ifdef DEBUG_MSG
static void dump_omx(void)
{
	int i, j;
	char name[OMX_MAX_STRINGNAME_SIZE];
	OMX_ERRORTYPE err;
	OMX_U32 role_nb;
	OMX_U8 **comp_role_list = NULL;

	OMX_Init();
	serprintf("omx components:\n");
	for (i = 0; ; ++i) {
		err = OMX_ComponentNameEnum(name, OMX_MAX_STRINGNAME_SIZE, i);
		if (err != OMX_ErrorNone)
			break;
		serprintf("%s: ", name);

		err = OMX_GetRolesOfComponent(name, &role_nb, NULL);
		if (err != OMX_ErrorNone || !role_nb)
			continue;

		comp_role_list = realloc(comp_role_list, role_nb * (sizeof(OMX_U8*) + OMX_MAX_STRINGNAME_SIZE));
		if(!comp_role_list)
			break;
		for (j = 0; j < role_nb; j++)
			comp_role_list[j] = ((OMX_U8 *)(&comp_role_list[role_nb])) + j * OMX_MAX_STRINGNAME_SIZE;

		err = OMX_GetRolesOfComponent(name, &role_nb, comp_role_list);
		if (err != OMX_ErrorNone || !role_nb)
			continue;
		for (j = 0; j < role_nb; j++)
			serprintf("%s ", comp_role_list[j]);
		serprintf("\n");
	}
	if (comp_role_list)
		free(comp_role_list);
}

DECLARE_DEBUG_COMMAND_VOID("dumpomx", dump_omx);
#endif

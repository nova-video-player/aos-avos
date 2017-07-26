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

#include "OMX_Core.h"
#include "OMX_Index.h"
#include "OMX_Component.h"
#include "OMX_Video.h"

extern OMX_ERRORTYPE (*fp_OMX_Init) (void);
extern OMX_ERRORTYPE (*fp_OMX_Deinit) (void);
extern OMX_ERRORTYPE (*fp_OMX_GetHandle) (OMX_HANDLETYPE *, OMX_STRING, OMX_PTR, OMX_CALLBACKTYPE *);
extern OMX_ERRORTYPE (*fp_OMX_FreeHandle) (OMX_HANDLETYPE);
extern OMX_ERRORTYPE (*fp_OMX_ComponentNameEnum)(OMX_STRING, OMX_U32, OMX_U32);
extern OMX_ERRORTYPE (*fp_OMX_GetRolesOfComponent)(OMX_STRING, OMX_U32 *, OMX_U8 **);
extern OMX_ERRORTYPE (*fp_OMXAndroid_EnableGraphicBuffers)(OMX_HANDLETYPE component, OMX_U32 port_index, OMX_BOOL enable);
extern OMX_ERRORTYPE (*fp_OMXAndroid_GetGraphicBufferUsage)(OMX_HANDLETYPE component, OMX_U32 port_index, OMX_U32* usage);

#define OMX_Init fp_OMX_Init
#define OMX_Deinit fp_OMX_Deinit
#define OMX_GetHandle fp_OMX_GetHandle
#define OMX_FreeHandle fp_OMX_FreeHandle
#define OMX_ComponentNameEnum fp_OMX_ComponentNameEnum
#define OMX_GetRolesOfComponent fp_OMX_GetRolesOfComponent
#define OMXAndroid_EnableGraphicBuffers fp_OMXAndroid_EnableGraphicBuffers
#define OMXAndroid_GetGraphicBufferUsage fp_OMXAndroid_GetGraphicBufferUsage

#define OMX_VERSION_MAJOR 1
#define OMX_VERSION_MINOR 0
#define OMX_VERSION_REV   0
#define OMX_VERSION_STEP  0

#define OMX_INIT_SIZE(a) (a).nSize = sizeof(a)

#define OMX_INIT_COMMON(a) \
	OMX_INIT_SIZE(a); \
	(a).nVersion.s.nVersionMajor = OMX_VERSION_MAJOR; \
	(a).nVersion.s.nVersionMinor = OMX_VERSION_MINOR; \
	(a).nVersion.s.nRevision = OMX_VERSION_REV; \
	(a).nVersion.s.nStep = OMX_VERSION_STEP

#define OMX_INIT_STRUCTURE(a) \
	memset(&(a), 0, sizeof(a)); \
	OMX_INIT_COMMON(a)

typedef struct {
	OMX_U32 nSize;
	OMX_VERSIONTYPE nVersion;
	OMX_U32 nPortIndex;
	OMX_BOOL bEnable;
} EnableAndroidNativeBuffersParams;

typedef struct {
	OMX_U32 nSize;              // IN
	OMX_VERSIONTYPE nVersion;   // IN
	OMX_U32 nPortIndex;         // IN
	OMX_U32 nUsage;             // OUT
} GetAndroidNativeBufferUsageParams;

/*
 * OMAP specific
 */

#define OMX_TI_IndexConfigStreamInterlaceFormats 0x7F000100
typedef enum OMX_TI_INTERLACETYPE {
	OMX_InterlaceFrameProgressive= 0x00,
	OMX_InterlaceInterleaveFrameTopFieldFirst= 0x01,
	OMX_InterlaceInterleaveFrameBottomFieldFirst= 0x02,
	OMX_InterlaceFrameTopFieldFirst= 0x04,
	OMX_InterlaceFrameBottomFieldFirst= 0x08,
	OMX_InterlaceInterleaveFieldTop= 0x10,
	OMX_InterlaceInterleaveFieldBottom= 0x20,
	OMX_InterlaceFmtMask= 0x7FFFFFFF
} OMX_TI_INTERLACETYPE;

typedef struct OMX_STREAMINTERLACEFORMATTYPE {
	OMX_U32 nSize;
	OMX_VERSIONTYPE nVersion;
	OMX_U32 nPortIndex;
	OMX_BOOL bInterlaceFormat;
	OMX_U32 nInterlaceFormats;
} OMX_STREAMINTERLACEFORMAT;
/*
 * End of OMAP specific
 */

void omx_get_format_sizes(OMX_COLOR_FORMATTYPE omx_format, int width, int height, OMX_U32 *size, OMX_U32 *stride);
OMX_COLOR_FORMATTYPE omx_get_colorspace(OMX_COLOR_FORMATTYPE omx_format);

const char *omx_get_format_name(OMX_COLOR_FORMATTYPE omx_format);

int omx_get_codec(int format, int is_video);

/*
 * get a list of components that can decode format
 * return an array of char *, NULL terminated 
 */
char **omx_get_components(int format, int is_video, const char **prole);

const char *omx_get_event_name(OMX_EVENTTYPE event);
const char *omx_get_command_name(OMX_COMMANDTYPE command);
const char *omx_get_state_name(OMX_STATETYPE state);
const char *omx_get_error_name(OMX_ERRORTYPE error);

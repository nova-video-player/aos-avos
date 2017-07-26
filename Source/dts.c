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

#ifndef STANDALONE

#include "global.h"
#include "util.h"
#include "av.h"
#include "debug.h"
#include "dts.h"
#include "bits.h"
#include "get.h"

#include <string.h>
#include <errno.h>
#include <stdint.h>

#endif

#ifdef CONFIG_DTS

#define AV_PROFILE_DTS         20
#define AV_PROFILE_DTS_ES      30
#define AV_PROFILE_DTS_96_24   40
#define AV_PROFILE_DTS_HD_HRA  50
#define AV_PROFILE_DTS_HD_MA   60
#define AV_PROFILE_DTS_EXPRESS 70

int DTS_get_format_from_profile(int profile)
{
	switch(profile) {
	case AV_PROFILE_DTS:	
		return WAVE_FORMAT_DTS;
	case AV_PROFILE_DTS_HD_MA:	
		return WAVE_FORMAT_DTS_HD;
	case AV_PROFILE_DTS_ES:
	case AV_PROFILE_DTS_96_24:
	case AV_PROFILE_DTS_HD_HRA:
	case AV_PROFILE_DTS_EXPRESS:
		// default?			
		return WAVE_FORMAT_DTS;
	}
	return WAVE_FORMAT_DTS;
}

#endif

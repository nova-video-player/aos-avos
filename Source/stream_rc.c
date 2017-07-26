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

#include "global.h"
#include "debug.h"
#include "object.h"
#include "resource_manager.h"
#include "astdlib.h"
#include "stream.h"
#include "rc_clocks.h"

#include <stdlib.h>

static int force_clock = 0;
static int force_max   = 0;

DECLARE_CLASS(Decoder);

struct Decoder_str {
	// protected: these entries are _only_ to be used by implementations of Object!
	Object s_object;
};

Class Decoder_class = {
	&Object_class, 
	"Decoder"
};

DECLARE_CLASS(AudioDecoder);

struct AudioDecoder_str {
	// protected: these entries are _only_ to be used by implementations of DecoderObject!
	Decoder s_decoder;
};

Class AudioDecoder_class = {
	&Decoder_class, 
	"AudioDecoder"
};

DECLARE_CLASS(VideoDecoder);

struct VideoDecoder_str {
	// protected: these entries are _only_ to be used by implementations of DecoderObject!
	Decoder s_decoder;
};

Class VideoDecoder_class = {
	&Decoder_class, 
	"VideoDecoder"
};

static AudioDecoder *audioDecObj;
static VideoDecoder *videoDecObj;

static int stream_dec_rc_request(Decoder **decObj, STREAM_RC *rc)
{
	if (!*decObj)
		return 1;
		
	int min_clock = force_clock ? force_clock * 1000000 : rc->min_clock;
	int max_clock = force_max   ? force_max   * 1000000 : 
			(rc->min_clock > RC_SYSCLK_VIDDEC_MAX ? rc->min_clock : RC_SYSCLK_VIDDEC_MAX);
	
	if( max_clock == 1000000000 ) {
		resource_systemclock_force_speed( max_clock );
	}
serprintf("rcrq: min %d  max %d\n", min_clock, max_clock );
	if( !RESOURCE_CLOCK_REQUEST_MAX(*decObj, min_clock, max_clock)) {
		// Can't get the resource.
serprintf("stream_dec_rc_request FAILED\n");
		resource_releaseAll( (Object*) *decObj );
		afree(*decObj);
		*decObj = NULL;
		return 1;
	}	

	return 0;
}

static int stream_dec_rc_change_constraints(Decoder *decObj, STREAM_RC *rc)
{
	int min_clock = force_clock ? force_clock * 1000000 : rc->min_clock;
	int max_clock = force_max   ? force_max   * 1000000 : 
			(rc->min_clock > RC_SYSCLK_VIDDEC_MAX ? rc->min_clock : RC_SYSCLK_VIDDEC_MAX);
	
serprintf("rccc: min %d  max %d\n", min_clock, max_clock );
	if (!RESOURCE_CLOCK_CHANGE_CONSTRAINTS_MAX(decObj, min_clock, max_clock)) {
serprintf("stream_dec_rc_change_constraints FAILED\n");
		return 1;
	}	
	return 0;
}

int stream_audio_dec_rc_request(STREAM_RC *rc)
{
	audioDecObj = OBJ_CREATE(AudioDecoder);
	if (!audioDecObj)
		return 1;	

	/* magic default value */
	if (rc->min_clock == 0)
		rc->min_clock = RC_SYSCLK_AUDDEC_DEFAULT;
		
	return stream_dec_rc_request((Decoder**)&audioDecObj, rc);
}

int stream_audio_dec_rc_change_constraints(STREAM_RC *rc)
{
	if (!audioDecObj)
		return 1;

	/* magic default value */
	if (rc->min_clock == 0)
		rc->min_clock = RC_SYSCLK_AUDDEC_DEFAULT;

	return stream_dec_rc_change_constraints((Decoder*)audioDecObj, rc);
}	

void stream_audio_dec_rc_release(STREAM_RC *rc)
{
//serprintf("%s, audioDecObj == %p\n", __FUNCTION__, audioDecObj);
	if (!audioDecObj)
	{
serprintf("%s, audioDecObj already detroyed\n", __FUNCTION__);
		return;
	}

	resource_releaseAll( (Object*) audioDecObj );
	afree(audioDecObj);
	audioDecObj = NULL;
}

int stream_video_dec_rc_request(STREAM_RC *rc)
{
	videoDecObj = OBJ_CREATE(VideoDecoder);
	if (!videoDecObj)
		return 1;	

	/* magic default value */
	if (rc->min_clock == 0)
		rc->min_clock = RC_SYSCLK_VIDDEC_DEFAULT;

	return stream_dec_rc_request((Decoder**)&videoDecObj, rc);
}

int stream_video_dec_rc_change_constraints(STREAM_RC *rc)
{
	if (!videoDecObj)
		return 1;

	/* magic default value */
	if (rc->min_clock == 0)
		rc->min_clock = RC_SYSCLK_VIDDEC_DEFAULT;

	return stream_dec_rc_change_constraints((Decoder*)videoDecObj, rc);
}	

void stream_video_dec_rc_release(STREAM_RC *rc)
{
//serprintf("%s, videoDecObj == %p\n", __FUNCTION__, videoDecObj);
	if (!videoDecObj)
	{
serprintf("%s, videoDecObj already detroyed\n", __FUNCTION__);
		return;
	}
	resource_systemclock_force_speed( 0 );	
	
	resource_releaseAll( (Object*) videoDecObj );

	afree(videoDecObj);
	videoDecObj = NULL;
}

#ifdef DEBUG_MSG
DECLARE_DEBUG_PARAM( "srfc", force_clock );
DECLARE_DEBUG_PARAM( "srfm", force_max );
#endif

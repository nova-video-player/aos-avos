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
#include "global.h"
#include "debug.h"
#include "astdlib.h"
#include "stream.h"
#include "rc_clocks.h"
#include "power_cpu_api.h"

#include <stdlib.h>

static int force_clock = 0;
//static int force_max   = 0;

static int stream_dec_rc_request(STREAM_RC *rc)
{
	int *fd;

	if (rc->priv == NULL) {
		rc->priv = amalloc(sizeof(int));
		fd = rc->priv;
		*fd = power_cpu_connect();
		if (*fd == -1)
			return 1;
	} else {
		fd = rc->priv;
	}
	int min_clock = force_clock ? force_clock * 1000000 : rc->min_clock;
	if (rc->cpu_type == STREAM_CPU_DSP && power_cpu_use_dsp(*fd, 1))
		return 1;
	return power_cpu_set(*fd, min_clock/1000000);
}

static int stream_dec_rc_change_constraints(STREAM_RC *rc)
{
	int *fd = rc->priv;

	if (fd == NULL || *fd == -1)
		return 1;
	int min_clock = force_clock ? force_clock * 1000000 : rc->min_clock;
	if (rc->cpu_type == STREAM_CPU_DSP && power_cpu_use_dsp(*fd, 1))
		return 1;
	return power_cpu_set(*fd, min_clock/1000000);
}

static void stream_rc_release(STREAM_RC *rc)
{
	int *fd = rc->priv;

	if (fd != NULL) {
		if (*fd != -1) {
			power_cpu_release(*fd);
			power_cpu_disconnect(*fd);
		}
		afree(rc->priv);
	}
}

int stream_audio_dec_rc_request(STREAM_RC *rc)
{
#if 0
	/* magic default value */
	if (rc->min_clock == 0)
		rc->min_clock = RC_SYSCLK_AUDDEC_DEFAULT;
	return stream_dec_rc_request(rc);
#endif
	return 0;
}

int stream_audio_dec_rc_change_constraints(STREAM_RC *rc)
{
#if 0
	/* magic default value */
	if (rc->min_clock == 0)
		rc->min_clock = RC_SYSCLK_AUDDEC_DEFAULT;
	return stream_dec_rc_change_constraints(rc);
#endif
	return 0;
}	

void stream_audio_dec_rc_release(STREAM_RC *rc)
{
#if 0
	stream_rc_release(rc);
#endif
}

int stream_video_dec_rc_request(STREAM_RC *rc)
{
	/* magic default value */
	if (rc->min_clock == 0)
		rc->min_clock = RC_SYSCLK_VIDDEC_DEFAULT;
	return stream_dec_rc_request(rc);
}

int stream_video_dec_rc_change_constraints(STREAM_RC *rc)
{
	/* magic default value */
	if (rc->min_clock == 0)
		rc->min_clock = RC_SYSCLK_VIDDEC_DEFAULT;
	return stream_dec_rc_change_constraints(rc);
}	

void stream_video_dec_rc_release(STREAM_RC *rc)
{
	stream_rc_release(rc);
}

#ifdef DEBUG_MSG
DECLARE_DEBUG_PARAM( "srfc", force_clock );
//DECLARE_DEBUG_PARAM( "srfm", force_max );
#endif

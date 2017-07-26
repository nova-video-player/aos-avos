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

#ifndef _STREAM_RC_H
#define _STREAM_RC_H

typedef struct STREAM_RC {
	int		min_clock;
	int		mem_type;
	int		mem_size;
	int		output_cached;
	int		num_frames;
	int             cpu_type;
	int 		hw_usage;
	int 		min_available_buffer;
	void		*priv;
} STREAM_RC;

int  stream_audio_dec_rc_request(STREAM_RC *rc);
int  stream_audio_dec_rc_change_constraints(STREAM_RC *rc);
void stream_audio_dec_rc_release(STREAM_RC *rc);

int  stream_video_dec_rc_request(STREAM_RC *rc);
int  stream_video_dec_rc_change_constraints(STREAM_RC *rc);
void stream_video_dec_rc_release(STREAM_RC *rc);

#endif /* _STREAM_RC_H */

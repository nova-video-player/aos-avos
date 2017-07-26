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

#ifndef MEDIA_PLAYER_H
#define MEDIA_PLAYER_H

#include "athread.h"
#include "audio.h"
#include "media_server.h"
#include "stream.h"

#include <stdint.h>

struct media_player_video_sub {
	int listen_fd;
	int fd;
	int send;
	int send_offset;
	pthread_t thread_handle;
};

struct media_player_video {
	STREAM *stream;
	int last_seekable;
	int last_pauseable;
	int last_duration;
	int buffered_pos;
	int started;
	int surface_timer;
	float zoom;
	float ratio;
	STREAM_SCREEN_PARAMS display;
	struct media_player_video_sub sub;
};

struct media_player_audio_track {
	int pos;
	int duration;
	int etype;
	STREAM_URL src;
}; 

struct media_player_audio {
	AUDIO a;
	int opened;
	int was_paused;
	int last_seekable;
	int is_idle;
	int is_locked;
	int idle_timer;
	struct media_player_audio_track current_track;
	struct media_player_audio_track next_track;
};

struct media_player_client {
	STREAM_URL		src; // url after redirection.
	int			type;
	int			etype;
	int			vol_l;
	int			vol_r;
	uint32_t		loop;
	union {
		struct media_player_video *video;
		struct media_player_audio *audio;
	} priv;
};
#endif

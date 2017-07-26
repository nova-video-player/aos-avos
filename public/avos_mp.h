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

#ifndef AVOS_MP_H
#define AVOS_MP_H

#include <stdint.h>
#include "avos_common.h"

#if __cplusplus
extern "C" {
#endif

typedef struct avos_mp avos_mp_t;

typedef void (*avos_mp_event_cb_t) (avos_mp_t *mp, int what, int ext1, int ext2, avos_msg_t *msg /* need to be freed if not NULL */);

typedef struct avos_mp_handle_t {
	avos_mp_t *(*create)	(avos_mp_event_cb_t event_cb);
	int (*destroy)		(avos_mp_t *mp);

	void (*setpriv)		(avos_mp_t *mp, void *priv);
	void *(*getpriv)	(avos_mp_t *mp);
	int (*setdatasource)	(avos_mp_t *mp, const char *path, const char **keys, const char **values);
	int (*setdatasource_fd)	(avos_mp_t *mp, int fd, int64_t offset, int64_t length);
	int (*setsurface)	(avos_mp_t *mp, void *handle);
	int (*getmetadata)	(avos_mp_t *mp, metadata_buffer_t **buffer);
	int (*open)		(avos_mp_t *mp);
	int (*open_async)	(avos_mp_t *mp);
	int (*close)		(avos_mp_t *mp);
	int (*start)		(avos_mp_t *mp);
	int (*pause)		(avos_mp_t *mp);
	int (*isplaying)	(avos_mp_t *mp, int *ret);
	int (*seek)		(avos_mp_t *mp, uint32_t msec);
	int (*seek_async)	(avos_mp_t *mp, uint32_t msec);
	int (*setstarttime)	(avos_mp_t *mp, uint32_t msec);
	int (*getpos)		(avos_mp_t *mp, uint32_t *ret);
	int (*getduration)	(avos_mp_t *mp, uint32_t *ret);
	int (*setlooping)	(avos_mp_t *mp, int looping);
	int (*islooping)	(avos_mp_t *mp, int *ret);
	int (*getaudiosessionid)(avos_mp_t *mp, int *ret);
	// video specific
	int (*setaudiotrack)	(avos_mp_t *mp, int track, int *ret);
	int (*checksubtitles)	(avos_mp_t *mp);
	int (*setsubtitletrack)	(avos_mp_t *mp, int track, int *ret);
	int (*setsubtitledelay)	(avos_mp_t *mp, int delay);
	int (*setsubtitleratio)	(avos_mp_t *mp, uint32_t n, uint32_t d);
	int (*setaudiofilter)	(avos_mp_t *mp, int n, int night_on);
	int (*setavdelay)	(avos_mp_t *mp, int delay);
	// audio specific
	int (*setnextrack)	(avos_mp_t *mp, const char *path);
} avos_mp_handle_t;

const avos_mp_handle_t *avos_mp_get_handle();

enum media_event_type {
	MEDIA_NOP			= 0, // interface test message
	MEDIA_PREPARED			= 1,
	MEDIA_PLAYBACK_COMPLETE		= 2,
	MEDIA_BUFFERING_UPDATE		= 3,
	MEDIA_SEEK_COMPLETE		= 4,
	MEDIA_SET_VIDEO_SIZE		= 5,
	MEDIA_RELATIVE_POSITION_UPDATE	= 6,
	MEDIA_NEXT_TRACK		= 7,
	MEDIA_SET_VIDEO_ASPECT		= 8,
	MEDIA_TIMED_TEXT		= 99,
	MEDIA_ERROR			= 100,
	MEDIA_INFO			= 200,
	MEDIA_SUBTITLE			= 1000
};

enum media_error_type {
	// 0xx
	MEDIA_ERROR_UNKNOWN = 1,
	// 1xx
	MEDIA_ERROR_SERVER_DIED = 100,
	// 2xx
	MEDIA_ERROR_NOT_VALID_FOR_PROGRESSIVE_PLAYBACK = 200,
	// 3xx
	MEDIA_ERROR_VE_NO_ERROR = 1000,
	MEDIA_ERROR_VE_FILE_ERROR = 1001,
	MEDIA_ERROR_VE_ERROR = 1002,
	MEDIA_ERROR_VE_CONNECTION_ERROR = 1003,
	MEDIA_ERROR_VE_USER_ABORT = 1004,
	MEDIA_ERROR_VE_VIDEO_NOT_SUPPORTED = 1005,
	MEDIA_ERROR_VE_AUDIO_NOT_SUPPORTED = 1006,
	MEDIA_ERROR_VE_VIDEO_NOT_ALLOWED = 1007,
	MEDIA_ERROR_VE_AUDIO_NOT_ALLOWED = 1008,
	MEDIA_ERROR_VE_TOO_BIG_FOR_STREAM = 1009,
	MEDIA_ERROR_VE_TOO_BIG_FOR_CODEC = 1010,
	MEDIA_ERROR_VE_NOT_INTERLEAVED = 1011,
	MEDIA_ERROR_VE_CRYPTED = 1012,
	MEDIA_ERROR_VE_MAX = 1012,
};

// ext2
enum media_error_qual_type {
	MEDIA_ERROR_VEQ_NONE = 1000,
	MEDIA_ERROR_VEQ_MPG4_UNSUPPORTED = 1001,
	MEDIA_ERROR_VEQ_PROFILE_AND_LEVEL_UNSUPPORTED = 1002,
	MEDIA_ERROR_VEQ_AUDIO_PROFILE_AND_LEVEL_UNSUPPORTED = 1003,
	MEDIA_ERROR_VEQ_INTERLACED_NOT_SUPPORTED = 1004,
	MEDIA_ERROR_VEQ_SEE_DESCRIPTION = 1005,
	MEDIA_ERROR_VEQ_MAX = 1005,
};

enum media_info_type {
	// 0xx
	MEDIA_INFO_UNKNOWN = 1,
	// 7xx
	// The video is too complex for the decoder: it can't decode frames fast
	// enough. Possibly only the audio plays fine at this stage.
	MEDIA_INFO_VIDEO_TRACK_LAGGING = 700,
	// MediaPlayer is temporarily pausing playback internally in order to
	// buffer more data.
	MEDIA_INFO_BUFFERING_START = 701,
	// MediaPlayer is resuming playback after filling buffers.
	MEDIA_INFO_BUFFERING_END = 702,
	// Bandwidth in recent past
	MEDIA_INFO_NETWORK_BANDWIDTH = 703,

	// 8xx
	// Bad interleaving means that a media has been improperly interleaved or not
	// interleaved at all, e.g has all the video samples first then all the audio
	// ones. Video is playing but a lot of disk seek may be happening.
	MEDIA_INFO_BAD_INTERLEAVING = 800,
	// The media is not seekable (e.g live stream).
	MEDIA_INFO_NOT_SEEKABLE = 801,
	// New media metadata is available.
	MEDIA_INFO_METADATA_UPDATE = 802,
};

#if __cplusplus
}
#endif

#endif // AVOS_MP_H

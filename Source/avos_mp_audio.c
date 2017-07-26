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

#include "athread.h"
#include "audio.h"
#include "avos_mp.h"
#include "avos_mp_priv.h"
#include "debug.h"
#include "global.h"
#include "file_type.h"
#include "file_info.h"
#include "stream.h"

#define DBG if(Debug[DBG_AUDIO_PLAYER])
#define DBG2 if(Debug[DBG_AUDIO_PLAYER] > 1)

#define MPLOG(fmt, ...) serprintf("%p|%s: " fmt "\n", mp, __FUNCTION__, ##__VA_ARGS__)

typedef struct avos_mp_audio_track {
	STREAM_URL src;
	int etype;
} avos_mp_audio_track_t; 

struct avos_mp_audio {
	AUDIO a;
	int abort;
	int opened;
	int was_paused;
	int last_seekable;
	int last_pauseable;
	int last_duration;
	avos_mp_audio_track_t current_track;
	avos_mp_audio_track_t next_track;
	pthread_mutex_t mtx;
	pthread_cond_t cond;
};

extern int audio_main_buffer;

static void audio_track_init(avos_mp_audio_track_t *track, STREAM_URL *url, int etype)
{
	if (url) {
		track->etype = etype;
		memcpy(&track->src, url, sizeof(STREAM_URL));
	} else {
		track->etype = ETYPE_NONE;
	}
}

static void audio_track_move(avos_mp_audio_track_t *dest, avos_mp_audio_track_t *src)
{
	memcpy(dest, src, sizeof(avos_mp_audio_track_t));
	src->etype = ETYPE_NONE;
}

static void send_audio_track_info(avos_mp_t *mp, avos_mp_audio_t *audio)
{
	int changed;
	int duration;
	ID3_TAG *tag;
	AV_PROPERTIES av;

	audio->last_pauseable = 1;
	audio->last_seekable = audio_seekable(&audio->a);
	audio_get_current_time(&audio->a, &audio->last_duration);

	memcpy(&av.audio[0], audio->a.audio, sizeof(AUDIO_PROPERTIES));
	av.as_max = 1;
	tag = audio_get_current_tag(&audio->a);	
	changed = avos_mp_fillmetadata(mp,
	    TYPE_AUD,
	    audio->a.size,
	    tag,
	    &av,
	    NULL,
	    audio->last_duration,
	    audio->last_seekable,
	    audio->last_pauseable,
	    0);
	if (changed) {
		MPLOG("changed");
		avos_mp_sendevent(mp, MEDIA_INFO, MEDIA_INFO_METADATA_UPDATE, 0);
	}
	if (!audio->last_seekable)
		avos_mp_sendevent(mp, MEDIA_INFO, MEDIA_INFO_NOT_SEEKABLE, 0);
}

static void audio_cb_track_changed(avos_mp_t *mp, avos_mp_audio_t *audio)
{
	STREAM_URL url;

	audio_get_current_url(&audio->a, &url);
	pthread_mutex_lock(&audio->mtx);
	if (!audio->opened && strcmpNC(url.url, audio->current_track.src.url) == 0) {
		if (!audio_get_next_etype(&audio->a) && audio->next_track.etype) {
			audio_set_next_track(&audio->a, &audio->next_track.src, 0, audio->next_track.etype, NULL);
		}

		send_audio_track_info(mp, audio);

		audio->opened = 1;
		pthread_cond_signal(&audio->cond);
	} else if (strcmpNC(url.url, audio->next_track.src.url) == 0) {
		audio_track_move(&audio->current_track, &audio->next_track);
		send_audio_track_info(mp, audio);
		avos_mp_sendevent_msg(mp, MEDIA_NEXT_TRACK, 0, 0, audio->next_track.src.url);
	}
	pthread_mutex_unlock(&audio->mtx);
}

int avos_mp_audio_abort(avos_mp_t *mp, avos_mp_audio_t *audio);

static void audio_cb(AUDIO *a, AUDIO_MESSAGE message)
{
/* no more audio support 
	avos_mp_t *mp = (avos_mp_t *)audio_get_user_ctx(a);
	avos_mp_audio_t *audio = avos_mp_getaudio(mp);

	switch( message) {
		case AM_TRACK_ERROR:
			MPLOG("AM_TRACK_ERROR");

			avos_mp_sendevent(mp, MEDIA_ERROR, 0, 0);
			avos_mp_audio_abort(mp, audio);
			break;
		case AM_TRACK_CHANGED:
			MPLOG("AM_TRACK_CHANGED");

			audio_cb_track_changed(mp, audio);
			MPLOG("AM_TRACK_CHANGED: done");
			break;
		case AM_NO_VALID_TRACK:
			MPLOG("AM_NO_VALID_TRACK");

			pthread_mutex_lock(&audio->mtx);
			audio->opened = 0;
			pthread_mutex_unlock(&audio->mtx);
			avos_mp_sendevent(mp, MEDIA_PLAYBACK_COMPLETE, 0, 0);
			break;
		case AM_DISCARD_TRACK:
			MPLOG("AM_DISCARD_TRACK");
			break;
		case AM_RECORDING_ERROR:
			MPLOG("AM_RECORDING_ERROR");
			break;
		case AM_RECORDING_DISK_FULL:
			MPLOG("AM_RECORDING_DISK_FULL");
			break;
		case AM_RECORDING_MAXSIZE:
			MPLOG("AM_RECORDING_MAXSIZE");
			break;
		default:
			break;
	}
*/
}

static int audio_abort_handler(void *ctx)
{
/* no more audio support 
	avos_mp_t *mp = (avos_mp_t *)audio_get_user_ctx((AUDIO *) ctx);
	avos_mp_audio_t *audio = avos_mp_getaudio(mp);
	return audio->abort;
*/
	return 0;
}

avos_mp_audio_t *avos_mp_audio_create()
{
/* no more audio support 
	avos_mp_audio_t *audio = acalloc(1, sizeof(avos_mp_audio_t));
	if (audio) {
		pthread_mutex_init(&audio->mtx, NULL);
		pthread_cond_init(&audio->cond, NULL);
	}
	return audio;
*/
	return NULL;
}

int avos_mp_audio_destroy(avos_mp_audio_t **paudio)
{
/* no more audio support 
	avos_mp_audio_t *audio = *paudio;

	pthread_mutex_destroy(&audio->mtx);
	pthread_cond_destroy(&audio->cond);

	afree(audio);
	*paudio = NULL;
*/ 
	return AVOS_ERR_OK;
}

int avos_mp_audio_open_track(avos_mp_t *mp, avos_mp_audio_t *audio, avos_mp_audio_track_t *track)
{
/* no more audio support 
	int ret = AVOS_ERR;

	pthread_mutex_lock(&audio->mtx);
	if (audio->opened) {
		ret = AVOS_ERR_OK;
		goto end;
	}
	MPLOG("%s", track->src.url);
	if (audio_set_next_track(&audio->a, &track->src, 0, track->etype, NULL) != 0)
		goto end;
	if (audio_play(&audio->a, 0, 1))
		goto end;

	while (!audio->opened && !audio->abort)
		pthread_cond_wait(&audio->cond, &audio->mtx);
	if (audio->opened)
		ret = AVOS_ERR_OK;
end:
	pthread_mutex_unlock(&audio->mtx);
	return ret;
*/
	return AVOS_ERR;
}

int avos_mp_audio_open(avos_mp_t *mp, avos_mp_audio_t *audio, STREAM_URL *src, int etype)
{
/* no more audio support 
	if (audio_open(&audio->a, audio_main_buffer, audio_cb, NULL) != 0)
		return AVOS_ERR;

	audio_set_user_ctx(&audio->a, mp);
	audio_set_abort_handler(&audio->a, audio_abort_handler);

	audio_track_init(&audio->current_track, src, etype);
	return avos_mp_audio_open_track(mp, audio, &audio->current_track);
*/
	return AVOS_ERR;
}

int avos_mp_audio_close(avos_mp_t *mp, avos_mp_audio_t *audio)
{
/* no more audio support 
	MPLOG();

	audio_stop(&audio->a);
	audio_close(&audio->a);
*/
	return AVOS_ERR_OK;
}

int avos_mp_audio_abort(avos_mp_t *mp, avos_mp_audio_t *audio)
{
/* no more audio support 
	pthread_mutex_lock(&audio->mtx);
	audio->abort = 1;
	pthread_cond_signal(&audio->cond);
	pthread_mutex_unlock(&audio->mtx);
*/
	return AVOS_ERR_OK;
}

int avos_mp_audio_start(avos_mp_t *mp, avos_mp_audio_t *audio)
{
/* no more audio support 
	if (avos_mp_audio_open_track(mp, audio, &audio->current_track) != AVOS_ERR_OK)
		return AVOS_ERR;
	audio_un_pause(&audio->a, audio->was_paused);
	return AVOS_ERR_OK;
*/
	return AVOS_ERR;
}

int avos_mp_audio_pause(avos_mp_t *mp, avos_mp_audio_t *audio)
{
/* no more audio support 
	if(!audio_is_paused(&audio->a))
		audio->was_paused = audio_pause(&audio->a);
	return AVOS_ERR_OK;
*/
	return AVOS_ERR;
}

int avos_mp_audio_isplaying(avos_mp_t *mp, avos_mp_audio_t *audio, int *ret)
{
/* no more audio support 
	*ret = !audio_is_paused(&audio->a);
*/
	*ret = 0;
	return AVOS_ERR_OK;
}

int avos_mp_audio_seek(avos_mp_t *mp, avos_mp_audio_t *audio, uint32_t msec)
{
/* no more audio support 
	if (audio->last_seekable) {
		// check if audio is still opened when seeking to 0
		if (msec == 0 && avos_mp_audio_open_track(mp, audio, &audio->current_track) != AVOS_ERR_OK)
			return AVOS_ERR;
		audio_set_pos(&audio->a, msec);
	}
	return AVOS_ERR_OK;
*/
	return AVOS_ERR;
}

int avos_mp_audio_getpos(avos_mp_t *mp, avos_mp_audio_t *audio, uint32_t *ret)
{
/* no more audio support 
	int total;

	*ret = audio_get_current_time(&audio->a, &total); 
	return AVOS_ERR_OK;
*/
	*ret = 0;
	return AVOS_ERR;
}

int avos_mp_audio_getduration(avos_mp_t *mp, avos_mp_audio_t *audio, uint32_t *ret)
{
/* no more audio support 
	*ret = audio->last_duration;
	return AVOS_ERR_OK;
*/
	*ret = 0;
	return AVOS_ERR;
}

int avos_mp_audio_getaudiosessionid(avos_mp_t *mp, avos_mp_audio_t *audio, uint32_t *ret)
{
	return AVOS_ERR_OK;
}

int avos_mp_audio_setnextrack(avos_mp_t *mp, avos_mp_audio_t *audio, const char *path)
{
/* no more audio support 
	if (!path || strcasecmp(path, "null") == 0) {
		audio_set_next_track(&audio->a, NULL, 0, 0, NULL);
		audio_track_init(&audio->next_track, NULL, ETYPE_NONE);
	} else {
		STREAM_URL src;
		int type, etype;

		stream_url_cpy_url(&src, path);
		get_url_type(&src, &type, &etype);
		if (audio_set_next_track(&audio->a, &src, 0, etype, NULL) == 0) {
			audio_track_init(&audio->next_track, &src, etype);
		}
	}
	return AVOS_ERR_OK;
*/
	return AVOS_ERR;
}

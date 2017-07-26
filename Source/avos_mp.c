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

#include <stdio.h>
#include <inttypes.h>

#include "avos_common.h"
#include "avos_common_priv.h"
#include "avos_mp.h"
#include "avos_mp_metadata.h"
#include "avos_mp_priv.h"
#include "astdlib.h"
#include "debug.h"
#include "global.h"
#include "file_type.h"
#include "athread.h"
#include "stream_config.h"
#include "device_config.h"

#define UPNP_FUSE_TO_HTTP

#define DBG if(Debug[DBG_VIDEO_PLAYER] || Debug[DBG_AUDIO_PLAYER])
#define DBG2 if((Debug[DBG_VIDEO_PLAYER] > 1) || (Debug[DBG_AUDIO_PLAYER] > 1))

#define MPLOG(fmt, ...) serprintf("%p|%s: " fmt "\n", mp, __FUNCTION__, ##__VA_ARGS__)
#define MPLOGV DBG MPLOG

enum {
	ASYNC_CMD_WAIT = 0,
	ASYNC_CMD_OPEN,
	ASYNC_CMD_SEEK,
	ASYNC_CMD_EXIT,
};

typedef struct async_cmd {
	int id;
	int arg;
} async_cmd_t;

struct avos_mp {
	avos_mp_event_cb_t event_cb;

	int fd;

	void *surface_handle;
	void *priv;

	STREAM_URL src;
	int type;
	int etype;
	int islooping;
	int starttime;

	struct {
		pthread_t thread;
		pthread_mutex_t mtx;
		pthread_cond_t cond;
		async_cmd_t cur_cmd;
		async_cmd_t next_cmd;
	} async;

	struct {
		int isplaying;
		int duration;
		int pos;
	} last;

	metadata_buffer_t *metadata_buffer;
	pthread_mutex_t metadata_mtx;
	void *media;
};

avos_mp_video_t *avos_mp_video_create();
avos_mp_audio_t *avos_mp_audio_create();
int avos_mp_video_destroy(avos_mp_video_t **video);
int avos_mp_audio_destroy(avos_mp_audio_t **audio);

int avos_mp_video_open(avos_mp_t *mp, avos_mp_video_t *video, STREAM_URL *src, int etype, void *surface_handle, int starttime);
int avos_mp_audio_open(avos_mp_t *mp, avos_mp_audio_t *audio, STREAM_URL *src, int etype);
int avos_mp_video_close(avos_mp_t *mp, avos_mp_video_t *video);
int avos_mp_audio_close(avos_mp_t *mp, avos_mp_audio_t *audio);

int avos_mp_video_abort(avos_mp_t *mp, avos_mp_video_t *video);
int avos_mp_video_start(avos_mp_t *mp, avos_mp_video_t *video);
int avos_mp_video_pause(avos_mp_t *mp, avos_mp_video_t *video);
int avos_mp_video_isplaying(avos_mp_t *mp, avos_mp_video_t *video, int *ret);
int avos_mp_video_seek(avos_mp_t *mp, avos_mp_video_t *video, uint32_t msec);
int avos_mp_video_getpos(avos_mp_t *mp, avos_mp_video_t *video, uint32_t *ret);
int avos_mp_video_getduration(avos_mp_t *mp, avos_mp_video_t *video, uint32_t *ret);
int avos_mp_video_getaudiosessionid(avos_mp_t *mp, avos_mp_video_t *video, uint32_t *ret);
int avos_mp_video_setaudiotrack(avos_mp_t *mp, avos_mp_video_t *video, int track, int *ret);
int avos_mp_video_checksubtitles(avos_mp_t *mp, avos_mp_video_t *video);
int avos_mp_video_setsubtitletrack(avos_mp_t *mp, avos_mp_video_t *video, int track, int *ret);
int avos_mp_video_setsubtitledelay(avos_mp_t *mp, avos_mp_video_t *video, int delay);
int avos_mp_video_setsubtitleratio(avos_mp_t *mp, avos_mp_video_t *video, uint32_t n, uint32_t d);
int avos_mp_video_setaudiofilter(avos_mp_t *mp, avos_mp_video_t *video, int n, int night_on);
int avos_mp_video_setavdelay(avos_mp_t *mp, avos_mp_video_t *video, int delay);

int avos_mp_audio_abort(avos_mp_t *mp, avos_mp_audio_t *audio);
int avos_mp_audio_start(avos_mp_t *mp, avos_mp_audio_t *audio);
int avos_mp_audio_pause(avos_mp_t *mp, avos_mp_audio_t *audio);
int avos_mp_audio_isplaying(avos_mp_t *mp, avos_mp_audio_t *audio, int *ret);
int avos_mp_audio_seek(avos_mp_t *mp, avos_mp_audio_t *audio, uint32_t msec);
int avos_mp_audio_getpos(avos_mp_t *mp, avos_mp_audio_t *audio, uint32_t *ret);
int avos_mp_audio_getduration(avos_mp_t *mp, avos_mp_audio_t *audio, uint32_t *ret);
int avos_mp_audio_getaudiosessionid(avos_mp_t *mp, avos_mp_audio_t *audio, uint32_t *ret);
int avos_mp_audio_setnextrack(avos_mp_t *mp, avos_mp_audio_t *audio, const char *url);

// only return in case or error
#define AVOS_MP_COMMON(fn, _mp, ...) \
do { \
	if (!(_mp)->media || ((_mp)->type != TYPE_VID && (_mp)->type != TYPE_AUD)) \
		return AVOS_ERR_CRITICAL; \
	if ((_mp)->type == TYPE_VID) {\
		if (avos_mp_video_##fn((_mp), (avos_mp_video_t *)(_mp)->media, ##__VA_ARGS__) != AVOS_ERR_OK) \
			return AVOS_ERR; \
	} else { \
		if (avos_mp_audio_##fn((_mp), (avos_mp_audio_t *)(_mp)->media, ##__VA_ARGS__) != AVOS_ERR_OK) \
			return AVOS_ERR; \
	} \
} while(0)

#define AVOS_MP_VIDEO(fn, _mp, ...) \
do { \
	if (!(_mp)->media || (_mp)->type != TYPE_VID) \
		return AVOS_ERR_CRITICAL; \
	if (avos_mp_video_##fn((_mp), (avos_mp_video_t *)(_mp)->media, ##__VA_ARGS__) != AVOS_ERR_OK) \
		return AVOS_ERR; \
} while(0)

#define AVOS_MP_AUDIO(fn, _mp, ...) \
do { \
	if (!(_mp)->media || (_mp)->type != TYPE_AUD) \
		return AVOS_ERR_CRITICAL; \
	if (avos_mp_audio_##fn((_mp), (avos_mp_audio_t *)(_mp)->media, ##__VA_ARGS__) != AVOS_ERR_OK) \
		return AVOS_ERR; \
} while(0)

avos_mp_video_t *avos_mp_getvideo(avos_mp_t *mp)
{
	return (avos_mp_video_t *)mp->media;
}

avos_mp_audio_t *avos_mp_getaudio(avos_mp_t *mp)
{
	return (avos_mp_audio_t *)mp->media;
}

int avos_mp_fillmetadata(avos_mp_t *mp, int type, uint64_t size, ID3_TAG *id3_tag, AV_PROPERTIES *av, const char *mimetype, int duration, int seekable, int pauseable, int decoder)
{
#define ADD_STR(_id, _str) do { \
	if (avos_metadata_append_str(buffer, (_id), (_str)) == -1) \
		goto quit; \
} while(0)
#define ADD_INT(_id, _arg) do { \
	if (avos_metadata_append_int(buffer, (_id), (_arg)) == -1) \
		goto quit; \
} while(0)
#define ADD_INT64(_id, _arg) do { \
	if (avos_metadata_append_int64(buffer, (_id), (_arg)) == -1) \
		goto quit; \
} while(0)
#define ADD_BOOL(_id, _arg) do { \
	if (avos_metadata_append_bool(buffer, (_id), (_arg)) == -1) \
		goto quit; \
} while(0)
	const char *token;
	int i, gap_key, changed = 0;
	metadata_buffer_t *buffer;

	MPLOGV();

	buffer = mp->metadata_buffer;
	if (!buffer)
		return -1;

	pthread_mutex_lock(&mp->metadata_mtx);
	avos_metadata_write_begin(buffer);

	if (size > 0)
		ADD_INT64(AVOS_MP_METADATA_FILE_SIZE, size);

	AUDIO_PROPERTIES *audiop = &av->audio[0];
	VIDEO_PROPERTIES *videop = &av->video[0];
	if (id3_tag && id3_tag->valid ) {
		ADD_INT(AVOS_MP_METADATA_CD_TRACK_NUM, id3_tag->track);
		ADD_STR(AVOS_MP_METADATA_ALBUM, id3_tag->album);
		ADD_STR(AVOS_MP_METADATA_ARTIST,id3_tag->artist);
		ADD_STR(AVOS_MP_METADATA_COMPOSER, id3_tag->composer);
		ADD_STR(AVOS_MP_METADATA_AUTHOR, id3_tag->author);
		ADD_STR(AVOS_MP_METADATA_GENRE, id3_tag->genre);
		ADD_STR(AVOS_MP_METADATA_TITLE, id3_tag->title);
		ADD_STR(AVOS_MP_METADATA_DATE, id3_tag->year);
		ADD_BOOL(AVOS_MP_METADATA_DRM_CRIPPLED, 0);
	}

	if (duration > 0)
		ADD_INT(AVOS_MP_METADATA_DURATION, duration);
	ADD_BOOL(AVOS_MP_METADATA_PAUSE_AVAILABLE, pauseable);
	ADD_BOOL(AVOS_MP_METADATA_SEEK_BACKWARD_AVAILABLE, seekable);
	ADD_BOOL(AVOS_MP_METADATA_SEEK_FORWARD_AVAILABLE, seekable);
	ADD_BOOL(AVOS_MP_METADATA_SEEK_AVAILABLE, seekable);
	if (mimetype)
		ADD_STR(AVOS_MP_METADATA_MIME_TYPE, mimetype);
	if (audiop->samplesPerSec > 0)
		ADD_INT(AVOS_MP_METADATA_AUDIO_SAMPLE_RATE, audiop->samplesPerSec);
	if (audiop->bytesPerSec > 0)
		ADD_INT(AVOS_MP_METADATA_AUDIO_BIT_RATE, (audiop->bytesPerSec * 8));
	if (audiop->format > 0)
		ADD_INT(AVOS_MP_METADATA_AUDIO_CODEC, audiop->format);

	if (videop->bytesPerSec > 0)
		ADD_INT(AVOS_MP_METADATA_VIDEO_BIT_RATE, (videop->bytesPerSec * 8));
	if (videop->framesPerSec > 0)
		ADD_INT(AVOS_MP_METADATA_VIDEOFRAME_RATE, videop->framesPerSec);
	if (videop->fourcc > 0)
		ADD_INT(AVOS_MP_METADATA_VIDEO_CODEC, videop->fourcc);
	if (videop->width > 0)
		ADD_INT(AVOS_MP_METADATA_VIDEO_WIDTH, videop->width);
	if (videop->height > 0)
		ADD_INT(AVOS_MP_METADATA_VIDEO_HEIGHT, videop->height);

	if (type == TYPE_VID && av && av->vs_max > 0) {
		ADD_INT(AVOS_MP_METADATA_NB_VIDEO_TRACK, av->vs_max);

		gap_key = AVOS_MP_METADATA_VIDEO_TRACK;
		token = video_get_fourcc_name(videop);
		ADD_STR(gap_key + AVOS_MP_METADATA_VIDEO_TRACK_FORMAT, token);

		if (videop->width > 0 && videop->height > 0) {
			ADD_INT(gap_key + AVOS_MP_METADATA_VIDEO_TRACK_WIDTH, videop->width);
			ADD_INT(gap_key + AVOS_MP_METADATA_VIDEO_TRACK_HEIGHT, videop->height);
			ADD_INT(gap_key + AVOS_MP_METADATA_VIDEO_TRACK_ASPECT_N, videop->aspect_n);
			ADD_INT(gap_key + AVOS_MP_METADATA_VIDEO_TRACK_ASPECT_D, videop->aspect_d);
			ADD_INT(gap_key + AVOS_MP_METADATA_VIDEO_TRACK_PIXEL_FORMAT, videop->colorspace);
		}

		ADD_INT(gap_key + AVOS_MP_METADATA_VIDEO_TRACK_PROFILE, videop->profile);

		ADD_INT(gap_key + AVOS_MP_METADATA_VIDEO_TRACK_LEVEL, videop->level);

		if (videop->bytesPerSec > 0)
			ADD_INT(gap_key + AVOS_MP_METADATA_VIDEO_TRACK_BIT_RATE, videop->bytesPerSec / 125);

		if (videop->framesPerSec > 0)
			ADD_INT(gap_key + AVOS_MP_METADATA_VIDEO_TRACK_FPS, videop->framesPerSec);

		if (videop->rate > 0)
			ADD_INT(gap_key + AVOS_MP_METADATA_VIDEO_TRACK_FPS_RATE, videop->rate);

		if (videop->scale > 0)
			ADD_INT(gap_key + AVOS_MP_METADATA_VIDEO_TRACK_FPS_SCALE, videop->scale);

		ADD_INT(gap_key + AVOS_MP_METADATA_VIDEO_TRACK_S3DMODE, videop->stereo_mode);
		ADD_INT(gap_key + AVOS_MP_METADATA_VIDEO_TRACK_DECODER, decoder);
	}
	if (av && type == TYPE_AUD)
		av->as_max = 1;
	if (av && av->as_max >= 0) {
		ADD_INT(AVOS_MP_METADATA_NB_AUDIO_TRACK, av->as_max);
		for (i = 0; i < av->as_max; ++i) {
			audiop = &av->audio[i];
			gap_key = AVOS_MP_METADATA_AUDIO_TRACK + i * AVOS_MP_METADATA_AUDIO_TRACK_MAX;

			ADD_STR(gap_key + AVOS_MP_METADATA_AUDIO_TRACK_NAME, audiop->name);

			if (audiop->format == WAVE_FORMAT_DTS)
				token = device_has_archos_enhancement() ? "Digital" :  "DTS";
			else
				token = audio_get_format_name(audiop);
			ADD_STR(gap_key + AVOS_MP_METADATA_AUDIO_TRACK_FORMAT, token);

			if (audiop->bytesPerSec > 0)
				ADD_INT(gap_key + AVOS_MP_METADATA_AUDIO_TRACK_BIT_RATE, audiop->bytesPerSec / 125);

			if (audiop->samplesPerSec > 0)
				ADD_INT(gap_key + AVOS_MP_METADATA_AUDIO_TRACK_SAMPLE_RATE, audiop->samplesPerSec);

			token = audio_get_channel_layout_name(audiop->channels);
			ADD_STR(gap_key + AVOS_MP_METADATA_AUDIO_TRACK_CHANNELS, token);

			ADD_BOOL(gap_key + AVOS_MP_METADATA_AUDIO_TRACK_VBR, audiop->vbr);

			int supported = 0;
			STREAM_DEC_AUDIO *dec = stream_get_audio_dec( audiop );
			if( dec ) {
				supported = 1;
				if( dec->is_supported ) {
					supported = dec->is_supported( audiop ) ? 1 : 0;
				}
			}
			ADD_BOOL(gap_key + AVOS_MP_METADATA_AUDIO_TRACK_SUPPORTED, supported);
		}

		int current_audio = -1;
		if (av->as >= 0 && av->as < av->as_max)
			current_audio = av->audio[av->as].valid ? av->as : -1;
		ADD_INT(AVOS_MP_METADATA_CURRENT_AUDIO_TRACK, current_audio);
	}
	if (type == TYPE_VID && av && av->subs_max >= 0) {
		ADD_INT(AVOS_MP_METADATA_NB_SUBTITLE_TRACK, av->subs_max);
		for (i = 0; i < av->subs_max; ++i) {
			gap_key = AVOS_MP_METADATA_SUBTITLE_TRACK + i * AVOS_MP_METADATA_SUBTITLE_TRACK_MAX;
			ADD_STR(gap_key + AVOS_MP_METADATA_SUBTITLE_TRACK_NAME, av->sub[i].name);
			ADD_STR(gap_key + AVOS_MP_METADATA_SUBTITLE_TRACK_PATH, av->sub[i].path);
		}

		int current_subitle = -1;
		if (av->subs >= 0 && av->subs < av->subs_max)
			current_subitle = av->sub[av->subs].valid ? av->subs : -1;
		ADD_INT(AVOS_MP_METADATA_CURRENT_SUBTITLE_TRACK, current_subitle);
	}

quit:
	changed = avos_metadata_write_end(buffer);
	pthread_mutex_unlock(&mp->metadata_mtx);
	return changed;
}

void avos_mp_sendevent_data(avos_mp_t *mp, int what, int arg1, int arg2, avos_msg_t *data)
{
	if (mp->event_cb)
		mp->event_cb(mp, what, arg1, arg2, data);
}

void avos_mp_sendevent_msg(avos_mp_t *mp, int what, int arg1, int arg2, const char *str)
{
	avos_msg_t *msg = NULL;
	if (str)
		msg = avos_msg_new_str(0, str);
	avos_mp_sendevent_data(mp, what, arg1, arg2, msg);
}

void avos_mp_sendevent(avos_mp_t *mp, int what, int arg1, int arg2)
{
	return avos_mp_sendevent_data(mp, what, arg1, arg2, NULL);
}

static int avos_mp_open_common(avos_mp_t *mp)
{
	if (mp->type == TYPE_VID) {
		avos_mp_video_t *video = avos_mp_video_create();
		mp->media = video;
		return avos_mp_video_open(mp, video, &mp->src, mp->etype, mp->surface_handle, mp->starttime);
	} else if (mp->type == TYPE_AUD) {
		avos_mp_audio_t *audio = avos_mp_audio_create();
		mp->media = audio;
		return avos_mp_audio_open(mp, audio, &mp->src, mp->etype);
	}
	return AVOS_ERR;
}

static int avos_mp_seek_common(avos_mp_t *mp, uint32_t msec)
{
	MPLOGV("%d", msec);
	AVOS_MP_COMMON(seek, mp, msec);
	return AVOS_ERR_OK;
}

static void *async_thread(void *ctx)
{
	int loop = 1;
	avos_mp_t *mp = (avos_mp_t *) ctx;

	while (loop) {
		pthread_mutex_lock(&mp->async.mtx);
		if (mp->async.cur_cmd.id == ASYNC_CMD_SEEK) {
			avos_mp_sendevent(mp, MEDIA_SEEK_COMPLETE,
			    mp->async.next_cmd.id == ASYNC_CMD_SEEK ? 1 /* seek pending */ : 0, 0);
		}
		if (mp->async.next_cmd.id == ASYNC_CMD_WAIT) {
			// signal async thread is done for now
			mp->async.cur_cmd.id = ASYNC_CMD_WAIT;
			mp->async.cur_cmd.arg = 0;
			pthread_cond_broadcast(&mp->async.cond);
			// wait for a new cmd
			while (mp->async.next_cmd.id == ASYNC_CMD_WAIT)
				pthread_cond_wait(&mp->async.cond, &mp->async.mtx);
		}
		mp->async.cur_cmd = mp->async.next_cmd;
		mp->async.next_cmd.id = ASYNC_CMD_WAIT;
		mp->async.next_cmd.arg = 0;
		pthread_mutex_unlock(&mp->async.mtx);
		switch (mp->async.cur_cmd.id) {
			case ASYNC_CMD_OPEN:
				if (avos_mp_open_common(mp) == AVOS_ERR_OK)
					avos_mp_sendevent(mp, MEDIA_PREPARED, 0, 0);
				else
					avos_mp_sendevent(mp, MEDIA_ERROR, 0, 0);
				break;
			case ASYNC_CMD_SEEK:
				avos_mp_seek_common(mp, mp->async.cur_cmd.arg);
				break;
			case ASYNC_CMD_EXIT:
				loop = 0;
				break;
		}
	}

	return NULL;
}

static int async_cmd_wait(avos_mp_t *mp)
{
	int ret;

	pthread_mutex_lock(&mp->async.mtx);
	if (mp->async.cur_cmd.id == ASYNC_CMD_EXIT) {
		ret = 1;
	} else {
		while (mp->async.cur_cmd.id != ASYNC_CMD_WAIT) {
			MPLOGV("WARNING: waiting for async thread");
			pthread_cond_wait(&mp->async.cond, &mp->async.mtx);
		}
		ret = 0;
	}
	pthread_mutex_unlock(&mp->async.mtx);
	return ret;
}

static int async_cmd_is_running(avos_mp_t *mp)
{
	int ret;

	pthread_mutex_lock(&mp->async.mtx);
	ret = mp->async.cur_cmd.id != ASYNC_CMD_WAIT ? 1 : 0;
	pthread_mutex_unlock(&mp->async.mtx);
	return ret;
}

static int async_cmd_add(avos_mp_t *mp, int id, int arg)
{
	pthread_mutex_lock(&mp->async.mtx);
	mp->async.next_cmd.id = id;
	mp->async.next_cmd.arg = arg;
	pthread_cond_broadcast(&mp->async.cond);
	pthread_mutex_unlock(&mp->async.mtx);
	return AVOS_ERR_OK;
}

static avos_mp_t *avos_mp_create(avos_mp_event_cb_t event_cb)
{
	avos_mp_t *mp = acalloc(1, sizeof(avos_mp_t));
	if (!mp)
		return NULL;
	mp->fd = -1;
	mp->event_cb = event_cb;
	mp->metadata_buffer = avos_metadata_create();
	pthread_mutex_init(&mp->metadata_mtx, NULL);
	pthread_mutex_init(&mp->async.mtx, NULL);
	pthread_cond_init(&mp->async.cond, NULL);
	pthread_create(&mp->async.thread, NULL, async_thread, mp);
	MPLOG();
	return mp;
}

static int avos_mp_close(avos_mp_t *mp);
static int avos_mp_destroy(avos_mp_t *mp)
{
	MPLOG();

	avos_mp_close(mp);

	async_cmd_add(mp, ASYNC_CMD_EXIT, 0);
	pthread_join(mp->async.thread, NULL);
	pthread_mutex_destroy(&mp->async.mtx);
	pthread_cond_destroy(&mp->async.cond);

	if (mp->metadata_buffer)
		avos_metadata_destroy(&mp->metadata_buffer);
	pthread_mutex_destroy(&mp->metadata_mtx);
	if (mp->fd != -1)
		close(mp->fd);
	afree(mp);
	return AVOS_ERR_OK;
}

static void avos_mp_setpriv(avos_mp_t *mp, void *priv)
{
	mp->priv = priv;
}

static void *avos_mp_getpriv(avos_mp_t *mp)
{
	return mp->priv;
}

#ifdef UPNP_FUSE_TO_HTTP
const char *upnp_fuse_to_http(avos_mp_t *mp, const char *file_url, char *http_url, int max)
{
	char xml[STREAM_MAX_PATH_LEN+1];
	char buf1[STREAM_MAX_PATH_LEN+1];
	char buf2[STREAM_MAX_PATH_LEN+1];
	char line[1024];
	const char *ext;
	int found = 0;
	FILE *file = NULL;

	ext = get_extension(file_url);
	if (ext[0] == '\0')
		goto end;
	// get file without ext
	cut_path_r(file_url, buf1, STREAM_MAX_PATH_LEN);
	cut_n_extension_r(buf1, buf2, STREAM_MAX_PATH_LEN);
	// get path
	cut_n_file_r(file_url, buf1, STREAM_MAX_PATH_LEN);
	snprintf(xml, STREAM_MAX_PATH_LEN, "%s.metadata/%s.xml", buf1, buf2);

	file = fopen(xml, "r");
	if (!file) {
		// try again with an other file
		snprintf(xml, STREAM_MAX_PATH_LEN, "%s.metadata/%s.%s.xml", buf1, buf2, ext);
		file = fopen(xml, "r");
		if (!file) {
			printf("media_server: fopen failed: %d - %s\n", errno, strerror(errno));
			goto end;
		}
	}
	while (fgets(line, 1024, file) == line) {
		const char *p;

		if ((p = strstr(line, "http-get")) != NULL && (p = strstr(p, "http://")) != NULL) {
			const char *http_ext, *end = strstr(p, "</res>");
			int len;

			if (end - p > STREAM_MAX_PATH_LEN)
				continue;
			strnZcpy(http_url, p, end - p);
			found = 1;
			// priority to url containing original extension
			http_ext = strrchr(http_url, '.');
			if (http_ext) {
				http_ext += 1;
				end = strrchr(http_ext, '?');
				if (end)
					len = end - http_ext;
				else
					len = strlen(http_ext);
				if (strncmp(http_ext, ext, len) == 0)
					break;
			}
		}
	}
end:
	if (file)
		fclose(file);
	return found == 1 ? http_url : file_url;
}
#endif

static int avos_mp_setdatasource(avos_mp_t *mp, const char *path, const char **keys, const char **values)
{
	MPLOG("%s", path);
	if (mp->media) {
		MPLOG("error: setdatasource with already opened video\n");
		return AVOS_ERR_CRITICAL;
	}

#ifdef UPNP_FUSE_TO_HTTP
	char http_url[STREAM_MAX_PATH_LEN];
	if (strstr(path, UPNP_ROOT) != NULL)
		path = upnp_fuse_to_http(mp, path, http_url, STREAM_MAX_PATH_LEN);
#endif
	const char *extra_name = NULL;
	if (keys && values) {
		int i;
		for (i = 0; keys[i] != NULL && values[i] != NULL; ++i) {
			const char *key = keys[i];
			if (strcmp(key, "extra_name") == 0)
				extra_name = values[i];
		}
	}
	if (extra_name)
		MPLOG("EXTRA_NAME: %s", extra_name);
	stream_url_cpy_url_name(&mp->src, path, extra_name);
	get_url_type(&mp->src, &mp->type, &mp->etype);
	MPLOGV("file type: %d|%s  %d|%s", mp->type, mp->type == TYPE_VID ? "VIDEO" : mp->type == TYPE_AUD ? "AUDIO" : "UNKNOWN", mp->etype, av_get_etype_name( mp->etype) );
	if (mp->type == TYPE_NONE || mp->type == TYPE_UNKNOWN)
		avos_mp_sendevent(mp, MEDIA_ERROR, MEDIA_ERROR_VE_FILE_ERROR, 0);
	return AVOS_ERR_OK;
}

static int avos_mp_setdatasource_fd(avos_mp_t *mp, int fd, int64_t offset, int64_t length)
{
	struct stat sb;
	MPLOG("%d:%ld:%ld", fd, offset, length);

	if (fstat(fd, &sb) != 0) {
		MPLOG("can't start fd");
		goto err;
	}
	if (offset >= sb.st_size) {
		MPLOG("offset error");
		goto err;
	}
	if (length == 0)
		length = sb.st_size;
	if (offset + length > sb.st_size)
		length = sb.st_size - offset;

	mp->fd = dup(fd);

	snprintf(mp->src.url, 255, "fd://%d:%"PRId64":%"PRId64, mp->fd, offset, length);
	get_url_type(&mp->src, &mp->type, &mp->etype);

	MPLOGV("file type: %s", mp->type == TYPE_VID ? "video" : mp->type == TYPE_AUD ? "audio" : "unknown");

	close(fd);
	if (mp->type == TYPE_NONE || mp->type == TYPE_UNKNOWN)
		avos_mp_sendevent(mp, MEDIA_ERROR, MEDIA_ERROR_VE_FILE_ERROR, 0);
	return AVOS_ERR_OK;
err:
	close(fd);
	return AVOS_ERR;
}

static int avos_mp_setsurface(avos_mp_t *mp, void *handle)
{
	mp->surface_handle = handle;
	return AVOS_ERR_OK;
}

int avos_mp_getmetadata(avos_mp_t *mp, metadata_buffer_t **buffer)
{
	MPLOGV("%p", buffer);
	if (!buffer)
		return AVOS_ERR;
	pthread_mutex_lock(&mp->metadata_mtx);
	*buffer = avos_metadata_dup(mp->metadata_buffer);
	pthread_mutex_unlock(&mp->metadata_mtx);
	return AVOS_ERR_OK;
}

static int avos_mp_open(avos_mp_t *mp)
{
	MPLOG();
	if (mp->media) {
		MPLOG("error: calling open twice\n");
		return AVOS_ERR_CRITICAL;
	}
	return avos_mp_open_common(mp);
}

static int avos_mp_open_async(avos_mp_t *mp)
{
	MPLOG();
	if (mp->media) {
		MPLOG("error: calling open twice\n");
		return AVOS_ERR_CRITICAL;
	}
	return async_cmd_add(mp, ASYNC_CMD_OPEN, 0);
}

static int avos_mp_close(avos_mp_t *mp)
{
	int ret;

	MPLOG();

	if (!mp->media)
		return AVOS_ERR_OK;

	AVOS_MP_COMMON(abort, mp);

	async_cmd_add(mp, ASYNC_CMD_WAIT, 0);
	async_cmd_wait(mp);

	if (!mp->media) {
		// close can be called more than 1 time (from close/destroy): not an error
		return AVOS_ERR_OK;
	}

	if (mp->type == TYPE_VID) {
		ret = avos_mp_video_close(mp, (avos_mp_video_t *)mp->media);
		avos_mp_video_destroy((avos_mp_video_t **)&mp->media);
	} else if (mp->type == TYPE_AUD) {
		ret = avos_mp_audio_close(mp, (avos_mp_audio_t *)mp->media);
		avos_mp_audio_destroy((avos_mp_audio_t **)&mp->media);
	} else {
		ret = AVOS_ERR;
	}
	return ret;
}

static int avos_mp_start(avos_mp_t *mp)
{
	MPLOG();
	async_cmd_wait(mp);
	AVOS_MP_COMMON(start, mp);
	return AVOS_ERR_OK;
}

static int avos_mp_pause(avos_mp_t *mp)
{
	MPLOG();
	async_cmd_wait(mp);
	AVOS_MP_COMMON(pause, mp);
	return AVOS_ERR_OK;
}

static int avos_mp_isplaying(avos_mp_t *mp, int *ret)
{
	if (async_cmd_is_running(mp)) {
		*ret = mp->last.isplaying;
	} else {
		AVOS_MP_COMMON(isplaying, mp, ret);
		mp->last.isplaying = *ret;
	}
	MPLOGV("%d", *ret);
	return AVOS_ERR_OK;
}

static int avos_mp_seek(avos_mp_t *mp, uint32_t msec)
{
	MPLOGV("%d", msec);
	async_cmd_wait(mp);
	avos_mp_seek_common(mp, msec);
	avos_mp_sendevent(mp, MEDIA_SEEK_COMPLETE, 0, 0);
	return AVOS_ERR_OK;
}

static int avos_mp_seek_async(avos_mp_t *mp, uint32_t msec)
{
	MPLOGV();
	mp->last.pos = msec;
	return async_cmd_add(mp, ASYNC_CMD_SEEK, msec);
}

static int avos_mp_setstarttime(avos_mp_t *mp, uint32_t msec)
{
	MPLOG();
	mp->starttime = msec;
	return AVOS_ERR_OK;
}

static int avos_mp_getpos(avos_mp_t *mp, uint32_t *ret)
{
	if (async_cmd_is_running(mp)) {
		*ret = mp->last.pos;
	} else {
		AVOS_MP_COMMON(getpos, mp, ret);
		mp->last.pos = *ret;
	}
	//MPLOGV("%d", *ret);
	return AVOS_ERR_OK;
}

static int avos_mp_getduration(avos_mp_t *mp, uint32_t *ret)
{
	if (async_cmd_is_running(mp)) {
		*ret = mp->last.duration;
	} else {
		AVOS_MP_COMMON(getduration, mp, ret);
		mp->last.duration = *ret;
	}
	MPLOGV("%d", *ret);
	return AVOS_ERR_OK;
}

static int avos_mp_setlooping(avos_mp_t *mp, int looping)
{
	MPLOG("%d", looping);
	mp->islooping = looping;
	return AVOS_ERR_OK;
}

static int avos_mp_islooping(avos_mp_t *mp, int *ret)
{
	*ret = mp->islooping;
	MPLOGV("%d", *ret);
	return AVOS_ERR_OK;
}

static int avos_mp_getaudiosessionid(avos_mp_t *mp, int *ret)
{
	AVOS_MP_COMMON(getaudiosessionid, mp, ret);
	MPLOGV("%d", *ret);
	return AVOS_ERR_OK;
}

static int avos_mp_setaudiotrack(avos_mp_t *mp, int track, int *ret)
{
	MPLOG("%d", track);
	async_cmd_wait(mp);
	AVOS_MP_VIDEO(setaudiotrack, mp, track, ret);
	return AVOS_ERR_OK;
}

static int avos_mp_checksubtitles(avos_mp_t *mp)
{
	MPLOG();
	async_cmd_wait(mp);
	AVOS_MP_VIDEO(checksubtitles, mp);
	return AVOS_ERR_OK;
}

static int avos_mp_setsubtitletrack(avos_mp_t *mp, int track, int *ret)
{
	MPLOG("%d", track);
	async_cmd_wait(mp);
	AVOS_MP_VIDEO(setsubtitletrack, mp, track, ret);
	return AVOS_ERR_OK;
}

static int avos_mp_setsubtitledelay(avos_mp_t *mp, int delay)
{
	MPLOG("%d", delay);
	async_cmd_wait(mp);
	AVOS_MP_VIDEO(setsubtitledelay, mp, delay);
	return AVOS_ERR_OK;
}

static int avos_mp_setsubtitleratio(avos_mp_t *mp, uint32_t n, uint32_t d)
{
	MPLOG("%d/%d", n, d);
	async_cmd_wait(mp);
	AVOS_MP_VIDEO(setsubtitleratio, mp, n, d);
	return AVOS_ERR_OK;
}

static int avos_mp_setaudiofilter(avos_mp_t *mp, int n, int nightOn)
{
	MPLOG("%d %d", n, nightOn);
	async_cmd_wait(mp);
	AVOS_MP_VIDEO(setaudiofilter, mp, n, nightOn);
	return AVOS_ERR_OK;
}

static int avos_mp_setavdelay(avos_mp_t *mp, int delay)
{
	MPLOG("%d", delay);
	async_cmd_wait(mp);
	AVOS_MP_VIDEO(setavdelay, mp, delay);
	return AVOS_ERR_OK;
}

static int avos_mp_setnextrack(avos_mp_t *mp, const char *path)
{
#ifdef UPNP_FUSE_TO_HTTP
	char http_url[STREAM_MAX_PATH_LEN];
	if (path && strstr(path, UPNP_ROOT) != NULL)
		path = upnp_fuse_to_http(mp, path, http_url, STREAM_MAX_PATH_LEN);
#endif
	MPLOG("%s", path);
	async_cmd_wait(mp);
	AVOS_MP_AUDIO(setnextrack, mp, path);
	return AVOS_ERR_OK;
}

static const avos_mp_handle_t avos_mp_handle = {
	.create = avos_mp_create,
	.destroy = avos_mp_destroy,
	.setpriv = avos_mp_setpriv,
	.getpriv = avos_mp_getpriv,
	.setdatasource = avos_mp_setdatasource,
	.setdatasource_fd = avos_mp_setdatasource_fd,
	.setsurface = avos_mp_setsurface,
	.getmetadata = avos_mp_getmetadata,
	.open = avos_mp_open,
	.open_async = avos_mp_open_async,
	.close = avos_mp_close,
	.start = avos_mp_start,
	.pause = avos_mp_pause,
	.isplaying = avos_mp_isplaying,
	.seek = avos_mp_seek,
	.seek_async = avos_mp_seek_async,
	.setstarttime = avos_mp_setstarttime,
	.getpos = avos_mp_getpos,
	.getduration = avos_mp_getduration,
	.setlooping = avos_mp_setlooping,
	.islooping = avos_mp_islooping,
	.getaudiosessionid = avos_mp_getaudiosessionid,
	.setaudiotrack = avos_mp_setaudiotrack,
	.checksubtitles = avos_mp_checksubtitles,
	.setsubtitletrack = avos_mp_setsubtitletrack,
	.setsubtitledelay = avos_mp_setsubtitledelay,
	.setsubtitleratio = avos_mp_setsubtitleratio,
	.setaudiofilter = avos_mp_setaudiofilter,
	.setavdelay = avos_mp_setavdelay,
	.setnextrack = avos_mp_setnextrack,
};

const avos_mp_handle_t *avos_mp_get_handle()
{
	return &avos_mp_handle;
}

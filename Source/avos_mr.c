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
#include "avos_mr.h"
#include "avos_mr_metadata.h"
#include "astdlib.h"
#include "debug.h"
#include "global.h"
#include "file_type.h"
#include "file_info.h"
#include "thumb.h"
#include "thumb_stream.h"
#include "athread.h"
#include "stream_config.h"

#define DBG if(Debug[DBG_VIDEO_PLAYER] || Debug[DBG_AUDIO_PLAYER])
#define DBG2 if((Debug[DBG_VIDEO_PLAYER] > 1) || (Debug[DBG_AUDIO_PLAYER] > 1))

#define MRLOG(fmt, ...) serprintf("%p|%s: " fmt "\n", mr, __FUNCTION__, ##__VA_ARGS__)
#define MRLOGV DBG MRLOG

struct avos_mr {
	int fd;
	STREAM_URL src;
	int type;
	int etype;
	const char *mimetype;

	FILE_INFO		info;
	APIC			apic;
	int			info_valid;
	thumb_stream_t		*thumb_stream;

	metadata_buffer_t *metadata_buffer;
};

static avos_mr_t *avos_mr_create()
{
	avos_mr_t *mr = acalloc(1, sizeof(avos_mr_t));
	if (!mr)
		return NULL;
	mr->fd = -1;
	mr->metadata_buffer = avos_metadata_create();
	MRLOGV();
	return mr;
}

static int avos_mr_destroy(avos_mr_t *mr)
{
	MRLOGV();

	if (mr->metadata_buffer)
		avos_metadata_destroy(&mr->metadata_buffer);
	if (mr->thumb_stream)
		thumb_stream_destroy(mr->thumb_stream);
	if (mr->apic.buffer)
		free(mr->apic.buffer);
	if (mr->fd != -1)
		close(mr->fd);
	afree(mr);
	return AVOS_ERR_OK;
}

static int avos_mr_setdatasource_common(avos_mr_t *mr)
{
	if (mr->info_valid) {
		if (mr->apic.buffer)
			free(mr->apic.buffer);
		memset(&mr->info, 0, sizeof(FILE_INFO));
		memset(&mr->apic, 0, sizeof(APIC));
		mr->info_valid = 0;
	}
	if (mr->thumb_stream) {
		thumb_stream_destroy(mr->thumb_stream);
		mr->thumb_stream = NULL;
	}
	mr->apic.buffer_size = APIC_MAX_SIZE;
	get_url_type_and_mime(&mr->src, &mr->type, &mr->etype, &mr->mimetype);

	MRLOG("file type: %s", mr->type == TYPE_VID ? "video" : mr->type == TYPE_AUD ? "audio" : "unknown");
	return mr->type == TYPE_NONE ? AVOS_ERR : AVOS_ERR_OK;
}

static int avos_mr_setdatasource(avos_mr_t *mr, const char *path, const char **keys, const char **values)
{
	MRLOGV("%s", path);
	// XXX handle extra
	stream_url_cpy_url(&mr->src, path);
	return avos_mr_setdatasource_common(mr);
}

static int avos_mr_setdatasource_fd(avos_mr_t *mr, int fd, int64_t offset, int64_t length)
{
	struct stat sb;
	MRLOG("%d:%ld:%ld", fd, offset, length);

	if (fstat(fd, &sb) != 0) {
		MRLOG("can't start fd");
		goto err;
	}
	if (offset >= sb.st_size) {
		MRLOG("offset error");
		goto err;
	}
	if (length == 0)
		length = sb.st_size;
	if (offset + length > sb.st_size)
		length = sb.st_size - offset;

	mr->fd = dup(fd);

	snprintf(mr->src.url, 255, "fd://%d:%"PRId64":%"PRId64, mr->fd, offset, length);
	close(fd);
	return avos_mr_setdatasource_common(mr);
err:
	close(fd);
	return AVOS_ERR;
}

static int avos_mr_fillmetadata(avos_mr_t *mr)
{
#define ADD_STR(_id, _str) do { \
	if (avos_metadata_append_str(buffer, (_id), (_str)) == -1) \
		goto quit; \
} while(0)
#define ADD_INT(_id, _arg) do { \
	sprintf(int_buffer, "%d", (_arg)); \
	if (avos_metadata_append_str(buffer, (_id), int_buffer) == -1) \
		goto quit; \
} while(0)
#define ADD_INT64(_id, _arg) do { \
	sprintf(int_buffer, "%"PRId64, (_arg)); \
	if (avos_metadata_append_str(buffer, (_id), int_buffer) == -1) \
		goto quit; \
} while(0)

	const char *token;
	char int_buffer[64];
	int i, gap_key;
	int duration = mr->info.duration;
	metadata_buffer_t *buffer = mr->metadata_buffer;
	ID3_TAG *id3_tag = &mr->info.id3_tag;
	AV_PROPERTIES *av = &mr->info.av;
	AUDIO_PROPERTIES *audiop = &av->audio[0];
	VIDEO_PROPERTIES *videop = &av->video[0];

	if ((mr->type != TYPE_AUD && mr->type != TYPE_VID && mr->type != TYPE_LST) || !buffer)
		return AVOS_ERR;

	MRLOGV();

	avos_metadata_write_begin(buffer);

	if (mr->info.size > 0)
		ADD_INT64(AVOS_MR_METADATA_FILE_SIZE, mr->info.size);

	if (id3_tag && id3_tag->valid ) {
		ADD_INT(AVOS_MR_METADATA_CD_TRACK_NUMBER, id3_tag->track);
		ADD_INT(AVOS_MR_METADATA_DISC_NUMBER, id3_tag->discnumber);
		ADD_STR(AVOS_MR_METADATA_ALBUM, id3_tag->album);
		ADD_STR(AVOS_MR_METADATA_ARTIST,id3_tag->artist);
		ADD_STR(AVOS_MR_METADATA_ALBUMARTIST, id3_tag->album_artist);
		ADD_STR(AVOS_MR_METADATA_COMPOSER, id3_tag->composer);
		ADD_STR(AVOS_MR_METADATA_AUTHOR, id3_tag->author);
		ADD_STR(AVOS_MR_METADATA_WRITER, id3_tag->writer);
		ADD_STR(AVOS_MR_METADATA_GENRE, id3_tag->genre);
		ADD_STR(AVOS_MR_METADATA_TITLE, id3_tag->title);
		ADD_STR(AVOS_MR_METADATA_DATE, id3_tag->year);
		ADD_STR(AVOS_MR_METADATA_COMPILATION, id3_tag->compilation);
		ADD_STR(AVOS_MR_METADATA_LOCATION, id3_tag->location);
		ADD_INT(AVOS_MR_METADATA_IS_DRM, 0);
	}
	ADD_INT(AVOS_MR_METADATA_BITRATE, ((audiop->bytesPerSec + videop->bytesPerSec) * 8));
	ADD_INT(AVOS_MR_METADATA_HAS_AUDIO, av->as_max ? 1 : 0);
	ADD_INT(AVOS_MR_METADATA_HAS_VIDEO, av->vs_max ? 1 : 0);

	if (duration > 0)
		ADD_INT(AVOS_MR_METADATA_DURATION, duration);
	if (mr->mimetype)
		ADD_STR(AVOS_MR_METADATA_MIMETYPE, mr->mimetype);
	if (audiop->samplesPerSec > 0)
		ADD_INT(AVOS_MR_METADATA_SAMPLE_RATE, audiop->samplesPerSec);
	if (audiop->bytesPerSec > 0)
		ADD_INT(AVOS_MR_METADATA_AUDIO_BITRATE, (audiop->bytesPerSec * 8));
	if (audiop->channels > 0)
		ADD_INT(AVOS_MR_METADATA_NUMBER_OF_CHANNELS, audiop->channels);
	if (audiop->format > 0)
		ADD_INT(AVOS_MR_METADATA_AUDIO_WAVE_CODEC, audiop->format);

	if (videop->bytesPerSec > 0)
		ADD_INT(AVOS_MR_METADATA_VIDEO_BITRATE, (videop->bytesPerSec * 8));
	if (videop->framesPerSec > 0)
		ADD_INT(AVOS_MR_METADATA_FRAMES_PER_THOUSAND_SECONDS, videop->framesPerSec);
	if (videop->fourcc > 0)
		ADD_INT(AVOS_MR_METADATA_VIDEO_FOURCC_CODEC, videop->fourcc);
	if (videop->width > 0)
		ADD_INT(AVOS_MR_METADATA_VIDEO_WIDTH, videop->width);
	if (videop->height > 0)
		ADD_INT(AVOS_MR_METADATA_VIDEO_HEIGHT, videop->height);

	if (mr->type == TYPE_VID && av->vs_max > 0) {
		ADD_INT(AVOS_MR_METADATA_NB_VIDEO_TRACK, av->vs_max);

		gap_key = AVOS_MR_METADATA_VIDEO_TRACK;
		token = video_get_fourcc_name(videop);
		ADD_STR(gap_key + AVOS_MR_METADATA_VIDEO_TRACK_FORMAT, token);

		if (videop->width > 0 && videop->height > 0) {
			ADD_INT(gap_key + AVOS_MR_METADATA_VIDEO_TRACK_WIDTH, videop->width);
			ADD_INT(gap_key + AVOS_MR_METADATA_VIDEO_TRACK_HEIGHT, videop->height);
			ADD_INT(gap_key + AVOS_MR_METADATA_VIDEO_TRACK_ASPECT_N, videop->aspect_n);
			ADD_INT(gap_key + AVOS_MR_METADATA_VIDEO_TRACK_ASPECT_D, videop->aspect_d);
			ADD_INT(gap_key + AVOS_MR_METADATA_VIDEO_TRACK_PIXEL_FORMAT, videop->colorspace);
		}

		ADD_INT(gap_key + AVOS_MR_METADATA_VIDEO_TRACK_PROFILE, videop->profile);

		ADD_INT(gap_key + AVOS_MR_METADATA_VIDEO_TRACK_LEVEL, videop->level);

		if (videop->bytesPerSec > 0)
			ADD_INT(gap_key + AVOS_MR_METADATA_VIDEO_TRACK_BIT_RATE, videop->bytesPerSec / 125);

		if (videop->framesPerSec > 0)
			ADD_INT(gap_key + AVOS_MR_METADATA_VIDEO_TRACK_FPS, videop->framesPerSec);

		if (videop->rate > 0)
			ADD_INT(gap_key + AVOS_MR_METADATA_VIDEO_TRACK_FPS_RATE, videop->rate);

		if (videop->scale > 0)
			ADD_INT(gap_key + AVOS_MR_METADATA_VIDEO_TRACK_FPS_SCALE, videop->scale);

		ADD_INT(gap_key + AVOS_MR_METADATA_VIDEO_TRACK_S3D_MODE, videop->stereo_mode);
	}
	if (mr->type == TYPE_AUD)
		av->as_max = 1;
	if (av->as_max > 0 && av->as_max <= AUDIO_STREAM_MAX) {
		ADD_INT(AVOS_MR_METADATA_NB_AUDIO_TRACK, av->as_max);
		for (i = 0; i < av->as_max; ++i) {
			audiop = &av->audio[i];
			gap_key = AVOS_MR_METADATA_AUDIO_TRACK + i * AVOS_MR_METADATA_AUDIO_TRACK_MAX;

			ADD_STR(gap_key + AVOS_MR_METADATA_AUDIO_TRACK_NAME, audiop->name);

			token = audio_get_format_name(audiop);
			ADD_STR(gap_key + AVOS_MR_METADATA_AUDIO_TRACK_FORMAT, token);

			if (audiop->bytesPerSec > 0)
				ADD_INT(gap_key + AVOS_MR_METADATA_AUDIO_TRACK_BIT_RATE, audiop->bytesPerSec / 125);

			if (audiop->samplesPerSec > 0)
				ADD_INT(gap_key + AVOS_MR_METADATA_AUDIO_TRACK_SAMPLE_RATE, audiop->samplesPerSec);

			token = audio_get_channel_layout_name(audiop->channels);
			ADD_STR(gap_key + AVOS_MR_METADATA_AUDIO_TRACK_CHANNELS, token);

			ADD_INT(gap_key + AVOS_MR_METADATA_AUDIO_TRACK_VBR, audiop->vbr);

			int supported = 0;
			STREAM_DEC_AUDIO *dec = stream_get_audio_dec( audiop );
			if( dec ) {
				supported = 1;
				if( dec->is_supported ) {
					supported = dec->is_supported( audiop ) ? 1 : 0;
				}
			}
			ADD_INT(gap_key + AVOS_MR_METADATA_AUDIO_TRACK_SUPPORTED, supported);
		}
	}
	if (mr->type == TYPE_VID && av && av->subs_max > 0 && av->subs_max <= SUB_STREAM_MAX) {
		ADD_INT(AVOS_MR_METADATA_NB_SUBTITLE_TRACK, av->subs_max);
		for (i = 0; i < av->subs_max; ++i) {
			gap_key = AVOS_MR_METADATA_SUBTITLE_TRACK + i * AVOS_MR_METADATA_SUBTITLE_TRACK_MAX;
			ADD_STR(gap_key + AVOS_MR_METADATA_SUBTITLE_TRACK_NAME, av->sub[i].name);
			ADD_STR(gap_key + AVOS_MR_METADATA_SUBTITLE_TRACK_PATH, av->sub[i].path);
		}
	}

quit:
	return avos_metadata_write_end(buffer);
}

static int avos_mr_retrieve(avos_mr_t *mr)
{
	if (!mr->info_valid) {
		if (get_url_info(&mr->src, mr->type, mr->etype, &mr->info, &mr->apic, NULL))
			return AVOS_ERR;
		avos_mr_fillmetadata(mr);
		mr->info_valid = 1;
	}
	return AVOS_ERR_OK;
}

static int avos_mr_getmetadata(avos_mr_t *mr, metadata_buffer_t **buffer)
{
	MRLOGV("%p", buffer);
	if (!buffer)
		return AVOS_ERR;
	if (avos_mr_retrieve(mr) != AVOS_ERR_OK)
		return AVOS_ERR;
	*buffer = avos_metadata_dup(mr->metadata_buffer);
	return AVOS_ERR_OK;
}

static const char* avos_mr_extractmetadata(avos_mr_t *mr, uint32_t id)
{
	if (avos_mr_retrieve(mr) != AVOS_ERR_OK)
		return NULL;
	avos_msg_t *msg = avos_metadata_get(mr->metadata_buffer, id);
	if (!msg || msg->type != AVOS_MSG_TYPE_STR) {
		MRLOGV("[%u|(null)]", id);
		return NULL;
	}
	MRLOGV("[%u|%s]", id, (const char *)msg->data);
	return (const char *)msg->data;
}

static int avos_mr_getframe(avos_mr_t *mr, int time_ms, avos_bgra_bitmap_t **pbitmap)
{
#define DEBUG_THUMB_TIME
#ifdef DEBUG_THUMB_TIME
#include <sys/time.h>
	struct timeval start, end, diff;
	gettimeofday(&start, NULL);
#endif

	avos_bgra_bitmap_t *bitmap;
	int rotation;
	IMAGE *img;

	MRLOGV("%d", time_ms);

	if (!pbitmap)
		return AVOS_ERR;

	if (!mr->thumb_stream)
		mr->thumb_stream = thumb_stream_create();
	if (!mr->thumb_stream) {
		*pbitmap = NULL;
		return AVOS_ERR_OK;
	}
	img = thumb_stream_get_frame(mr->thumb_stream, &mr->src, mr->etype, time_ms, AV_IMAGE_BGRA_32, &rotation);
	if (!img) {
		*pbitmap = NULL;
		return AVOS_ERR_OK;
	}
#ifdef DEBUG_THUMB_TIME
	gettimeofday(&end, NULL);
	timersub(&end, &start, &diff);
	MRLOGV("thumbnail time: %d ms\n", diff.tv_sec * 1000 + (diff.tv_usec / 1000));
#endif

	bitmap = (avos_bgra_bitmap_t *)calloc(1, sizeof(avos_bgra_bitmap_t));
	if (!bitmap) {
		*pbitmap = NULL;
		return AVOS_ERR;
	}

	bitmap->width = img->width;
	bitmap->height = img->height;
	bitmap->linestep = img->linestep[0];
	bitmap->rotation = rotation;
	bitmap->data_size = img->size;
	bitmap->data = img->data[0];

	*pbitmap = bitmap;
	return AVOS_ERR_OK;
}

static int avos_mr_getapic(avos_mr_t *mr, avos_apic_t **papic)
{
	avos_apic_t *apic;

	*papic = NULL;
	if (avos_mr_retrieve(mr) != AVOS_ERR_OK || !mr->apic.valid)
		return AVOS_ERR_OK; // not critical, apic is NULL

	apic = (avos_apic_t *) calloc(1, sizeof(avos_apic_t) + mr->apic.size);
	if (!apic)
		return AVOS_ERR;
	apic->size = mr->apic.size;
	memcpy(apic->data, mr->apic.buffer, mr->apic.size);
	*papic = apic;
	return AVOS_ERR_OK;
}

static const avos_mr_handle_t avos_mr_handle = {
	.create = avos_mr_create,
	.destroy = avos_mr_destroy,
	.setdatasource = avos_mr_setdatasource,
	.setdatasource_fd = avos_mr_setdatasource_fd,
	.getmetadata = avos_mr_getmetadata,
	.extractmetadata = avos_mr_extractmetadata,
	.getframe = avos_mr_getframe,
	.getapic = avos_mr_getapic,
};

const avos_mr_handle_t *avos_mr_get_handle()
{
	return &avos_mr_handle;
}

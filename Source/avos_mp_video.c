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

#include "app_av.h"
#include "avos_mp.h"
#include "avos_mp_priv.h"
#include "browse.h"
#include "debug.h"
#include "device_config.h"
#include "global.h"
#include "image.h"
#include "stream.h"
#include "stream_subtitle.h"
#include "sub_engine.h"

extern SUB_ENGINE *g_sub_engine;
extern int libavos_get_ac3_recoding_enabled(void);

#define DBG if(Debug[DBG_VIDEO_PLAYER])
#define DBG2 if(Debug[DBG_VIDEO_PLAYER] > 1)

#define MPLOG(fmt, ...) serprintf("%p|%s: " fmt "\n", mp, __FUNCTION__, ##__VA_ARGS__)

#define SUBTITLE_SEND_OFFSET (-100)

static int64_t engine_clock_cb(void *ctx) {
	STREAM *s = (STREAM *)ctx; // Cast the context directly to the STREAM pointer
	if (!s) return 0;
	int dummy_duration = 0;

	// 1. Get the raw video PTS
	int64_t time = stream_get_current_time(s, &dummy_duration);

	// 2. Apply the exact same correction as _sub_decode
	time -= s->subtitle_offset;
	if (time < 0) {
		time = 0;
	}

	return time;
}

static int stream_buffer_size = 24;

struct avos_mp_video {
	STREAM *s;
	int abort;
	int last_seekable;
	int last_pauseable;
	int last_duration;
	int buffered_pos;
	int send_sub;
	int width;
	int height;
	int aspect_n;
	int aspect_d;
};

static void send_state_msg(avos_mp_t *mp, avos_mp_video_t *video)
{
	if (!video->last_seekable)
		avos_mp_sendevent(mp, MEDIA_INFO, MEDIA_INFO_NOT_SEEKABLE, 0);
}

static void send_stream_error(avos_mp_t *mp, avos_mp_video_t *video)
{
	const char *msg = video->s->video_error_qualifier == VEQ_SEE_DESCRIPTION ? video->s->video_error_desc : NULL;
	/*
	 * error < 1000: generic error,
	 * error >= 1000: avos error,
	 * so, add 1000 to avos specific error
	 */
	avos_mp_sendevent_msg(mp, MEDIA_ERROR, MEDIA_ERROR_VE_NO_ERROR + video->s->video_error,
	    MEDIA_ERROR_VEQ_NONE + video->s->video_error_qualifier, msg);
}

static void send_metadata(avos_mp_t *mp, avos_mp_video_t *video, int notify)
{
	int decoder = MP_DECODER_ANY;

	if (video->s->video_dec) {
		switch (video->s->video_dec->cpu) {
			case STREAM_CPU_ARM2:
				decoder = MP_DECODER_HW_OMX;
				break;
			case STREAM_CPU_DSP2:
				decoder = MP_DECODER_HW_SFDEC_OMXCODEC;
				break;
			case STREAM_CPU_DSP3:
				decoder = MP_DECODER_HW_SFDEC_MEDIACODEC;
				break;
			case STREAM_CPU_DSP4:
				decoder = MP_DECODER_HW_OMXPLUS;
				break;
			default:
				decoder = MP_DECODER_SW;
				break;
		}
	}
	int changed = avos_mp_fillmetadata(mp,
	    TYPE_VID,
	    video->s->size,
	    &video->s->tag,
	    &video->s->av,
	    NULL,
	    video->last_duration,
	    video->last_seekable,
	    video->last_pauseable,
	    decoder);
	if (changed && notify) {
		MPLOG("changed");
		avos_mp_sendevent(mp, MEDIA_INFO, MEDIA_INFO_METADATA_UPDATE, 0);
	}
}

static void send_subtitle(avos_mp_t *mp, avos_mp_video_t *video)
{
	int sub_time;
	VIDEO_FRAME *sub_frame;
	avos_msg_t *msg = NULL;

	DBG MPLOG();

	if (!video->send_sub)
		return;

	// --- NATIVE OPENGL UPGRADE ---
	// ALL subtitle formats (Text and Bitmap) are now handled natively by the GPU compositor.
	// Explicitly abort sending any JNI payload to the legacy Java SubtitleManager.
	int fmt = video->s->av.sub[video->s->av.subs].format;
	if (fmt == SUB_FORMAT_SSA ||
		fmt == SUB_FORMAT_TEXT ||
		fmt == SUB_FORMAT_DVD_GFX ||
		fmt == SUB_FORMAT_PGS) {
		return;
	}
	sub_frame = stream_get_current_subtitle(video->s);
	sub_time = sub_frame->time - SUBTITLE_SEND_OFFSET;

	if (video->s->av.sub[video->s->av.subs].gfx) {
		msg = avos_msg_new_bitmap_subtitle(0, sub_time, sub_frame->duration, (IMAGE *)sub_frame);
	} else {
		msg = avos_msg_new_text_subtitle(0, sub_time, sub_frame->duration, sub_frame->data[0]);
	}
	if (msg)
		avos_mp_sendevent_data(mp, MEDIA_SUBTITLE, 0, 0, msg);
}

static int is_stream_seekable(avos_mp_t *mp, avos_mp_video_t *video)
{
	int new_seekable;

	new_seekable = stream_seekable(video->s);
	if (new_seekable != video->last_seekable) {
		DBG MPLOG("stream: seekable state changed: %d", new_seekable);
		video->last_seekable = new_seekable;
		send_state_msg(mp, video);
	}
	return new_seekable;
}

static int is_stream_pauseable(avos_mp_t *mp, avos_mp_video_t *video)
{
	int new_pauseable;

	new_pauseable = stream_pauseable(video->s);
	if (new_pauseable != video->last_pauseable) {
		DBG MPLOG("stream: pauseable state changed: %d", new_pauseable);
		video->last_pauseable = new_pauseable;
		send_state_msg(mp, video);
	}
	return new_pauseable;
}

static void stream_msg_cb(STREAM *s, STREAM_MESSAGE message)
{
	avos_mp_t *mp = (avos_mp_t *)stream_get_user_ctx(s);
	avos_mp_video_t *video = avos_mp_getvideo(mp);
	VIDEO_PROPERTIES *vp;

	DBG MPLOG("%d", message);
	switch (message) {
	case STREAM_VIDEO_PROPS_CHANGED:
		vp = &video->s->av.video[0];
		if (video->aspect_n != vp->aspect_n || video->aspect_d != vp->aspect_d) {
			video->aspect_n = vp->aspect_n;
			video->aspect_d = vp->aspect_d;
			avos_mp_sendevent(mp, MEDIA_SET_VIDEO_ASPECT, video->aspect_n, video->aspect_d);
		}
		if (video->width != vp->width || video->height != vp->height) {
			video->width = vp->width;
			video->height = vp->interlaced == VIDEO_INTERLACED_ONE_FIELD ? vp->height * 2 : vp->height;
			avos_mp_sendevent(mp, MEDIA_SET_VIDEO_SIZE, video->width, video->height);
		}
		break;
	case STREAM_AUDIO_PROPS_CHANGED:
	case STREAM_SUB_PROPS_CHANGED:
	case STREAM_DECODER_CHANGED:
		send_metadata(mp, video, 1);
		break;
	case STREAM_SUBTITLE_CHANGED:
		send_subtitle(mp, video);
		break;
	default:
		break;
	}

}
static void stream_stop_handler(STREAM *s)
{
	avos_mp_t *mp = (avos_mp_t *)stream_get_user_ctx(s);
	avos_mp_video_t *video = avos_mp_getvideo(mp);

	DBG MPLOG();
	if (!video)
		return;
	if (video->s->video_error != VE_NO_ERROR) {
		send_stream_error(mp, video);
	} else {
		avos_mp_sendevent(mp, MEDIA_PLAYBACK_COMPLETE, 0, 0);
	}
}

static int stream_abort_handler(void *ctx)
{
	avos_mp_t *mp = (avos_mp_t *)stream_get_user_ctx((STREAM *) ctx);
	avos_mp_video_t *video = avos_mp_getvideo(mp);
	return video->abort;
}

static void stream_progress_handler(STREAM *s, int total, int progress)
{
	avos_mp_t *mp = (avos_mp_t *)stream_get_user_ctx(s);
	avos_mp_sendevent(mp, MEDIA_BUFFERING_UPDATE, 100 * progress / total, 0);
}

STREAM_SINK_VIDEO *stream_sink_video_FAKE_new(void);

avos_mp_video_t *avos_mp_video_create()
{
	return acalloc(1, sizeof(avos_mp_video_t));
}

int avos_mp_video_destroy(avos_mp_video_t **video)
{
	afree(*video);
	*video = NULL;
	return AVOS_ERR_OK;
}

int avos_mp_video_open(avos_mp_t *mp, avos_mp_video_t *video, STREAM_URL *src, int etype, void *surface_handle, int starttime)
{
	int _flags = STREAM_PAUSED;
	VIDEO_PROPERTIES *vp;
	int from_list = 0; // XXX
	const char *subtitle_path = device_config_get_subtitlepath();
	int decoder = device_config_get_decoder();

	video->send_sub = 1;
	if (!(video->s = stream_new())) {
		MPLOG("error: stream_new");
		afree(video);
		return AVOS_ERR;
	}
	stream_set_user_ctx(video->s, mp);
	if (!is_local_file(src->url)) {
		_flags |= STREAM_FILE_NONLOCAL;
		/* NETWORK_ROOT is fuse, don't send buffered position when video come from fuse */
		if (strstr(src->url, NETWORK_ROOT) == NULL)
			video->buffered_pos = 1;
		else
			_flags |= STREAM_MULTI;
	} else {
		_flags |= STREAM_MULTI;
	}
	if (_flags & STREAM_FILE_NONLOCAL) {
		stream_set_buffer_size(video->s, get_default_stream_buffer_size()); // use less memory for NONLOCAL playback
	} else if ( !device_has_hdd() ) {
		stream_set_buffer_size(video->s, get_default_stream_buffer_size()); // use less memory for S units
	}

	if (subtitle_path) {
		const char *sub_urls[] = { subtitle_path, NULL };
		stream_set_subtitle_url(video->s, sub_urls);
	}
	switch (decoder) {
		case MP_DECODER_ANY:
			stream_set_cpu_priority(video->s, STREAM_CPU_ANY);
			break;
		case MP_DECODER_SW:
			stream_set_cpu_priority(video->s, STREAM_CPU_ARM);
			break;
		case MP_DECODER_HW_OMX:
			stream_set_cpu_priority(video->s, STREAM_CPU_ARM2);
			break;
		case MP_DECODER_HW_OMXPLUS:
			stream_set_cpu_priority(video->s, STREAM_CPU_DSP4);
			break;
		case MP_DECODER_HW_SFDEC_OMXCODEC:
			stream_set_cpu_priority(video->s, STREAM_CPU_DSP2);
			break;
		case MP_DECODER_HW_SFDEC_MEDIACODEC:
			stream_set_cpu_priority(video->s, STREAM_CPU_DSP3);
			break;

	}

	stream_set_max_video_dimensions(video->s, VIDEO_MAX_WIDTH, VIDEO_MAX_HEIGHT);
	stream_set_message_cb(video->s, stream_msg_cb);
	stream_set_subtitle_offset(video->s, SUBTITLE_SEND_OFFSET);
	stream_set_stop_handler(video->s, stream_stop_handler);
	stream_set_abort_handler(video->s, stream_abort_handler);
	stream_set_progress_handler(video->s, stream_progress_handler);
	stream_set_audio_filter_level(video->s, 0, 0 );
	if (starttime) {
		stream_set_start_time(video->s, starttime);
	}

	stream_set_surface_handle(video->s, surface_handle);

	if (stream_open(video->s, src, etype, _flags)) {
		MPLOG("stream_open() failed: %d", video->s->video_error);
		if (video->s->video_error == VE_NO_ERROR)
			video->s->video_error = VE_ERROR;
		goto stream_err;
	}

	AV_set_state(AV_PLAYING, TYPE_VID, 0, video->s, NULL);
	if (stream_start(video->s)) {
		MPLOG("stream_start() failed");
		if (video->s->video_error == VE_NO_ERROR)
			video->s->video_error = VE_ERROR;
		goto stream_err;
	}

	video->last_seekable = stream_seekable(video->s);
	video->last_pauseable = stream_pauseable(video->s);
	stream_get_current_time(video->s, &video->last_duration);

	vp = &video->s->av.video[0];
	video->aspect_n = vp->aspect_n;
	video->aspect_d = vp->aspect_d;
	avos_mp_sendevent(mp, MEDIA_SET_VIDEO_ASPECT, video->aspect_n, video->aspect_d);
	video->width = vp->width;
	video->height = vp->interlaced == VIDEO_INTERLACED_ONE_FIELD ? vp->height * 2 : vp->height;
	avos_mp_sendevent(mp, MEDIA_SET_VIDEO_SIZE, video->width, video->height);

	is_stream_seekable(mp, video);
	is_stream_pauseable(mp, video);
	send_metadata(mp, video, 0);
	send_state_msg(mp, video);

	stream_set_volume(video->s, 100, 100);
	// NEW: Start the subtitle engine clock
	video->s->sub_engine = (void*)g_sub_engine;

	// Use the isolated instance
	if (video->s->sub_engine) {
		sub_engine_start((SUB_ENGINE*)video->s->sub_engine, engine_clock_cb, video->s);
	}

	return AVOS_ERR_OK;
stream_err:
	// if from_list: silently fail
	if (!from_list && video->s && video->s->video_error != VE_NO_ERROR && video->s->video_error != VE_USER_ABORT) {
		send_metadata(mp, video, 0);
		send_stream_error(mp, video);
	}
	return AVOS_ERR_OK;
}

int avos_mp_video_close(avos_mp_t *mp, avos_mp_video_t *video)
{
	MPLOG();

	// Stop the subtitle engine clock FIRST: its clock_ctx is the raw STREAM*
	// (video->s) that stream_delete() frees below, so the render thread must
	// stop dereferencing it (via engine_clock_cb) before that happens.
	if (video->s && video->s->sub_engine) sub_engine_stop((SUB_ENGINE*)video->s->sub_engine);
	if (video->s) {
		AV_set_state(AV_STOPPED, 0, 0, NULL, NULL);
		stream_stop(video->s);
		stream_delete(&video->s);
	}
	// Close the subtitle track only AFTER the stream -- and its background
	// subtitle-decode thread (stream_sub_dec_thread in stream_subtitle.c) --
	// is fully torn down. Closing it earlier (right after sub_engine_stop, as
	// the first pass at this fix did) left a window where that thread could
	// still be mid-loop and call back into sub_engine_open_track()/feed()
	// with this video's own subtitle data, re-populating g_sub_engine's
	// active track right after we'd just cleared it -- so the next video
	// could still inherit stale state. Waiting until the stream (and its
	// thread) is actually gone removes that window.
	if (video->s && video->s->sub_engine) sub_engine_close_track((SUB_ENGINE*)video->s->sub_engine);

	return AVOS_ERR_OK;
}

int avos_mp_video_abort(avos_mp_t *mp, avos_mp_video_t *video)
{
	video->abort = 1;
	return AVOS_ERR_OK;
}

void stream_un_pause_from_jni( STREAM *s, int was_paused );

int avos_mp_video_start(avos_mp_t *mp, avos_mp_video_t *video)
{
	// NEW: Unpause the subtitle engine
	if (video->s && video->s->sub_engine) sub_engine_set_paused((SUB_ENGINE*)video->s->sub_engine, 0);
	if (stream_is_paused(video->s))
		stream_un_pause_from_jni(video->s, 0);
	else if (!is_stream_pauseable(mp, video))
		stream_audio_unmute(video->s);
	return AVOS_ERR_OK;
}

int avos_mp_video_pause(avos_mp_t *mp, avos_mp_video_t *video)
{
	// NEW: Pause the subtitle engine
	if (video->s && video->s->sub_engine) sub_engine_set_paused((SUB_ENGINE*)video->s->sub_engine, 1);
	if (is_stream_pauseable(mp, video))
		stream_pause(video->s);
	else
		stream_audio_mute(video->s);
	return AVOS_ERR_OK;
}

int avos_mp_video_isplaying(avos_mp_t *mp, avos_mp_video_t *video, int *ret)
{
	*ret = !stream_is_paused(video->s);
	return AVOS_ERR_OK;
}

int avos_mp_video_seek(avos_mp_t *mp, avos_mp_video_t *video, uint32_t pos)
{
	if (is_stream_seekable(mp, video)) {
		int wind_start, stream_total = 0, dir;

		wind_start = stream_get_current_pos(video->s, &stream_total);
		dir = pos >= wind_start ? STREAM_SEEK_FORWARD : STREAM_SEEK_BACKWARD;
		serprintf("avos_mp_video:avos_mp_video_seek current_pos=%d to pos=%d direction=%d\n", wind_start, pos, dir);
		stream_seek_pos(video->s, pos, dir, STREAM_SEEK_RELAXED );
	}
	return AVOS_ERR_OK;
}

void avos_mp_video_supersede_seek_preview(avos_mp_video_t *video)
{
	if (video && video->s)
		stream_seek_preview_supersede(video->s);
}

int avos_mp_video_getpos(avos_mp_t *mp, avos_mp_video_t *video, uint32_t *ret)
{
	*ret = stream_get_current_time(video->s, &video->last_duration);
	is_stream_seekable(mp, video);
	is_stream_pauseable(mp, video);
	if (video->last_duration == 0) {
		int total, pos;
		pos = stream_get_current_pos(video->s, &total);
		avos_mp_sendevent(mp, MEDIA_RELATIVE_POSITION_UPDATE,
		    (uint32_t) ((double)(pos/(double)total) * 1000), 0);
	} else if (video->buffered_pos) {
		avos_mp_sendevent(mp, MEDIA_BUFFERING_UPDATE,
		    100 * stream_get_buffered_pos(video->s, NULL) / video->last_duration, 0);
	}
	return AVOS_ERR_OK;
}

int avos_mp_video_getduration(avos_mp_t *mp, avos_mp_video_t *video, uint32_t *ret)
{
	stream_get_current_time(video->s, &video->last_duration);
	*ret = video->last_duration;
	return AVOS_ERR_OK;
}

int avos_mp_video_getaudiosessionid(avos_mp_t *mp, avos_mp_video_t *video, uint32_t *ret)
{
	*ret = stream_get_audio_session_id(video->s);
	return AVOS_ERR_OK;
}

int avos_mp_video_setaudiotrack(avos_mp_t *mp, avos_mp_video_t *video, int track, int *ret)
{
	*ret = stream_set_audio_stream(video->s, track) == 0 ? 1 : 0;

	if (*ret && stream_get_current_audio_stream(video->s) != track)
		*ret = 0;
	return AVOS_ERR_OK;
}

int avos_mp_video_refreshaudiooutput(avos_mp_t *mp, avos_mp_video_t *video)
{
	return stream_refresh_audio_stream(video->s) == 0 ? AVOS_ERR_OK : AVOS_ERR;
}

int avos_mp_video_checksubtitles(avos_mp_t *mp, avos_mp_video_t *video)
{
	return stream_check_subtitles(video->s) == 0 ? AVOS_ERR_OK : AVOS_ERR;
}

int avos_mp_video_setsubtitletrack(avos_mp_t *mp, avos_mp_video_t *video, int track, int *ret)
{
	if (track < 0 || track >= video->s->av.subs_max) {
		video->send_sub = 0;
		*ret = 1;
		// Clear the screen if track is disabled
		if (video->s && video->s->sub_engine) sub_engine_close_track((SUB_ENGINE*)video->s->sub_engine);
	} else {
		video->send_sub = 1;
		*ret = stream_set_subtitle_stream(video->s, track) == 0 ? 1 : 0;
		// DO NOT open or close the track here!
		// stream_subtitle.c will natively open it when the first packet arrives.
	}
	return AVOS_ERR_OK;
}

int avos_mp_video_setsubtitledelay(avos_mp_t *mp, avos_mp_video_t *video, int delay)
{
	stream_set_subtitle_offset(video->s, delay + SUBTITLE_SEND_OFFSET);
	return AVOS_ERR_OK;
}

int avos_mp_video_setsubtitleratio(avos_mp_t *mp, avos_mp_video_t *video, uint32_t n, uint32_t d)
{
	stream_set_subtitle_ratio(video->s, n, d);
	return AVOS_ERR_OK;
}

int avos_mp_video_setaudiofilter(avos_mp_t *mp, avos_mp_video_t *video, int n, int night_on)
{
	// Pass user's audio boost and night mode settings directly to the filter
	// The audio filter chain (compress -> AC3 -> AGC) handles AC3 recoding correctly
	return stream_set_audio_filter_level(video->s, n, night_on) == 0 ?
		AVOS_ERR_OK : AVOS_ERR;
}

int avos_mp_video_setavdelay(avos_mp_t *mp, avos_mp_video_t *video, int delay)
{
	return stream_set_av_delay(video->s, delay) == 0 ? AVOS_ERR_OK : AVOS_ERR;
}

int avos_mp_video_setavspeed(avos_mp_t *mp, avos_mp_video_t *video, float speed)
{
	return stream_set_av_speed(video->s, speed) == 0 ? AVOS_ERR_OK : AVOS_ERR;
}

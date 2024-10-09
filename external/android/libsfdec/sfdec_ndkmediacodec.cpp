// Copyright 2017 Archos SA
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

#ifdef __ANDROID_API__
#undef __ANDROID_API__
#define __ANDROID_API__ 21
#endif

#include <media/NdkMediaCodec.h>

#include "sfdec_common.h"

typedef struct sfdec_mediacodec sfdec_priv_t;
#include "sfdec_priv.h"

#define DBG if (0)

#undef LOG
#define LOG(fmt, ...) do { \
    printf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
    fflush(stdout); \
} while (0)


struct sfdec_mediacodec
{
    ANativeWindow   *mNativeWindow;
    AMediaFormat    *mFormat;
    AMediaCodec     *mCodec;
    sfdec_codec_t codec;
    int32_t width;
    int32_t height;
    void *extradata;
    size_t extradata_size;
    int rotation;
    bool started;
    int flush;

    // the CLOCK_MONOTONIC value when we started/resumed playback
    int64_t start_monotonic;
    // the 1st frame timestamp since starting/resumed playback
    int64_t start_off;

    int64_t last_off;
    int64_t last_monotonic;
};

struct sfbuf
{
    size_t index;
    bool released;
    int64_t timestamp_us;
};

static void sfdec_destroy(sfdec_priv_t *sfdec);
static ssize_t sfdec_send_input2(sfdec_priv_t *sfdec, void *data, size_t size, int64_t time_us, int is_sync_frame, int wait, int flag);

#define CHECK(x) do { \
    if (!(x)) { \
        LOG("sfdec_init failed: %s", #x); \
        sfdec_destroy(sfdec); \
        return NULL; \
    } \
} while (0)
#define CHECK_STATUS(err) CHECK((err) == AMEDIA_OK)

static int init_renderer(sfdec_priv_t *sfdec)
{
    media_status_t err;
    int32_t width, height;
    AMediaFormat *format = AMediaCodec_getOutputFormat(sfdec->mCodec);

    if (format != NULL) {
        if (AMediaFormat_getInt32(format, "width", &width)) {
            LOG("width NOT changed: %d -> %d", sfdec->width, width);
            //sfdec->width = width;
        }
        if (AMediaFormat_getInt32(format, "height", &height)) {
            LOG("height NOT changed: %d -> %d", sfdec->height, height);
            //sfdec->height = height;
        }
    }

    return 0;
}

static int err_count;

static sfdec_priv_t *sfdec_init(sfdec_codec_t codec,
            sfdec_flags_t flags,
            int *width, int *height, int rotation,
            int64_t duration_us, int input_size,
            void *surface_handle,
            void *extradata, size_t extradata_size,
            int *pts_reorder, int sampleSize, int channels, int bitrate,
            int64_t codec_delay, int64_t seek_preroll)
{
    media_status_t err;
    const char *mime_type;
    unsigned int nb_csd = 0;

    mime_type = get_mimetype(codec);
    if (!mime_type)
        return NULL;

    DBG LOG("(NdkMediaCodec): %s with extradata size %d", mime_type, extradata_size);

    sfdec_priv_t *sfdec = new sfdec_priv_t();
    if (sfdec == NULL)
        return NULL;

    err_count = 0;
    sfdec->codec = codec;
    sfdec->rotation = rotation;
    sfdec->width = *width;
    sfdec->height = *height;
    sfdec->extradata = extradata;
    sfdec->extradata_size = extradata_size;
    sfdec->mNativeWindow = (ANativeWindow *)surface_handle;
    sfdec->start_off = 0;
    sfdec->start_monotonic = 0;

    DBG LOG("sfdec->mCodec %d sfdec->mCodec %d", sfdec->mCodec, sfdec->mFormat);

    sfdec->mCodec = AMediaCodec_createDecoderByType(mime_type);
    CHECK(sfdec->mCodec != NULL);

    sfdec->mFormat = AMediaFormat_new();
    CHECK(sfdec->mFormat != NULL);

    AMediaFormat_setString(sfdec->mFormat, "mime", mime_type);
    if (duration_us > 0)
        AMediaFormat_setInt64(sfdec->mFormat, "durationUs", duration_us);
    AMediaFormat_setInt32(sfdec->mFormat, "width", sfdec->width);
    AMediaFormat_setInt32(sfdec->mFormat, "height", sfdec->height);
    if (input_size > 0)
        AMediaFormat_setInt32(sfdec->mFormat, "max-input-size", input_size);

    err = AMediaCodec_configure(sfdec->mCodec, sfdec->mFormat, sfdec->mNativeWindow, NULL, 0);
    CHECK_STATUS(err);

    err = AMediaCodec_start(sfdec->mCodec);
    CHECK_STATUS(err);
    sfdec->started = true;
    sfdec->flush = 1;

    return sfdec;
}

#undef CHECK
#undef CHECK_STATUS

#define CHECK(x) do { \
    if (!(x)) { \
        LOG("%s failed: %s", __FUNCTION__, #x); \
        return -1; \
    } \
} while (0)
#define CHECK_STATUS(err) CHECK((err) == AMEDIA_OK)

static void sfdec_destroy(sfdec_priv_t *sfdec)
{
    if (sfdec->mCodec != NULL)
        AMediaCodec_delete(sfdec->mCodec);
    if (sfdec->mFormat != NULL)
        AMediaFormat_delete(sfdec->mFormat);
    delete sfdec;
}

static int sfdec_start(sfdec_priv_t *sfdec)
{
    if (!sfdec->started) {
        media_status_t err = AMediaCodec_start(sfdec->mCodec);
        CHECK_STATUS(err);
        sfdec->started = true;
    }
    return 0;
}

static int sfdec_stop(sfdec_priv_t *sfdec)
{
    if (sfdec->started) {
        media_status_t err = AMediaCodec_stop(sfdec->mCodec);
        CHECK_STATUS(err);
        sfdec->started = false;
    }
    return 0;
}

static ssize_t sfdec_send_input(sfdec_priv_t *sfdec, void *data, size_t size, int64_t time_us, int is_sync_frame, int wait)
{
    return sfdec_send_input2(sfdec, data, size, time_us, is_sync_frame, wait, 0);
}

static ssize_t sfdec_send_input2(sfdec_priv_t *sfdec, void *data, size_t size, int64_t time_us, int is_sync_frame, int wait, int flag)
{
    ssize_t index;
    media_status_t err;
    size_t bufsize = 0;
    uint8_t *buf = NULL;

    if (sfdec->flush) {
	sfdec->flush = 0;
	if (sfdec->extradata && (sfdec->codec == SFDEC_VIDEO_AVC || sfdec->codec == SFDEC_VIDEO_HEVC || sfdec->codec == SFDEC_VIDEO_WMV || sfdec->codec == SFDEC_VIDEO_DOLBY_VISION) )
            sfdec_send_input2(sfdec, sfdec->extradata, sfdec->extradata_size, 0, 0, 1, 2/* BUFFER_FLAG_CODECCONFIG (not exported)*/);
    }

    index = AMediaCodec_dequeueInputBuffer(sfdec->mCodec, wait ? -1ll : 0);
    if (index < 0)
        return 0;
    buf = AMediaCodec_getInputBuffer(sfdec->mCodec, index, &bufsize);

    if (size > bufsize)
        size = bufsize;

    memcpy(buf, data, size);

    DBG LOG("queueInputBuffer: index %d size %d time %lld flag %d\n", index, size, time_us, flag);
    err = AMediaCodec_queueInputBuffer(sfdec->mCodec,
            index,
            0,
            size,
            time_us,
            flag);
    CHECK_STATUS(err);

    return size;
}

static int sfdec_flush(sfdec_priv_t *sfdec)
{
    sfdec->flush = 1;
    media_status_t err = AMediaCodec_flush(sfdec->mCodec);
    CHECK_STATUS(err);
    err_count = 0;

    return 0;
}

static int sfdec_stop_input(sfdec_priv_t *sfdec)
{
    return 0;
}

static int sfdec_read(sfdec_priv_t *sfdec, int64_t seek, sfdec_read_out_t *read_out)
{
    ssize_t index;

    if (!read_out)
        return -1;

    read_out->flag = SFDEC_READ_INVALID;

    for (;;) {
        AMediaCodecBufferInfo info;
        index = AMediaCodec_dequeueOutputBuffer(sfdec->mCodec, &info, -1);

        if (index >= 0) {
            err_count = 0;
            sfbuf_t *sfbuf = (sfbuf_t*) calloc(1, sizeof(sfbuf_t));
            if (sfbuf == NULL)
                return -1;
            sfbuf->index = index;
            sfbuf->released = false;
            sfbuf->timestamp_us = info.presentationTimeUs;
            read_out->flag |= SFDEC_READ_BUF;
            read_out->buf.sfbuf = sfbuf;
            read_out->buf.time_us = info.presentationTimeUs;
            DBG LOG("buf: %d / time: %lld", index, info.presentationTimeUs);
            return 0;
        } else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {

	    // here we init renderer based on codec information that could be erroneous and yield to incorrect AR
            if (init_renderer(sfdec))
                continue;
            read_out->flag |= SFDEC_READ_SIZE;
            read_out->size.width = sfdec->width;
            read_out->size.height = sfdec->height;
            read_out->size.interlaced = 0;
            LOG("INFO_FORMAT_CHANGED: %dx%d", sfdec->width, sfdec->height);
            return 0;
        } else if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            DBG LOG("INFO_OUTPUT_BUFFERS_CHANGED");
        } else if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            DBG LOG("mCodec->dequeueOutputBuffer returned -EAGAIN");
            return 0;
        } else if (index == -10000) { // 0xFFFFD8F0 but works in 64b
            if (sfdec->flush) {
                LOG("mCodec->dequeueOutputBuffer returned -10000 while flushing IGNORE!");
            	return 0;
            } else {
                LOG("mCodec->dequeueOutputBuffer returned -10000");
                return -1;
            }
        } else {
            err_count++;
            LOG("mCodec->dequeueOutputBuffer returned: %zx", index);
            if( err_count > 100 ) {
                LOG("we had enough!");
                return -1;
            }
            return 0;
        }
    }
}

static int sfdec_buf_render(sfdec_priv_t *sfdec, sfbuf_t *sfbuf, int render, int asap)
{
    media_status_t err;
    if( render ) {
        if (asap) {
            err = AMediaCodec_releaseOutputBuffer(sfdec->mCodec, sfbuf->index, true);
        } else {
            int64_t now_ts;

            {
                struct timespec now;
                int ret = clock_gettime(CLOCK_MONOTONIC, &now);
                now_ts = now.tv_sec * 1000000000LL + now.tv_nsec;
            }

            int64_t off_delta = sfbuf->timestamp_us * 1000LL - sfdec->last_off;
            if (
                    !sfdec->start_off || //Got reset
                    (now_ts - sfdec->last_monotonic) > 500*1000LL*1000LL || //If we had no frame since the last 500ms, user did pause/resume
                    (off_delta < -500*1000LL*1000LL || off_delta > 500*1000LL*1000LL) // If distance between two frames is >500ms, that's a seek
                    ) {
                // We store the first frame (its realtime timestamp -- now & codec timestamp)
                sfdec->start_monotonic = now_ts + 100 * 1000LL * 1000L; // Start in 100ms
                sfdec->start_off = sfbuf->timestamp_us * 1000LL;
                // display first frame there asap
                asap = 1;
            }
            // Compute the realtime timestamp to display the frame based on timestamp from codec, and the info we stored when we started
            int64_t ts = sfbuf->timestamp_us * 1000LL - sfdec->start_off + sfdec->start_monotonic;

            if (asap)
                DBG LOG("Scheduling frame in a jiffy");
            else
                DBG LOG("Scheduling frame in %lld", ts - now_ts);

            sfdec->last_monotonic = now_ts;
            sfdec->last_off = sfbuf->timestamp_us * 1000LL;

            if (asap)
                err = AMediaCodec_releaseOutputBuffer(sfdec->mCodec, sfbuf->index, true);
            else
                err = AMediaCodec_releaseOutputBufferAtTime(sfdec->mCodec, sfbuf->index, ts);
        }
    } else {
        err = AMediaCodec_releaseOutputBuffer(sfdec->mCodec, sfbuf->index, false);
    }
    CHECK_STATUS(err);
    sfbuf->released = true;
    return 0;
}

static int sfdec_buf_release(sfdec_priv_t *sfdec, sfbuf_t *sfbuf)
{
    media_status_t err = AMEDIA_OK;
    if (!sfbuf->released)
        err = AMediaCodec_releaseOutputBuffer(sfdec->mCodec, sfbuf->index, false);
    free(sfbuf);
        
    return err == AMEDIA_OK ? 0 : -1;
}

static int sfdec_reset_ts(sfdec_priv_t *sfdec)
{
    sfdec->start_off = 0;
    sfdec->start_monotonic = 0;
    return 0;
}

sfdec_itf_t sfdec_itf_mediacodec = {
    "MediaCodec",
    sfdec_init,
    sfdec_destroy,
    sfdec_start,
    sfdec_stop,
    sfdec_send_input,
    sfdec_flush,
    sfdec_stop_input,
    sfdec_read,
    sfdec_buf_render,
    sfdec_buf_release,
    sfdec_reset_ts,
};

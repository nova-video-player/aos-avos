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

#ifdef __ANDROID_API__
#undef __ANDROID_API__
#define __ANDROID_API__ 21
#endif

#include <media/NdkMediaCodec.h>

#include "sfdec_common.h"

typedef struct dec_audio_mediacodec sfdec_priv_t;
#include "sfdec_priv.h"

#define DBG if (0)

#undef LOG
#define LOG(fmt, ...) do { \
    printf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
    fflush(stdout); \
} while (0)


struct dec_audio_mediacodec
{
    ANativeWindow   *mNativeWindow;
    AMediaFormat    *mFormat;
    AMediaCodec     *mCodec;
    int32_t width;
    int32_t height;
    int rotation;
    bool started;
    int flush;
};

struct sfbuf
{
    size_t index;
    bool released;
};

static void dec_audio_destroy(sfdec_priv_t *sfdec);
static ssize_t dec_audio_send_input2(sfdec_priv_t *sfdec, void *data, size_t size, int64_t time_us, int is_sync_frame, int wait, int flag);

#define CHECK(x) do { \
    if (!(x)) { \
        LOG("dec_audio_init failed: %s", #x); \
        dec_audio_destroy(sfdec); \
        return NULL; \
    } \
} while (0)
#define CHECK_STATUS(err) CHECK((err) == AMEDIA_OK)

static int init_renderer(sfdec_priv_t *sfdec)
{
    media_status_t err;
    int32_t width, height,channels,samplesPerSec;
    AMediaFormat *format = AMediaCodec_getOutputFormat(sfdec->mCodec);

    if (format != NULL) {
        if (AMediaFormat_getInt32(format, "width", &width)) {
            LOG("width changed: %d -> %d", sfdec->width, width);
            sfdec->width = width;
        }
        if (AMediaFormat_getInt32(format, "height", &height)) {
            LOG("height changed: %d -> %d", sfdec->height, height);
            sfdec->height = height;
        }
    }

    return 0;
}

static sfdec_priv_t *dec_audio_init(sfdec_codec_t codec,
            sfdec_flags_t flags,
            int *width, int *height, int rotation,
            int64_t duration_us, int input_size,
            void *surface_handle,
            void *extradata, size_t extradata_size,
            int *pts_reorder, int samplesPerSec, int channels, int bitrate,
            int64_t codec_delay, int64_t seek_preroll)
{
    media_status_t err;
    const char *mime_type;
    unsigned int nb_csd = 0;

    mime_type = get_mimetype(codec);
    if (!mime_type)
        return NULL;


    sfdec_priv_t *sfdec = new sfdec_priv_t();
    if (sfdec == NULL)
        return NULL;

    sfdec->mFormat = NULL;
    sfdec->mCodec = AMediaCodec_createDecoderByType(mime_type);
    CHECK(sfdec->mCodec != NULL);

    sfdec->mFormat = AMediaFormat_new();
    CHECK(sfdec->mFormat != NULL);

    AMediaFormat_setString(sfdec->mFormat, "mime", mime_type);
    if (duration_us > 0)
        AMediaFormat_setInt64(sfdec->mFormat, "durationUs", duration_us);

    if (input_size > 0)
        AMediaFormat_setInt32(sfdec->mFormat, "max-input-size", input_size);
    AMediaFormat_setInt32(sfdec->mFormat, "sample-rate", samplesPerSec);
    AMediaFormat_setInt32(sfdec->mFormat, "channel-count", channels);
    //AMediaFormat_setInt32(sfdec->mFormat, "bitrate", bitrate);

   if ( extradata ) {
    AMediaFormat_setBuffer(sfdec->mFormat, "csd-0", extradata, extradata_size);
    if (codec == SFDEC_AUDIO_OPUS) {
        AMediaFormat_setBuffer(sfdec->mFormat, "csd-1", &codec_delay, sizeof(codec_delay));
	AMediaFormat_setBuffer(sfdec->mFormat, "csd-2", &seek_preroll, sizeof(seek_preroll));
    	DBG LOG("(NdkMediaCodec): %s with extradata size %d", mime_type, extradata_size);
	DBG LOG("(NdkMediaCodec): codec_delay %lld seek_preroll %lld", codec_delay, seek_preroll);
        }
    }

    err = AMediaCodec_configure(sfdec->mCodec, sfdec->mFormat,NULL, NULL, 0);
    CHECK_STATUS(err);

    err = AMediaCodec_start(sfdec->mCodec);
    CHECK_STATUS(err);
    sfdec->started = true;

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

static void dec_audio_destroy(sfdec_priv_t *sfdec)
{
    if (sfdec->mCodec != NULL)
        AMediaCodec_delete(sfdec->mCodec);
    if (sfdec->mFormat != NULL)
        AMediaFormat_delete(sfdec->mFormat);
    delete sfdec;
}

static int dec_audio_start(sfdec_priv_t *sfdec)
{
    if (!sfdec->started) {
        media_status_t err = AMediaCodec_start(sfdec->mCodec);
        CHECK_STATUS(err);
        sfdec->started = true;
    }
    return 0;
}

static int dec_audio_stop(sfdec_priv_t *sfdec)
{
    if (sfdec->started) {
        media_status_t err = AMediaCodec_stop(sfdec->mCodec);
        CHECK_STATUS(err);
        sfdec->started = false;
    }
    return 0;
}

static ssize_t dec_audio_send_input(sfdec_priv_t *sfdec, void *data, size_t size, int64_t time_us, int is_sync_frame, int wait)
{
    return dec_audio_send_input2(sfdec, data, size, time_us, is_sync_frame, wait, 0);
}

static ssize_t dec_audio_send_input2(sfdec_priv_t *sfdec, void *data, size_t size, int64_t time_us, int is_sync_frame, int wait, int flag)
{
    ssize_t index;
    media_status_t err;
    size_t bufsize = 0;
    uint8_t *buf = NULL;

    sfdec->flush = 0;

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

static int dec_audio_flush(sfdec_priv_t *sfdec)
{
    sfdec->flush = 1;
    media_status_t err = AMediaCodec_flush(sfdec->mCodec);
    CHECK_STATUS(err);

    return 0;
}

static int dec_audio_stop_input(sfdec_priv_t *sfdec)
{
    return 0;
}

static int dec_audio_read(sfdec_priv_t *sfdec, int64_t seek, sfdec_read_out_t *read_out)
{
    ssize_t index;

    if (!read_out)
        return -1;

    read_out->flag = SFDEC_READ_INVALID;

    for (;;) {
        AMediaCodecBufferInfo info;
        index = AMediaCodec_dequeueOutputBuffer(sfdec->mCodec, &info, INT64_C(100));

        if (index >= 0) {
            sfbuf_t *sfdec_buf = (sfbuf_t*) calloc(1, sizeof(sfbuf_t));
            if (sfdec_buf == NULL)
                return -1;
	    size_t out_size;
            sfdec_buf->index = index;
            sfdec_buf->released = false;

	    read_out->buf.out = AMediaCodec_getOutputBuffer(sfdec->mCodec,index, &out_size);
	    
	    DBG LOG("sfdec->mCodec %p, index %d size %u, info.size %d", sfdec->mCodec, index, out_size, info.size);
	
	    read_out->buf.out_size= info.size;
	       
            read_out->flag |= SFDEC_READ_BUF;
	    int32_t channels,samplesPerSec,bitRate;
	if(AMediaFormat_getInt32(AMediaCodec_getOutputFormat(sfdec->mCodec), "channel-count", &channels))
	    read_out->channels = channels;
	if(AMediaFormat_getInt32(AMediaCodec_getOutputFormat(sfdec->mCodec), "sample-rate", &samplesPerSec))
	    read_out->samplesPerSec = samplesPerSec;
	if(AMediaFormat_getInt32(AMediaCodec_getOutputFormat(sfdec->mCodec), "bitrate", &bitRate))
	    read_out->bitRate = bitRate;
	    DBG LOG("\n\rmediacodec_audio_codec_decode set ead_out->flag %d",read_out->flag);
            read_out->buf.sfbuf = sfdec_buf;
            read_out->buf.time_us = info.presentationTimeUs;
            DBG LOG("buf: %zu / time: % " PRId64, index, info.presentationTimeUs);
            return 0;
        } else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
	  
            if (init_renderer(sfdec))
                continue;
            read_out->flag |= SFDEC_READ_SIZE;
            read_out->size.width = sfdec->width;
            read_out->size.height = sfdec->height;
            read_out->size.interlaced = 0;
	    
            DBG LOG("INFO_FORMAT_CHANGED: %dx%d", sfdec->width, sfdec->height);
            return 1;
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
            LOG("mCodec->dequeueOutputBuffer returned: %X", index);
            return 0;
        }
    }
}

static int sfdec_buf_render(sfdec_priv_t *sfdec, sfbuf_t *sfdec_buf, int render)
{
    media_status_t err;
    if( render ) {
        err = AMediaCodec_releaseOutputBuffer(sfdec->mCodec, sfdec_buf->index, true);
    } else {
        err = AMediaCodec_releaseOutputBuffer(sfdec->mCodec, sfdec_buf->index, false);
    }
    CHECK_STATUS(err);
    sfdec_buf->released = true;
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

sfdec_itf_t dec_audio_mediacodec = {
    "MediaCodec",
    dec_audio_init,
    dec_audio_destroy,
    dec_audio_start,
    dec_audio_stop,
    dec_audio_send_input,
    dec_audio_flush,
    dec_audio_stop_input,
    dec_audio_read,
    sfdec_buf_render,
    sfdec_buf_release,
};

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

#include <media/ICrypto.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaErrors.h>

#include <gui/Surface.h>

typedef struct sfdec_mediacodec sfdec_priv_t;
#include "sfdec_priv.h"

using namespace android;

#define DLHELPER_HEADER "dlhelper_mc.h"
#include "dlhelper.h"

#include "sfdec_common.h"
//#include "dump.h"

#define DBG if (0)

#undef LOG
#define LOG(fmt, ...) do { \
    printf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
    fflush(stdout); \
} while (0)


#define ALOOPER_SIZE 256 // 44
#define AMESSAGE_SIZE 4096 // 1560
#define ABUFFER_SIZE 256 // 40
#define VECTORIMPL_SIZE 256 // 20

static ALooper* ALooper_create()
{
    void *obj = calloc(1, ALOOPER_SIZE);
    if (!obj)
        return NULL;
    dl_mc.ALooper_ctor(obj);
    return (ALooper *)obj;
}

static AMessage* AMessage_create(uint32_t what, ALooper::handler_id target)
{
    void *obj = calloc(1, AMESSAGE_SIZE);
    if (!obj)
        return NULL;
    dl_mc.AMessage_ctor(obj, what, target);
    return (AMessage *)obj;
}

static ABuffer* ABuffer_create(size_t capacity)
{
    void *obj = calloc(1, ABUFFER_SIZE);
    if (!obj)
        return NULL;
    dl_mc.ABuffer_ctor(obj, capacity);
    return (ABuffer *)obj;
}

struct sfdec_mediacodec
{
    sp<ANativeWindow>   mNativeWindow;
    sp<ALooper>         mCodecLooper;
    sp<MediaCodec>      mCodec;
    Vector<sp<ABuffer> > mInputBuffers;
    Vector<sp<ABuffer> > mOutputBuffers;
    int32_t width;
    int32_t height;
    bool started;
};

struct sfbuf
{
    size_t index;
    bool released;
};

static inline uint16_t U16_AT(const uint8_t *ptr)
{
    return ptr[0] << 8 | ptr[1];
}

/*
 * from media/libstagefright/Utils.cpp
 */
static unsigned int set_avc_config(void *data, size_t size, sp<AMessage> &msg)
{
#define CHECK(x) do { \
    if ((!x)) \
        return 0; \
} while(0)
#define CHECK_EQ(a, b) do { \
    if ((a) != (b)) \
        return 0; \
} while(0)
    // Parse the AVCDecoderConfigurationRecord

    unsigned int nbCSD = 0;
    const uint8_t *ptr = (const uint8_t *)data;

    CHECK(size >= 7);
    CHECK_EQ((unsigned)ptr[0], 1u);  // configurationVersion == 1
    uint8_t profile = ptr[1];
    uint8_t level = ptr[3];

    // There is decodable content out there that fails the following
    // assertion, let's be lenient for now...
    // CHECK((ptr[4] >> 2) == 0x3f);  // reserved

    size_t lengthSize = 1 + (ptr[4] & 3);

    // commented out check below as H264_QVGA_500_NO_AUDIO.3gp
    // violates it...
    // CHECK((ptr[5] >> 5) == 7);  // reserved

    size_t numSeqParameterSets = ptr[5] & 31;

    ptr += 6;
    size -= 6;

    sp<ABuffer> buffer = ABuffer_create(1024);
    dl_mc.ABuffer_setRange(buffer.get(), 0, 0);

    for (size_t i = 0; i < numSeqParameterSets; ++i) {
        CHECK(size >= 2);
        size_t length = U16_AT(ptr);

        ptr += 2;
        size -= 2;

        CHECK(size >= length);

        memcpy(buffer->data() + buffer->size(), "\x00\x00\x00\x01", 4);
        memcpy(buffer->data() + buffer->size() + 4, ptr, length);
        dl_mc.ABuffer_setRange(buffer.get(), 0, buffer->size() + 4 + length);

        ptr += length;
        size -= length;
    }

    sp<AMessage> meta = dl_mc.ABuffer_meta(buffer.get());

    dl_mc.AMessage_setInt32(meta.get(), "csd", true);
    dl_mc.AMessage_setInt64(meta.get(), "timeUs", 0);

//    LOG("(set_avc_config): csd-0");
//    Dump( (unsigned char *)buffer->data(), buffer->size() );

    dl_mc.AMessage_setBuffer(msg.get(), "csd-0", buffer);
    nbCSD++;

    buffer = ABuffer_create(1024);
    dl_mc.ABuffer_setRange(buffer.get(), 0, 0);

    CHECK(size >= 1);
    size_t numPictureParameterSets = *ptr;
    ++ptr;
    --size;

    for (size_t i = 0; i < numPictureParameterSets; ++i) {
        CHECK(size >= 2);
        size_t length = U16_AT(ptr);

        ptr += 2;
        size -= 2;

        CHECK(size >= length);

        memcpy(buffer->data() + buffer->size(), "\x00\x00\x00\x01", 4);
        memcpy(buffer->data() + buffer->size() + 4, ptr, length);
        dl_mc.ABuffer_setRange(buffer.get(), 0, buffer->size() + 4 + length);

        ptr += length;
        size -= length;
    }
    
    meta = dl_mc.ABuffer_meta(buffer.get());

    dl_mc.AMessage_setInt32(meta.get(), "csd", true);
    dl_mc.AMessage_setInt64(meta.get(), "timeUs", 0);

//    LOG("(set_avc_config): csd-1");
//    Dump( (unsigned char *)buffer->data(), buffer->size() );

    dl_mc.AMessage_setBuffer(msg.get(), "csd-1", buffer);
    nbCSD++;
    return nbCSD;
#undef CHECK
#undef CHECK_EQ
}

/*
 * from media/libstagefright/Utils.cpp
 */
static unsigned int set_buffer_config(void *data, size_t size, sp<AMessage> &msg)
{
    sp<ABuffer> buffer = ABuffer_create(size);

    memcpy(buffer->data(), data, size);

    sp<AMessage> meta = dl_mc.ABuffer_meta(buffer.get());

    dl_mc.AMessage_setInt32(meta.get(), "csd", true);
    dl_mc.AMessage_setInt64(meta.get(), "timeUs", 0);

    dl_mc.AMessage_setBuffer(msg.get(), "csd-0", buffer);
    return 1;
}

static void sfdec_destroy(sfdec_priv_t *sfdec);
#define CHECK(x) do { \
    if (!(x)) { \
        LOG("sfdec_init failed: %s", #x); \
        sfdec_destroy(sfdec); \
        return NULL; \
    } \
} while (0)
#define CHECK_STATUS(err) CHECK((err) == OK)

static int init_renderer(sfdec_priv_t *sfdec)
{
    status_t err;
    int32_t width, height;
    sp<AMessage> format;

    if (dl_mc.MediaCodec_getOutputFormat(sfdec->mCodec.get(), &format) == OK) {
        if (dl_mc.AMessage_findInt32(format.get(), "width", &width)) {
            LOG("width changed: %d -> %d", sfdec->width, width);
            sfdec->width = width;
        }
        if (dl_mc.AMessage_findInt32(format.get(), "height", &height)) {
            LOG("height changed: %d -> %d", sfdec->height, height);
            sfdec->height = height;
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
    status_t err;
    const char *mime_type;
    unsigned int nb_csd = 0;

    mime_type = get_mimetype(codec);
    if (!mime_type)
        return NULL;

    LOG("(MediaCodec): %s", mime_type);
    LOG("(MediaCodec): extradata_size %d", extradata_size);
//    Dump( (unsigned char *)extradata, extradata_size );

    if (dlhelper_mc_init())
        return NULL;

    sfdec_priv_t *sfdec = new sfdec_priv_t();
    if (sfdec == NULL)
        return NULL;

    err_count = 0;
    sfdec->width = *width;
    sfdec->height = *height;
    sfdec->mNativeWindow = (ANativeWindow *)surface_handle;
    sfdec->mCodecLooper = ALooper_create();
    dl_mc.ALooper_start(sfdec->mCodecLooper.get(), false, false, PRIORITY_DEFAULT);

    sfdec->mCodec = dl_mc.MediaCodec_CreateByType(sfdec->mCodecLooper, mime_type, false);
    CHECK(sfdec->mCodec != NULL);

    sp<AMessage> format = AMessage_create(0, 0);
    dl_mc.AMessage_setString(format.get(), "mime", mime_type, -1);
    if (duration_us > 0)
        dl_mc.AMessage_setInt64(format.get(), "durationUs", duration_us);
    dl_mc.AMessage_setInt32(format.get(), "width", sfdec->width);
    dl_mc.AMessage_setInt32(format.get(), "height", sfdec->height);
    if (input_size > 0)
        dl_mc.AMessage_setInt32(format.get(), "max-input-size", input_size);
    if (extradata) {
        if (codec == SFDEC_VIDEO_AVC)
            nb_csd = set_avc_config(extradata, extradata_size, format);
        else if (codec == SFDEC_VIDEO_MPEG4 || codec == SFDEC_VIDEO_HEVC)
            nb_csd = set_buffer_config(extradata, extradata_size, format);
    }
    sp<Surface> nativeSurface(static_cast<android::Surface *>(sfdec->mNativeWindow.get()));

    err = dl_mc.MediaCodec_configure(sfdec->mCodec.get(), format, nativeSurface, NULL, 0);
    CHECK_STATUS(err);

    err = dl_mc.MediaCodec_start(sfdec->mCodec.get());
    CHECK_STATUS(err);
    sfdec->started = true;
    err = dl_mc.MediaCodec_getInputBuffers(sfdec->mCodec.get(), &sfdec->mInputBuffers);
    CHECK_STATUS(err);
    err = dl_mc.MediaCodec_getOutputBuffers(sfdec->mCodec.get(), &sfdec->mOutputBuffers);
    CHECK_STATUS(err);

    sp<ABuffer> srcBuffer;
    size_t j = 0;
    char csdStr[strlen("csd-XXX") + 1];
    for (j = 0; j < nb_csd;j++) {
        size_t index;

        snprintf(csdStr, strlen("csd-XXX"), "csd-%d", j);
        if (!dl_mc.AMessage_findBuffer(format.get(), csdStr, &srcBuffer))
            break;
        LOG("queue: %s", csdStr);

        err = dl_mc.MediaCodec_dequeueInputBuffer(sfdec->mCodec.get(), &index, -1ll);
        CHECK_STATUS(err);

        const sp<ABuffer> &dstBuffer = sfdec->mInputBuffers.itemAt(index);

        CHECK(srcBuffer->size() < dstBuffer->capacity());
        dl_mc.ABuffer_setRange(dstBuffer.get(), 0, srcBuffer->size());
        memcpy(dstBuffer->data(), srcBuffer->data(), srcBuffer->size());

        err = dl_mc.MediaCodec_queueInputBuffer(sfdec->mCodec.get(),
                index,
                0,
                dstBuffer->size(),
                0ll,
                MediaCodec::BUFFER_FLAG_CODECCONFIG,
                NULL);
        CHECK_STATUS(err);
    }
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
#define CHECK_STATUS(err) CHECK((err) == OK)

static void sfdec_destroy(sfdec_priv_t *sfdec)
{
    if (sfdec->mCodec != NULL)
        dl_mc.MediaCodec_release(sfdec->mCodec.get());
    sfdec->mNativeWindow.clear();
    dl_mc.ALooper_stop(sfdec->mCodecLooper.get());
    sfdec->mCodecLooper.clear();
    sfdec->mCodec.clear();
    delete sfdec;
}

static int sfdec_start(sfdec_priv_t *sfdec)
{
    if (!sfdec->started) {
        status_t err = dl_mc.MediaCodec_start(sfdec->mCodec.get());
        CHECK_STATUS(err);
        sfdec->started = true;
    }
    return 0;
}

static int sfdec_stop(sfdec_priv_t *sfdec)
{
    if (sfdec->started) {
        status_t err = dl_mc.MediaCodec_stop(sfdec->mCodec.get());
        CHECK_STATUS(err);
        sfdec->started = false;
    }
    return 0;
}

static ssize_t sfdec_send_input(sfdec_priv_t *sfdec, void *data, size_t size, int64_t time_us, int is_sync_frame, int wait)
{
    size_t index;
    status_t err;

    err = dl_mc.MediaCodec_dequeueInputBuffer(sfdec->mCodec.get(), &index, wait ? -1ll : 0);
    if (err == -EAGAIN)
        return 0;
    else if (err != 0)
        return -1;

    const sp<ABuffer> &dstBuffer = sfdec->mInputBuffers.itemAt(index);
    if (size > dstBuffer->capacity())
        size = dstBuffer->capacity();

    dl_mc.ABuffer_setRange(dstBuffer.get(), 0, size);
    memcpy(dstBuffer->data(), data, size);

    err = dl_mc.MediaCodec_queueInputBuffer(sfdec->mCodec.get(),
            index,
            dstBuffer->offset(),
            dstBuffer->size(),
            time_us,
            0,
            NULL);
    CHECK_STATUS(err);

    return size;
}

static int sfdec_flush(sfdec_priv_t *sfdec)
{
    status_t err;
    err = dl_mc.MediaCodec_flush(sfdec->mCodec.get());
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
    status_t err;
    size_t index, offset, size;
    int64_t presentationTimeUs;
    uint32_t flags;

    if (!read_out)
        return -1;

    read_out->flag = SFDEC_READ_INVALID;

    for (;;) {
        err = dl_mc.MediaCodec_dequeueOutputBuffer(sfdec->mCodec.get(),
                &index,
                &offset,
                &size,
                &presentationTimeUs,
                &flags,
                -1);

        if (err == INFO_FORMAT_CHANGED) {

            if (init_renderer(sfdec))
                continue;
            read_out->flag |= SFDEC_READ_SIZE;
            read_out->size.width = sfdec->width;
            read_out->size.height = sfdec->height;
            read_out->size.interlaced = 0;
            DBG LOG("INFO_FORMAT_CHANGED: %dx%d", sfdec->width, sfdec->height);
            return 0;
        } else if (err == INFO_OUTPUT_BUFFERS_CHANGED) {
            DBG LOG("INFO_OUTPUT_BUFFERS_CHANGED");
            err = dl_mc.MediaCodec_getOutputBuffers(sfdec->mCodec.get(), &sfdec->mOutputBuffers);
            CHECK_STATUS(err);
        } else if (err == OK) {
	    err_count = 0;

            sfbuf_t *sfbuf = (sfbuf_t*) calloc(1, sizeof(sfbuf_t));
            if (sfbuf == NULL)
                return -1;
            sfbuf->index = index;
            sfbuf->released = false;
            read_out->flag |= SFDEC_READ_BUF;
            read_out->buf.sfbuf = sfbuf;
            read_out->buf.time_us = presentationTimeUs;
            DBG LOG("buf: %d / time: %lld", index, presentationTimeUs);
            return 0;
        } else if (err == -EAGAIN) {
            DBG LOG("mCodec->dequeueOutputBuffer returned -EAGAIN");
            return 0;
        } else {
	    err_count++;
	    LOG("mCodec->dequeueOutputBuffer returned: %d", err);
            if( err_count > 100 ) {
	        LOG("f*ck it, we had enough!");
	    	return -1;
	    }
	    return 0;
        }
    }
}

static int sfdec_buf_render(sfdec_priv_t *sfdec, sfbuf_t *sfbuf, int render)
{
    if( !render )
        return 0;
	
    status_t err;
    err = dl_mc.MediaCodec_renderOutputBufferAndRelease(sfdec->mCodec.get(), sfbuf->index);
    CHECK_STATUS(err);
    sfbuf->released = true;
    return 0;
}

static int sfdec_buf_release(sfdec_priv_t *sfdec, sfbuf_t *sfbuf)
{
    status_t err = 0;
    if (!sfbuf->released)
        err = dl_mc.MediaCodec_releaseOutputBuffer(sfdec->mCodec.get(), sfbuf->index);
    free(sfbuf);
        
    return err == 0 ? 0 : -1;
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
};

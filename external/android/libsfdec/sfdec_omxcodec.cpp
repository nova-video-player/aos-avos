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

#include <binder/IPCThreadState.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/OMXCodec.h>

#include <system/window.h>

#include <dlfcn.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/queue.h>
#include <unistd.h>

#include "sfdec_common.h"

typedef struct sfdec_omxcodec sfdec_priv_t;
#include "sfdec_priv.h"

using namespace android;

#define DLHELPER_HEADER "dlhelper_oc.h"
#include "dlhelper.h"

#define METADATA_SIZE 256 // 28
#define MEDIABUFFER_SIZE 256 // 28
#define SOFTWARERENDERER_SIZE 512 // 28

static inline MetaData* MetaData_Create()
{
    void *obj = calloc(1, METADATA_SIZE);
    if (!obj)
        return NULL;
    dl_oc.Metadata_ctor(obj);
    return (MetaData *)obj;
}

static inline MediaBuffer* MediaBuffer_Create(size_t size)
{
    void *obj = calloc(1, MEDIABUFFER_SIZE);
    if (!obj)
        return NULL;
    dl_oc.MediaBuffer_ctor(obj, size);
    return (MediaBuffer *)obj;
}

static inline void* SoftwareRenderer_Create(const sp<ANativeWindow> &nativeWindow, const sp<MetaData> &meta)
{
    void *obj = calloc(1, SOFTWARERENDERER_SIZE);
    if (!obj)
        return NULL;
    dl_oc.SoftwareRenderer_ctor(obj, nativeWindow, meta);
    return obj;
}

static inline void SoftwareRenderer_Destroy(void *obj)
{
    dl_oc.SoftwareRenderer_dtor(obj);
    free(obj);
}

#define DBG if (0)

#undef LOG
#define LOG(fmt, ...) do { \
    printf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
    fflush(stdout); \
} while (0)

#define FIND_INT32(meta, key, ptr, def) do { \
    if (!(meta)->findInt32((key), (ptr))) \
        *(ptr) = (def); \
} while (0)

#define FIND_INT64(meta, key, ptr, def) do { \
    if (!(meta)->findInt64((key), (ptr))) \
        *(ptr) = (def); \
} while (0)

#define FIND_STRING(meta, key, ptr) do { \
    if (!(meta)->findCString((key), (ptr))) \
        *(ptr) = NULL; \
} while (0)

enum {
    kKeyBufferLayout = 'lout',
};

class AvosSource;
class SfdecRenderer;

struct sfbuf;

class BufferQueue {
public:
    struct Elm {
        MediaBuffer *mbuf;
        TAILQ_ENTRY(Elm) next;

        Elm() : mbuf(NULL) {}
        Elm(MediaBuffer *mbuf) : mbuf(mbuf) {}
    };

    BufferQueue()
    {
        TAILQ_INIT(&this->queue);
    }

    void insert(Elm *buffer)
    {
        TAILQ_INSERT_TAIL(&this->queue, buffer, next);
    }

    void insertFirst(Elm *buffer)
    {
        TAILQ_INSERT_HEAD(&this->queue, buffer, next);
    }

    Elm *get(MediaBuffer *mbuf)
    {
        Elm *ret = NULL, *buffer, *nextBuffer;

        for (buffer = TAILQ_FIRST(&this->queue); buffer != NULL; buffer = nextBuffer) {
            nextBuffer = TAILQ_NEXT(buffer, next);
            if (buffer->mbuf == mbuf) {
                TAILQ_REMOVE(&this->queue, buffer, next);
                ret = buffer;
                break;
            }
        }
        return ret;
    }

    Elm *getFirst()
    {
        Elm *buffer;

        buffer = TAILQ_FIRST(&this->queue);
        if (buffer)
            TAILQ_REMOVE(&this->queue, buffer, next);

        return buffer;
    }
private:
    typedef TAILQ_HEAD(, Elm) BUFFER_Q;
    BUFFER_Q queue;
};

struct sfdec_omxcodec
{
    sp<ANativeWindow>   mNativeWindow;
    sp<MediaSource>     mOMXCodec;
    sp<AvosSource>      mAvosSource;
    sp<SfdecRenderer>   mSfdecRenderer;
    OMXClient          *mOMXClient;
    int                 hackCropUpdate;
};

#define SEEK_MAX 20

class AvosSource : public MediaSource, public MediaBufferObserver {
public:
    AvosSource(sp<MetaData> format, size_t size)
    {
        LOG();
        pthread_mutex_init(&this->mutex, NULL);
        pthread_cond_init(&this->cond, NULL);
        this->format = format;
        for (int i = 0; i < BUFFER_NB; ++i) {
            this->buffers[i].mbuf = MediaBuffer_Create(size);
            this->buffers[i].mbuf->setObserver(this);
            this->freeQ.insert(&this->buffers[i]);
        }
        this->seeking = false;
        this->run = true;
    }

    ~AvosSource()
    {
        LOG();
        BufferQueue::Elm *buffer;

        pthread_mutex_lock(&this->mutex);
        while ((buffer = this->freeQ.getFirst())) {
            buffer->mbuf->setObserver(NULL);
            buffer->mbuf->release();
        }
        while ((buffer = this->filledQ.getFirst())) {
            buffer->mbuf->setObserver(NULL);
            buffer->mbuf->release();
        }
        while ((buffer = this->busyQ.getFirst())) {
            buffer->mbuf->setObserver(NULL);
        }
        pthread_mutex_unlock(&this->mutex);

        pthread_mutex_destroy(&this->mutex);
        pthread_cond_destroy(&this->cond);
    }

    status_t start(MetaData *params)
    {
        LOG();
        return OK;
    }

    status_t stop()
    {
        LOG();

        pthread_mutex_lock(&this->mutex);
        this->run = false;
        pthread_cond_broadcast(&this->cond);
        pthread_mutex_unlock(&this->mutex);
        return OK;
    }

    sp<MetaData> getFormat()
    {
        DBG LOG();
        return this->format;
    }

    void setFormat(sp<MetaData> format)
    {
        DBG LOG();
        this->format = format;
    }

    size_t fillBuffer(void *data, size_t size, int64_t time_us, int is_sync_frame, int wait)
    {
        size_t ret = 0;
        BufferQueue::Elm *buffer = NULL;
    
        pthread_mutex_lock(&this->mutex);

        /*
         * when seeking: wait for read called with seek options in order to
         * fill up a new buffer
         */
        while (this->run && !this->seeking && !(buffer = this->freeQ.getFirst()) && wait)
            pthread_cond_wait(&this->cond, &this->mutex);
        if (buffer) {
            MediaBuffer *mbuf = buffer->mbuf;

            if (size > mbuf->size()) {
                LOG("MediaBuffer too small: reallocting... (%d vs %d)", size, mbuf->size());
                mbuf->setObserver(NULL);
                mbuf->release();
                buffer->mbuf = mbuf = MediaBuffer_Create(size);
                mbuf->setObserver(this);
            }
            mbuf->reset();

            DBG LOG("filling input buffer %p s: %d t: %lld", data, size, time_us);
            if (data)
                memcpy(mbuf->data(), data, size);
            mbuf->set_range(0, size);
            mbuf->meta_data()->clear();
            mbuf->meta_data()->setInt32(kKeyIsSyncFrame, is_sync_frame);
            mbuf->meta_data()->setInt64(kKeyTime, time_us);
            ret = size;
            this->filledQ.insert(buffer);
            pthread_cond_broadcast(&this->cond);
        }
        pthread_mutex_unlock(&this->mutex);
        return ret;
    }

    int flush()
    {
        pthread_mutex_lock(&this->mutex);
        this->seeking = true;
        this->seek_try = 0;
        pthread_cond_broadcast(&this->cond);
        pthread_mutex_unlock(&this->mutex);
        return 0;
    }

    status_t read(MediaBuffer **pmbuf, const ReadOptions *options)
    {
        status_t ret;
        BufferQueue::Elm *buffer = NULL;

        pthread_mutex_lock(&this->mutex);
        if (options) {
            int64_t seekTimeUs = -1;
            ReadOptions::SeekMode seekMode;

            if (options->getSeekTo(&seekTimeUs, &seekMode)) {
                this->seeking = false;
                pthread_cond_broadcast(&this->cond);
            }
        }
        for (;;) {
            if (this->seeking && this->seek_try == SEEK_MAX ) {
LOG("ABORT seek!");
            this->seeking = false;
        }

            while (this->run && !this->seeking && !(buffer = this->filledQ.getFirst()))
                pthread_cond_wait(&this->cond, &this->mutex);
            /*
             * when seeking but read not called with seek options:
             * send back the previous buffer to unblock the read call.
             */
            if (!buffer && this->seeking) {
                buffer = this->freeQ.getFirst();
                DBG LOG("sending previous frame to unblock read, %d", this->seek_try);
                this->seek_try ++;
            }

            if (buffer) {
                MediaBuffer *mbuf = buffer->mbuf;

                mbuf->add_ref();
                this->busyQ.insert(buffer);
                *pmbuf = mbuf;
                ret = OK;
            } else {
                ret = this->run ? UNKNOWN_ERROR : ERROR_END_OF_STREAM;
            }
            break;
        }
        pthread_mutex_unlock(&this->mutex);

        return ret;
    }

    void signalBufferReturned(MediaBuffer *mbuf)
    {
        BufferQueue::Elm *buffer;

        pthread_mutex_lock(&this->mutex);

        buffer = this->busyQ.get(mbuf);
        if (buffer) {
            this->freeQ.insert(buffer);

            pthread_cond_broadcast(&this->cond);
        } else {
            LOG("Warning: unknow buffer returned ! ! !");
        }
        pthread_mutex_unlock(&this->mutex);
    }
private:
    static const int BUFFER_NB = 1;

    sp<MetaData> format;
    BufferQueue::Elm buffers[BUFFER_NB];
    BufferQueue freeQ;
    BufferQueue filledQ;
    BufferQueue busyQ;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool run;
    bool seeking;
    int seek_try;
};

class SfdecRenderer : public RefBase
{
public:
    SfdecRenderer() {}

    virtual void render(MediaBuffer *buffer) = 0;
private:
    SfdecRenderer(const SfdecRenderer &);
    SfdecRenderer &operator=(const SfdecRenderer &);
};

class SfdecNativeWindowRenderer : public SfdecRenderer
{
public:
    SfdecNativeWindowRenderer(const sp<ANativeWindow> &nativeWindow)
        : mNativeWindow(nativeWindow)
    {
        LOG();
    }

    virtual void render(MediaBuffer *buffer)
    {
        status_t err;
        int64_t timeUs;

        FIND_INT64(buffer->meta_data(), kKeyTime, &timeUs, -1);
        if (timeUs != -1)
            native_window_set_buffers_timestamp(mNativeWindow.get(), timeUs * 1000);

        ANativeWindowBuffer *awb = (ANativeWindowBuffer *) dl_oc.MediaBuffer_getGraphicBuffer(buffer).get();

#if SFDEC_ANDROID_API >= 17
        err = mNativeWindow->queueBuffer(mNativeWindow.get(), awb, -1);
#else
        err = mNativeWindow->queueBuffer(mNativeWindow.get(), awb);
#endif

        if (err == OK) {
            buffer->meta_data()->setInt32(kKeyRendered, 1);
        }
    }

protected:
    virtual ~SfdecNativeWindowRenderer()
    {
        LOG();
    }
private:
    sp<ANativeWindow> mNativeWindow;
};

class SfdecLocalRenderer : public SfdecRenderer
{
public:
    SfdecLocalRenderer(const sp<ANativeWindow> &nativeWindow, const sp<MetaData> &meta)
        : mRenderer(SoftwareRenderer_Create(nativeWindow, meta))
    {
        LOG();
    }

    virtual void render(MediaBuffer *buffer)
    {
        dl_oc.SoftwareRenderer_render(mRenderer, (const uint8_t *)buffer->data() + buffer->range_offset(),
               buffer->range_length(), NULL);
    }

protected:
    virtual ~SfdecLocalRenderer() {
        LOG();
        SoftwareRenderer_Destroy(mRenderer);
        mRenderer = NULL;
    }

private:
    void *mRenderer;
};

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
    sp<MetaData> meta;
    uint32_t omxFlags = 0;
    const char *mime_type;
    sfdec_priv_t *sfdec;

    if (dlhelper_oc_init())
        return NULL;

    mime_type = get_mimetype(codec);
    if (!mime_type)
        return NULL;

    LOG("(OMXCodec): %s", mime_type);

    sfdec = new sfdec_priv_t();
    if (!sfdec)
        return NULL;

    sfdec->mNativeWindow = (ANativeWindow *)surface_handle;
    if (sfdec->mNativeWindow == NULL ||
            native_window_api_connect(sfdec->mNativeWindow.get(), NATIVE_WINDOW_API_MEDIA) != 0) {
        delete sfdec;
        return NULL;
    }

    meta = MetaData_Create();
    if (meta == NULL) {
        native_window_api_disconnect(sfdec->mNativeWindow.get(), NATIVE_WINDOW_API_MEDIA);
        delete sfdec;
        return NULL;
    }
    meta->setCString(kKeyMIMEType, mime_type);
    meta->setInt32(kKeyWidth, *width);
    meta->setInt32(kKeyHeight, *height);
    meta->setInt32(kKeyRotation, rotation);
    if (codec == SFDEC_VIDEO_AVC && extradata) {
        LOG("meta->setData(kKeyAVCC, kTypeAVCC, %p, %d)", extradata, extradata_size);
        meta->setData(kKeyAVCC, kTypeAVCC, extradata, extradata_size);
    }
    if (codec == SFDEC_VIDEO_HEVC && extradata) {
        meta->setData('hvcc', 'hvcc', extradata, extradata_size);
    }

    sfdec->mOMXClient = new OMXClient;
    sfdec->mOMXClient->connect();

    sfdec->mAvosSource = new AvosSource(meta, (*width) * (*height) * 3 / 2);

    if (flags & SFDEC_FLAG_SWDEC)
        omxFlags = OMXCodec::kSoftwareCodecsOnly;

    sfdec->mOMXCodec = OMXCodec::Create(sfdec->mOMXClient->interface(),
                     meta,
                     false, // decoder
                     sfdec->mAvosSource,
                     NULL, // matchComponentName
                     omxFlags,
                     sfdec->mNativeWindow); // NativeWindow
    if (sfdec->mOMXCodec == NULL) {
        sfdec->mOMXClient->disconnect();
        native_window_api_disconnect(sfdec->mNativeWindow.get(), NATIVE_WINDOW_API_MEDIA);
        delete sfdec->mOMXClient;
        delete sfdec;
        return NULL;
    }

    const char *comp = NULL;
    int32_t colorFormat = 0;
    sp<MetaData> outFormat;

    outFormat = sfdec->mOMXCodec->getFormat();
    FIND_INT32(outFormat, kKeyColorFormat, &colorFormat, 0);
    FIND_STRING(outFormat, kKeyDecoderComponent, &comp);

    if (pts_reorder) {
        if (comp && !strncmp(comp, "OMX.SEC.avc", strlen("OMX.SEC.avc")))
            *pts_reorder = 1;
        else
            *pts_reorder = 0;
    }

    /*
     * HACK: crop is normally updated in OMXCodec code, but some
     * OMX.SEC.avc.dec version are buggy and crop is not updated.
     */
    if (!strcmp(comp, "OMX.SEC.avc.dec"))
        sfdec->hackCropUpdate = 1;
    else
        sfdec->hackCropUpdate = 0;

    LOG("color: 0x%X / comp: %s", colorFormat, comp);

    return sfdec;
}

static void sfdec_destroy(sfdec_priv_t *sfdec)
{
    LOG();

    sfdec->mAvosSource.clear();
    sfdec->mSfdecRenderer.clear();

    // From AwesomePlayer.cpp:
    // The following hack is necessary to ensure that the OMX
    // component is completely released by the time we may try
    // to instantiate it again.
    wp<MediaSource> tmp = sfdec->mOMXCodec;
    sfdec->mOMXCodec.clear();
    while (tmp.promote() != NULL) {
        usleep(1000);
    }
    IPCThreadState::self()->flushCommands();

    sfdec->mOMXClient->disconnect();

    native_window_api_disconnect(sfdec->mNativeWindow.get(), NATIVE_WINDOW_API_MEDIA);
    sfdec->mNativeWindow.clear();

    delete sfdec;
}

static int sfdec_start(sfdec_priv_t *sfdec)
{
    status_t err;
    
    LOG();
    err = sfdec->mOMXCodec->start();
    if (err != OK) {
        return -1;
    }

    return 0;
}

static int sfdec_stop(sfdec_priv_t *sfdec)
{
    LOG();
    status_t err;

    LOG("stop()");
    err = sfdec->mOMXCodec->stop();
    LOG("stop()::end");

    return err == OK ? 0 : -1;
}

static ssize_t sfdec_send_input(sfdec_priv_t *sfdec, void *data, size_t size, int64_t time_us, int is_sync_frame, int wait)
{
    return sfdec->mAvosSource->fillBuffer(data, size, time_us, is_sync_frame,  wait);
}

static int sfdec_flush(sfdec_priv_t *sfdec)
{
    return sfdec->mAvosSource->flush();
}

static int sfdec_stop_input(sfdec_priv_t *sfdec)
{
    /*
     * send a dummy frame to unblock sfdec_read that can be blocked with
     * some decoders
     */
    sfdec->mAvosSource->fillBuffer(NULL, 0, 0, 0, 0);
    sfdec->mAvosSource->stop();
    return 0;
}

static int initRenderer(sfdec_priv_t *sfdec, int32_t *displayWidth, int32_t *displayHeight, int32_t *interlaced)
{
    int32_t rotationDegrees;
    uint32_t transform;
    sp<MetaData> meta = sfdec->mOMXCodec->getFormat();

    // output input source

    sfdec->mAvosSource->setFormat(meta);

    // found display size using differents methods

    FIND_INT32(meta, kKeyDisplayWidth, displayWidth, -1);
    FIND_INT32(meta, kKeyDisplayHeight, displayHeight, -1);
    FIND_INT32(meta, kKeyBufferLayout, interlaced, 0);
    FIND_INT32(meta, kKeyRotation, &rotationDegrees, 0);

    switch (rotationDegrees) {
        case 0: transform = 0; break;
        case 90: transform = HAL_TRANSFORM_ROT_90; break;
        case 180: transform = HAL_TRANSFORM_ROT_180; break;
        case 270: transform = HAL_TRANSFORM_ROT_270; break;
        default: transform = 0; break;
    }
    if (transform)
        native_window_set_buffers_transform(sfdec->mNativeWindow.get(), transform);

    if (*displayWidth < 0 || *displayHeight < 0) {
        int32_t cropLeft, cropTop, cropRight, cropBottom;
        if (!meta->findRect(kKeyCropRect, &cropLeft, &cropTop, &cropRight, &cropBottom)) {
            int32_t width, height;

            FIND_INT32(meta, kKeyWidth, &width, -1);
            FIND_INT32(meta, kKeyHeight, &height, -1);
            cropLeft = cropTop = 0;
            cropRight = width - 1;
            cropBottom = height - 1;
            LOG("got dimensions only %d x %d", width, height);
        } else {
            LOG("got crop rect %d, %d, %d, %d", cropLeft, cropTop, cropRight, cropBottom);
        }

        *displayWidth = cropRight - cropLeft + 1;
        *displayHeight = cropBottom - cropTop + 1;

        if (sfdec->hackCropUpdate) {
            android_native_rect_t crop;

            crop.left = cropLeft;
            crop.top = cropTop;
            crop.right = cropRight;
            crop.bottom = cropBottom;
            native_window_set_crop(sfdec->mNativeWindow.get(), &crop);
            LOG("HACK: updating crop manually");
        }
    } else {
        LOG("got display size: %dx%d", *displayWidth, *displayHeight);
    }

    if (*interlaced)
        *displayHeight *= 2;

    if (rotationDegrees == 90 || rotationDegrees == 270) {
        int32_t tmp = *displayWidth;
        *displayWidth = *displayHeight;
        *displayHeight = tmp;
    }

    // clear and recreate the renderer

    sfdec->mSfdecRenderer.clear();

    IPCThreadState::self()->flushCommands();

    native_window_set_scaling_mode(sfdec->mNativeWindow.get(), NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);

    const char *component;
    FIND_STRING(meta, kKeyDecoderComponent, &component);
    if (!component)
        return -1;

    if (!strncmp(component, "OMX.", 4) &&
            strncmp(component, "OMX.google.", 11) &&
            strcmp(component, "OMX.Nvidia.mpeg2v.decode"))
        sfdec->mSfdecRenderer = new SfdecNativeWindowRenderer(sfdec->mNativeWindow);
    else
        sfdec->mSfdecRenderer = new SfdecLocalRenderer(sfdec->mNativeWindow, meta);

    return 0;
}

static int sfdec_read(sfdec_priv_t *sfdec, int64_t seek, sfdec_read_out_t *read_out)
{
    MediaBuffer *mbuf;
    status_t err;

    if (!read_out)
        return -1;

    read_out->flag = SFDEC_READ_INVALID;

    for (;;) {
        if (seek != -1) {
            MediaSource::ReadOptions options;
            options.setSeekTo(seek, MediaSource::ReadOptions::SEEK_NEXT_SYNC);
            err = sfdec->mOMXCodec->read(&mbuf, &options);
            seek = -1;
        } else {
            err = sfdec->mOMXCodec->read(&mbuf);
        }

        if ((err == OK && sfdec->mSfdecRenderer == NULL) || err == INFO_FORMAT_CHANGED) {
            int32_t displayWidth, displayHeight, interlaced;

            if (initRenderer(sfdec, &displayWidth, &displayHeight, &interlaced))
                break;
            read_out->flag |= SFDEC_READ_SIZE;
            read_out->size.width = displayWidth;
            read_out->size.height = displayHeight;
            read_out->size.interlaced = interlaced;
        }
        if (err == OK) {
            if (mbuf->range_length() == 0) {
                DBG LOG("(mbuf->range_length() == 0) -> release");
                mbuf->release();
                continue;
            } else {
                int64_t time_us;

                FIND_INT64(mbuf->meta_data(), kKeyTime, &time_us, -1);
                read_out->flag |= SFDEC_READ_BUF;
                read_out->buf.sfbuf = (sfbuf_t *) mbuf;
                read_out->buf.time_us = time_us;
                DBG LOG("buf: %p / time: %lld", mbuf, time_us);
            }
        } else {
            LOG("mOMXCodec->read returned: %d", err);
        }
        break;
    }

    return read_out->flag == SFDEC_READ_INVALID ? -1 : 0;
}

static int sfdec_buf_render(sfdec_priv_t *sfdec, sfbuf_t *sfbuf, int render)
{
    if(!render)
        return 0;
	
    MediaBuffer *mbuf = (MediaBuffer *) sfbuf;

    DBG {
        int64_t time_us;
        FIND_INT64(mbuf->meta_data(), kKeyTime, &time_us, -1);
        LOG("buf: %p / time: %lld", mbuf, time_us);
    }
    if (sfdec->mSfdecRenderer != NULL)
        sfdec->mSfdecRenderer->render(mbuf);
    return 0;
}

static int sfdec_buf_release(sfdec_priv_t *sfdec, sfbuf_t *sfbuf)
{
    MediaBuffer *mbuf = (MediaBuffer *) sfbuf;

    mbuf->release();
}

sfdec_itf_t sfdec_itf_omxcodec = {
    "OMXCodec",
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

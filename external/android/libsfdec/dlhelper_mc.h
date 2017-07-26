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

#ifdef DLHELPER_CFG

#define DLHELPER_STRUCT dl_mc
#define DLHELPER_INIT dlhelper_mc_init
#define DLHELPER_DEINIT dlhelper_mc_deinit

#else

#ifndef DLHELPER_REG
#error DLHELPER_REG not defined
#endif

DLHELPER_REG(DLHELPER_CRIT, MediaCodec_CreateByType,
    sp<MediaCodec>, (const sp<ALooper> &, const char *, bool),
    "_ZN7android10MediaCodec12CreateByTypeERKNS_2spINS_7ALooperEEEPKcb");

#if SFDEC_ANDROID_API >= 18
DLHELPER_REG(DLHELPER_CRIT, MediaCodec_configure,
    status_t, (void *,const sp<AMessage> &, const sp<Surface> &, const sp<ICrypto> &, uint32_t),
    "_ZN7android10MediaCodec9configureERKNS_2spINS_8AMessageEEERKNS1_INS_20SurfaceTextureClientEEERKNS1_INS_7ICryptoEEEj",
    "_ZN7android10MediaCodec9configureERKNS_2spINS_8AMessageEEERKNS1_INS_7SurfaceEEERKNS1_INS_7ICryptoEEEj");
#else
DLHELPER_REG(DLHELPER_CRIT, MediaCodec_configure,
    status_t, (void *,const sp<AMessage> &, const sp<SurfaceTextureClient> &, const sp<ICrypto> &, uint32_t),
    "_ZN7android10MediaCodec9configureERKNS_2spINS_8AMessageEEERKNS1_INS_20SurfaceTextureClientEEERKNS1_INS_7ICryptoEEEj",
    "_ZN7android10MediaCodec9configureERKNS_2spINS_8AMessageEEERKNS1_INS_7SurfaceEEERKNS1_INS_7ICryptoEEEj");
#endif

DLHELPER_REG(DLHELPER_CRIT, MediaCodec_start,
    status_t, (void *),
    "_ZN7android10MediaCodec5startEv");

DLHELPER_REG(DLHELPER_CRIT, MediaCodec_stop,
    status_t, (void *),
    "_ZN7android10MediaCodec4stopEv");

DLHELPER_REG(DLHELPER_CRIT, MediaCodec_flush,
    status_t, (void *),
    "_ZN7android10MediaCodec5flushEv");

DLHELPER_REG(DLHELPER_CRIT, MediaCodec_release,
    status_t, (void *),
    "_ZN7android10MediaCodec7releaseEv");

DLHELPER_REG(DLHELPER_CRIT, MediaCodec_queueInputBuffer,
    status_t, (void *, size_t, size_t, size_t, int64_t, uint32_t, AString *),
    "_ZN7android10MediaCodec16queueInputBufferEjjjxjPNS_7AStringE");

DLHELPER_REG(DLHELPER_CRIT, MediaCodec_dequeueInputBuffer,
    status_t, (void *, size_t *, int64_t),
    "_ZN7android10MediaCodec18dequeueInputBufferEPjx");

DLHELPER_REG(DLHELPER_CRIT, MediaCodec_dequeueOutputBuffer,
    status_t, (void *, size_t *, size_t *, size_t *, int64_t *, uint32_t *, int64_t),
    "_ZN7android10MediaCodec19dequeueOutputBufferEPjS1_S1_PxS1_x");

DLHELPER_REG(DLHELPER_CRIT, MediaCodec_renderOutputBufferAndRelease,
    status_t, (void *, size_t),
    "_ZN7android10MediaCodec28renderOutputBufferAndReleaseEj");

DLHELPER_REG(DLHELPER_CRIT, MediaCodec_releaseOutputBuffer,
    status_t, (void *, size_t),
    "_ZN7android10MediaCodec19releaseOutputBufferEj");

DLHELPER_REG(DLHELPER_CRIT, MediaCodec_getInputBuffers,
    status_t, (void *, Vector<sp<ABuffer> > *),
    "_ZNK7android10MediaCodec15getInputBuffersEPNS_6VectorINS_2spINS_7ABufferEEEEE");

DLHELPER_REG(DLHELPER_CRIT, MediaCodec_getOutputBuffers,
    status_t, (void *, Vector<sp<ABuffer> > *buffers),
    "_ZNK7android10MediaCodec16getOutputBuffersEPNS_6VectorINS_2spINS_7ABufferEEEEE");

DLHELPER_REG(DLHELPER_CRIT, MediaCodec_getOutputFormat,
    status_t, (void *, sp<AMessage> *format),
    "_ZNK7android10MediaCodec15getOutputFormatEPNS_2spINS_8AMessageEEE");

DLHELPER_REG(DLHELPER_CRIT, ALooper_ctor,
    void, (void *),
    "_ZN7android7ALooperC1Ev");

DLHELPER_REG(DLHELPER_CRIT, ALooper_start,
    status_t, (void *, bool, bool, int32_t),
    "_ZN7android7ALooper5startEbbi");

DLHELPER_REG(DLHELPER_CRIT, ALooper_stop,
    status_t, (void *),
    "_ZN7android7ALooper4stopEv");

DLHELPER_REG(DLHELPER_CRIT, AMessage_ctor,
    void, (void *, uint32_t, ALooper::handler_id),
    "_ZN7android8AMessageC1Eji");

DLHELPER_REG(DLHELPER_CRIT, AMessage_setString,
    void, (void *, const char *, const char *, int),
    "_ZN7android8AMessage9setStringEPKcS2_l",
    "_ZN7android8AMessage9setStringEPKcS2_i");

DLHELPER_REG(DLHELPER_CRIT, AMessage_setInt32,
    void, (void *, const char *, int32_t),
    "_ZN7android8AMessage8setInt32EPKci");

DLHELPER_REG(DLHELPER_CRIT, AMessage_setInt64,
    void, (void *, const char *, int64_t),
    "_ZN7android8AMessage8setInt64EPKcx");

DLHELPER_REG(DLHELPER_CRIT, AMessage_setBuffer,
    void, (void *, const char *, const sp<ABuffer> &),
    "_ZN7android8AMessage9setBufferEPKcRKNS_2spINS_7ABufferEEE");

DLHELPER_REG(DLHELPER_CRIT, AMessage_findBuffer,
    bool, (void *, const char *, sp<ABuffer> *),
    "_ZNK7android8AMessage10findBufferEPKcPNS_2spINS_7ABufferEEE");

DLHELPER_REG(DLHELPER_CRIT, AMessage_findInt32,
    bool, (void *, const char *, int32_t *),
    "_ZNK7android8AMessage9findInt32EPKcPi");

DLHELPER_REG(DLHELPER_CRIT, ABuffer_ctor,
    void, (void *, size_t),
    "_ZN7android7ABufferC1Ej");

DLHELPER_REG(DLHELPER_CRIT, ABuffer_meta,
    sp<AMessage>, (void *),
    "_ZN7android7ABuffer4metaEv");

DLHELPER_REG(DLHELPER_CRIT, ABuffer_setRange,
    void, (void *, size_t, size_t),
    "_ZN7android7ABuffer8setRangeEjj");

#endif

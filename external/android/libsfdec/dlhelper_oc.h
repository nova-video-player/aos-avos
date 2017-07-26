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

#define DLHELPER_STRUCT dl_oc
#define DLHELPER_INIT dlhelper_oc_init
#define DLHELPER_DEINIT dlhelper_oc_deinit

#else

#ifndef DLHELPER_REG
#error DLHELPER_REG not defined
#endif

DLHELPER_REG(DLHELPER_CRIT, Metadata_ctor,
    void, (void *),
    "_ZN7android8MetaDataC1Ev");

DLHELPER_REG(DLHELPER_CRIT, MediaBuffer_ctor,
    void, (void *, size_t size),
    "_ZN7android11MediaBufferC1Ej");

DLHELPER_REG(DLHELPER_CRIT, MediaBuffer_getGraphicBuffer,
    sp<GraphicBuffer>, (void *),
    "_ZNK7android11MediaBuffer13graphicBufferEv");

DLHELPER_REG(DLHELPER_CRIT, SoftwareRenderer_ctor,
    void, (void *, const sp<ANativeWindow> &nativeWindow, const sp<MetaData> &meta),
    "_ZN7android16SoftwareRendererC1ERKNS_2spI13ANativeWindowEERKNS1_INS_8MetaDataEEE");

DLHELPER_REG(DLHELPER_CRIT, SoftwareRenderer_dtor,
    void, (void *),
    "_ZN7android16SoftwareRendererD1Ev");

DLHELPER_REG(DLHELPER_CRIT, SoftwareRenderer_render,
    void, (void *, const void *data, size_t size, void *platformPrivate),
    "_ZN7android16SoftwareRenderer6renderEPKvjPv");

#endif

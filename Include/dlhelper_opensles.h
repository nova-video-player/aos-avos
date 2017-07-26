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

#define DLHELPER_STRUCT dl_osl
#define DLHELPER_INIT dlhelper_osl_init
#define DLHELPER_DEINIT dlhelper_osl_deinit
#define DLHELPER_HANDLE "libOpenSLES.so"

#else

DLHELPER_REG(DLHELPER_CRIT, slCreateEngine,
	SLresult, (SLObjectItf *, SLuint32, const SLEngineOption *, SLuint32, const SLInterfaceID *, const SLboolean *),
	"slCreateEngine");

DLHELPER_REG_VAR(DLHELPER_CRIT, SL_IID_ENGINE,
    const SLInterfaceID *, "SL_IID_ENGINE");

DLHELPER_REG_VAR(DLHELPER_CRIT, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
    const SLInterfaceID *, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");

DLHELPER_REG_VAR(DLHELPER_CRIT, SL_IID_PLAY,
    const SLInterfaceID *, "SL_IID_PLAY");

DLHELPER_REG_VAR(DLHELPER_CRIT, SL_IID_VOLUME,
    const SLInterfaceID *, "SL_IID_VOLUME");

#endif

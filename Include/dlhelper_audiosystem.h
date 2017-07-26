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

#define DLHELPER_STRUCT dl_as
#define DLHELPER_INIT dlhelper_as_init
#define DLHELPER_DEINIT dlhelper_as_deinit

#else

#ifndef DLHELPER_REG
#error DLHELPER_REG not defined
#endif

DLHELPER_REG(DLHELPER_CRIT, getOutputLatency,
	int32_t, (uint32_t *, uint32_t),
	"_ZN7android11AudioSystem16getOutputLatencyEPj19audio_stream_type_t");

#endif

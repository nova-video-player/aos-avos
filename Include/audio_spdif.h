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

#ifndef _AUDIO_SPDIF_H
#define _AUDIO_SPDIF_H

#include "types.h"

#ifdef CONFIG_SPDIF
int spdif_init(AUDIO_PROPERTIES *);
int spdif_encapsulate( AUDIO_PROPERTIES *a, UCHAR *data, int size, AUDIO_FRAME *frame, int *decoded );
int spdif_set_passthrough(int on);
int spdif_is_passthrough_on();
int spdif_format_passthrough_supported(int format);
void set_hdmi_supported_audio_codecs(long flag);
long get_hdmi_supported_audio_codecs();
int get_hdmi_supports_iec_8ch192khz(void);
int get_hdmi_supports_iec(void);
#else
static inline int spdif_init(AUDIO_PROPERTIES *a) { return 0; }
static inline int spdif_encapsulate( AUDIO_PROPERTIES *a, UCHAR *data, int size, AUDIO_FRAME *frame, int *decoded ) { return 0; }
static inline int spdif_set_passthrough(int on) { return 0; }
static inline int spdif_is_passthrough_on() { return 0; }
static inline int spdif_format_passthrough_supported(int format) { (void)format; return 0; }
static inline void set_hdmi_supported_audio_codecs(long flag) {}
static inline long get_hdmi_supported_audio_codecs() { return 0; }
static inline int get_hdmi_supports_iec_8ch192khz(void) { return 0; }
static inline int get_hdmi_supports_iec(void) { return 0; }
#endif
#endif

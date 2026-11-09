/*
 * Copyright 2021 Courville Software
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

#ifndef ANDROID_CODEC_H
#define ANDROID_CODEC_H

/*
 * Android only check if codec is supported
 */
int acodecs_is_type_supported(const char *type, int is_sw_allowed);
const char* acodecs_get_for_profile(const char *mime_type, int profile);
int acodecs_get_dovi_mode(void);
// 1 when a hardware decoder advertises the profile, ignoring Force mode;
// 0 for older Java clients.
int acodecs_is_profile_advertised(const char *mime, int profile);
/* Returned codec names are owned by the caller. Index bounds initialization retries. */
const char *acodecs_get_video_decoder(const char *mime, int profile,
    int width, int height, double frame_rate, int index);
int acodecs_get_display_hdr_capabilities(void); /* -1: unknown; bit 3: Dolby Vision */

#endif

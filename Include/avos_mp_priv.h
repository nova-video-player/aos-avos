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

#ifndef AVOS_MP_PRIV_H
#define AVOS_MP_PRIV_H

#include "av.h"
#include "id3tag.h"
#include "avos_common_priv.h"

typedef struct avos_mp_video avos_mp_video_t;
typedef struct avos_mp_audio avos_mp_audio_t;

avos_mp_video_t *avos_mp_getvideo(avos_mp_t *mp);
avos_mp_audio_t *avos_mp_getaudio(avos_mp_t *mp);
int avos_mp_fillmetadata(avos_mp_t *mp, int type, uint64_t size, ID3_TAG *id3_tag, AV_PROPERTIES *av, const char *mimetype, int duration, int seekable, int pauseable, int decoder); /* return 1 if metadata changed */

void avos_mp_sendevent(avos_mp_t *mp, int what, int arg1, int arg2);
void avos_mp_sendevent_msg(avos_mp_t *mp, int what, int arg1, int arg2, const char *msg);
void avos_mp_sendevent_data(avos_mp_t *mp, int what, int arg1, int arg2, avos_msg_t *data);

#endif // AVOS_MP_PRIV_H

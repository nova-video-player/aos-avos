/*
 * Copyright 2026 Courville Software
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

#ifndef VIDEO_HDR_H
#define VIDEO_HDR_H

#include <stdint.h>
#include <string.h>
#include <libavutil/mastering_display_metadata.h>

/* CTA-861.3 descriptor: one ID byte followed by 12 little-endian uint16s.
 * Serialize bytewise: the values start at an unaligned address.
 */
static inline int video_hdr_scaled(AVRational value, int scale, int limit)
{
    if (value.num < 0 || value.den <= 0 ||
        (int64_t)value.num * scale > (int64_t)limit * value.den)
        return -1;
    return ((int64_t)value.num * scale + value.den / 2) / value.den;
}

static inline int video_hdr_static_info(unsigned char out[25],
    const AVMasteringDisplayMetadata *mastering, const AVContentLightMetadata *light)
{
    int values[12] = {0}, present = 0;
    memset(out, 0, 25);
    if (mastering && mastering->has_primaries) {
        int primaries[8], valid = 1;
        for (int i = 0; i < 6; ++i)
            primaries[i] = video_hdr_scaled(mastering->display_primaries[i / 2][i % 2], 50000, 50000);
        for (int i = 0; i < 2; ++i)
            primaries[6 + i] = video_hdr_scaled(mastering->white_point[i], 50000, 50000);
        for (int i = 0; i < 8; ++i) valid &= primaries[i] >= 0;
        if (valid) {
            memcpy(values, primaries, sizeof(primaries));
            present = 1;
        }
    }
    if (mastering && mastering->has_luminance) {
        int maximum = video_hdr_scaled(mastering->max_luminance, 1, 65535);
        int minimum = video_hdr_scaled(mastering->min_luminance, 10000, 65535);
        if (maximum >= 0 && minimum >= 0 &&
            (int64_t)mastering->min_luminance.num * mastering->max_luminance.den <=
            (int64_t)mastering->max_luminance.num * mastering->min_luminance.den) {
            values[8] = maximum;
            values[9] = minimum;
            present = 1;
        }
    }
    if (light && light->MaxCLL <= 65535 && light->MaxFALL <= 65535) {
        values[10] = light->MaxCLL;
        values[11] = light->MaxFALL;
        present = 1;
    }
    if (!present) return 0;
    for (int i = 0; i < 12; ++i) {
        out[1 + 2 * i] = values[i] & 255;
        out[2 + 2 * i] = values[i] >> 8;
    }
    return 25;
}

#endif

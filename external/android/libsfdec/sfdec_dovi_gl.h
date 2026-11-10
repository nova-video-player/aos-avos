// Copyright 2026 Courville Software
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

#ifndef SFDEC_DOVI_GL_H
#define SFDEC_DOVI_GL_H

#include <stdbool.h>
#include <stdint.h>
#include <android/native_window.h>

#include "video_dovi_reshape.h"

// Dolby Vision single-layer reshaping on the GPU (Profile 5 decoded by a plain
// HEVC decoder). MediaCodec renders into an AImageReader; a GL thread samples
// each frame as raw YUV (GL_EXT_YUV_target), applies the frame's RPU
// reshaping and presents BT.2020 PQ to the app window at the codec's
// presentation time. See Video/doc/DOVI.md.
typedef struct sfdec_dovi_gl sfdec_dovi_gl_t;

// NULL when the device cannot do it (API < 26, no usable window surface); the
// caller then renders directly. sdr: the display has no HDR, tone map to SDR
// BT.709 instead of presenting PQ, as is also done without a BT.2020 PQ surface.
sfdec_dovi_gl_t *sfdec_dovi_gl_create(ANativeWindow *window, int width, int height, bool sdr);
void sfdec_dovi_gl_destroy(sfdec_dovi_gl_t *gl);
// Surface to configure the codec with.
ANativeWindow *sfdec_dovi_gl_codec_window(sfdec_dovi_gl_t *gl);
// Reshaping parameters of the access unit with this input timestamp.
void sfdec_dovi_gl_set_params(sfdec_dovi_gl_t *gl, int64_t pts_us, const VIDEO_DOVI_RESHAPE *params);
// Called right before the codec renders the output buffer of pts_us into the
// reader: image_ts_ns is the timestamp that frame will carry and present_ns
// the requested presentation time (0 = as soon as possible).
void sfdec_dovi_gl_queue(sfdec_dovi_gl_t *gl, int64_t pts_us, int64_t image_ts_ns, int64_t present_ns);
// Forget queued frames and parameters (flush/seek).
void sfdec_dovi_gl_flush(sfdec_dovi_gl_t *gl);

#endif

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

#ifndef _AC3_RECODE_H
#define _AC3_RECODE_H

// The recoder always emits 1536-sample AC3 frames at 48 kHz, independently
// of the decoded source rate. AudioTrack and stream timing must use these
// output-domain values from the first sink configuration.
#define AC3_RECODE_SAMPLE_RATE   48000
#define AC3_RECODE_FRAME_SAMPLES 1536

// AC3 recode output layout: published by the AC3 encoder filter after a fully
// successful open, then latched into the AudioTrack context when its AC3 sink
// is configured. The publication is only a startup handoff; latency code must
// use the context-local copy rather than reading this state live.
//
// The mode2 compatibility policy selects pipeline latency for stereo 2.0/192k
// output and app latency for multichannel/640k output. Discriminate using the
// encoder target, not the AudioTrack channel count: both compressed carriers
// are configured as two-channel AudioTracks.
void libavos_set_ac3_recode_target_stereo(int stereo);
int  libavos_get_ac3_recode_target_stereo(void);

#endif

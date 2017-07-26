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

#define DLHELPER_STRUCT dl_at
#define DLHELPER_INIT dlhelper_at_init
#define DLHELPER_DEINIT dlhelper_at_deinit

#else

#ifndef DLHELPER_REG
#error DLHELPER_REG not defined
#endif

DLHELPER_REG(DLHELPER_CRIT, getMinFrameCount,
	int, (int *, int, unsigned int),
	"_ZN7android10AudioTrack16getMinFrameCountEPi19audio_stream_type_tj",
	"_ZN7android10AudioTrack16getMinFrameCountEPiij");

DLHELPER_REG(DLHELPER_CRIT, ctor,
	void, (void *, int, unsigned int, int, int, int, unsigned int, void (*)(int, void *, void *), void *, int, int),
	"_ZN7android10AudioTrackC1EijiiijPFviPvS1_ES1_ii");

DLHELPER_REG(DLHELPER_CRIT, dtor,
	void, (void *),
	"_ZN7android10AudioTrackD1Ev");

DLHELPER_REG(DLHELPER_CRIT, initCheck,
	int, (void *),
	"_ZNK7android10AudioTrack9initCheckEv");

DLHELPER_REG(DLHELPER_CRIT, start,
	void, (void *),
	"_ZN7android10AudioTrack5startEv");

DLHELPER_REG(DLHELPER_CRIT, stop,
	void, (void *),
	"_ZN7android10AudioTrack4stopEv");

DLHELPER_REG(DLHELPER_CRIT, stopped,
	bool, (void *),
	"_ZNK7android10AudioTrack7stoppedEv");

DLHELPER_REG(DLHELPER_CRIT, write,
	int, (void *, void  const*, unsigned int),
	"_ZN7android10AudioTrack5writeEPKvj");

DLHELPER_REG(DLHELPER_CRIT, flush,
	void, (void *),
	"_ZN7android10AudioTrack5flushEv");

DLHELPER_REG(DLHELPER_CRIT, latency,
	uint32_t, (void *),
	"_ZNK7android10AudioTrack7latencyEv");

DLHELPER_REG(DLHELPER_CRIT, frameCount,
	uint32_t, (void *),
	"_ZNK7android10AudioTrack10frameCountEv");

DLHELPER_REG(DLHELPER_CRIT, frameSize,
	size_t, (void *),
	"_ZNK7android10AudioTrack9frameSizeEv");

DLHELPER_REG(DLHELPER_CRIT, channelCount,
	int, (void *),
	"_ZNK7android10AudioTrack12channelCountEv");

DLHELPER_REG(DLHELPER_CRIT, getSessionId,
	int, (void *),
	"_ZNK7android10AudioTrack12getSessionIdEv",
	"_ZN7android10AudioTrack12getSessionIdEv");

#endif

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

#ifndef _SOUND_H_
#define _SOUND_H_

#include "audio_interface.h"

#define EQUALIZER_NUM_BARS           5

typedef struct SOUND {
	int volume;
	int mute;
	int boost;

	int balance;
	int bass;
	int treble;
	int speaker;		// Current state of the speaker : on/off
	int speaker_mode;	// Speaker mode selected by the user : on/off/auto
        int speaker_forced;
        int fm_transmiter;

	int dynamic_volume;

	int audio_output;	// 0 analog, 1 spdif, 2 hdmi

	int bb_freq;
	int bb_level;
	int eq_preset;
	int eq_custom[EQUALIZER_NUM_BARS];

	int line_gain;
	int mic_gain;
	int video_gain;
	int fm_gain;
	int voice_gain;
	int audio_agc;
	audio_ctx_t *audio_ctx;
} SOUND;

// Sound settings
#define VOL_MAX     		99
#define VOL_DEFAULT 		75
#define VOL_STEP 		3

#define BALANCE_MIN		-10
#define BALANCE_MAX		10
#define BALANCE_STEP_LEVEL	1/20
#define BASS_MIN		-5
#define BASS_MAX		5
#define TREBLE_MIN		-5
#define TREBLE_MAX		5

// External speaker
#define	SPEAKER_OFF		0
#define	SPEAKER_ON		1
#define	SPEAKER_AUTO		2

#define SPEAKER_DEFAULT		SPEAKER_AUTO

#define	SPEAKER_INACTIVE	0
#define	SPEAKER_ACTIVE		1

// Mute
#define	SOUND_MUTE_TOGGLE	0x01
#define	SOUND_MUTE_MANUAL	0x02
#define	SOUND_MUTE_FORCE	0x04

//audio output
#define SOUND_ANALOG 0
#define SOUND_SPDIF  1
#define SOUND_HDMI   2

#define SOUND_SPDIF_ENABLE      1
#define SOUND_SPDIF_DISABLE     0

extern SOUND	sound;

void sound_init( void );
void sound_deinit( void );
void sound_apply_settings( void );
void sound_apply_volume( void );
void sound_update_volume( int value );

#endif	// _SOUND_H_

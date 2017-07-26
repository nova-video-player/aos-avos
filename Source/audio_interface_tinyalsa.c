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

#include "global.h"
#include "debug.h"
#include "types.h"
#include "audio_interface.h"
#include "atime.h"

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <tinyalsa/asoundlib.h>

#ifndef STANDALONE

#define DBG  if(Debug[DBG_AUDIODEVICE])
#define DBG2 if(Debug[DBG_AUDIODEVICE] > 1)

#endif

#define MIXER_MASTER_SWITCH_CTL 0
#define MIXER_MASTER_VOLUME_CTL 1

#define FRAG_SIZE      1024
#define FRAG_COUNT        4

struct audio_interface_tinyalsa_mixer {
	struct mixer *mixer;
	struct mixer_ctl *ctl;
	int num_controls;
 	struct mixer_ctl *master_volume_ctl;
 	struct mixer_ctl *master_switch_ctl;
};

struct audio_interface_tinyalsa {
	unsigned int device;
	unsigned int card;
	struct pcm *pcm;
	struct pcm_config config;
	struct audio_interface_tinyalsa_mixer mixer;
} _tinyalsa_handle;

int audio_interface_init(void)
{
	return 0;
}

int audio_interface_exit(void)
{
	return 0;
}

#if 0
static void tinymix_print_enum(struct mixer_ctl *ctl, int print_all)
{
    unsigned int num_enums;
    char buffer[256];
    unsigned int i;

    num_enums = mixer_ctl_get_num_enums(ctl);

    for (i = 0; i < num_enums; i++) {
        mixer_ctl_get_enum_string(ctl, i, buffer, sizeof(buffer));
        if (print_all)
            printf("\t%s%s", mixer_ctl_get_value(ctl, 0) == (int)i ? ">" : "",
                   buffer);
        else if (mixer_ctl_get_value(ctl, 0) == (int)i)
            printf(" %-s", buffer);
    }
}
static void tinymix_detail_control(struct mixer *mixer, unsigned int id,
                                   int print_all)
{
    struct mixer_ctl *ctl;
    enum mixer_ctl_type type;
    unsigned int num_values;
    char buffer[256];
    unsigned int i;
    int min, max;

    if (id >= mixer_get_num_ctls(mixer)) {
        fprintf(stderr, "Invalid mixer control\n");
        return;
    }

    ctl = mixer_get_ctl(mixer, id);

    mixer_ctl_get_name(ctl, buffer, sizeof(buffer));
    type = mixer_ctl_get_type(ctl);
    num_values = mixer_ctl_get_num_values(ctl);

    if (print_all)
        printf("%s:", buffer);

    for (i = 0; i < num_values; i++) {
        switch (type)
        {
        case MIXER_CTL_TYPE_INT:
            printf(" %d", mixer_ctl_get_value(ctl, i));
            break;
        case MIXER_CTL_TYPE_BOOL:
            printf(" %s", mixer_ctl_get_value(ctl, i) ? "On" : "Off");
            break;
        case MIXER_CTL_TYPE_ENUM:
            tinymix_print_enum(ctl, print_all);
            break;
         case MIXER_CTL_TYPE_BYTE:
            printf(" 0x%02x", mixer_ctl_get_value(ctl, i));
            break;
        default:
            printf(" unknown");
            break;
        };
    }

    if (print_all) {
        if (type == MIXER_CTL_TYPE_INT) {
            min = mixer_ctl_get_range_min(ctl);
            max = mixer_ctl_get_range_max(ctl);
            printf(" (range %d->%d)", min, max);
        }
    }
    printf("\n");
}
#endif

static struct audio_interface_tinyalsa_mixer *audio_interface_open_mixer(struct audio_interface_tinyalsa *tinyalsa)
{
	struct audio_interface_tinyalsa_mixer *mixer = &tinyalsa->mixer;
 	struct mixer_ctl *ctl;
 	char buffer[256];
	int i;

serprintf( "TINYALSA: open mixer\r\n");
	mixer->mixer = mixer_open(tinyalsa->card);
	if (!mixer->mixer) {
		serprintf("TINYALSA: Failed to open mixer\n");
		return NULL;
	}

#if 0
printf("TINYALSA: num controls: %d\n", mixer_get_num_ctls(mixer->mixer));
	for (i=0; i<mixer_get_num_ctls(mixer->mixer); i++) {
		const char *type;
		unsigned int num_values;
		ctl = mixer_get_ctl(mixer->mixer, i);
		type = mixer_ctl_get_type_string(ctl);
		num_values = mixer_ctl_get_num_values(ctl);
		mixer_ctl_get_name(ctl, buffer, sizeof(buffer));
		printf("%d: %s (%s, %d)\n", i, buffer, type, num_values);
		tinymix_detail_control(mixer->mixer, i, 0);
	}
#endif

	mixer->master_switch_ctl = mixer_get_ctl(mixer->mixer, MIXER_MASTER_SWITCH_CTL);
	mixer->master_volume_ctl = mixer_get_ctl(mixer->mixer, MIXER_MASTER_VOLUME_CTL);
	return mixer;
failed:
	mixer_close(mixer->mixer);
	return NULL;
}

static int audio_interface_close_mixer(struct audio_interface_tinyalsa_mixer *mixer)
{
serprintf( "TINYALSA close mixer\r\n");
	mixer_close(mixer->mixer);
	return 0;
}

void *audio_interface_open(int _mode)
{
	struct pcm_config *config = &_tinyalsa_handle.config;
DBG serprintf( "TINYALSA open(%i)\r\n", _mode );

	switch ( _mode ) {
	case AUDIO_OUTPUT_MODE:
		break;
	case AUDIO_MIXER_MODE:
		return audio_interface_open_mixer(&_tinyalsa_handle);
/*
	case ReadOnlyMode:
		break;
	case ReadWriteMode:
		break;
*/
	default:
		return NULL;
	}

	_tinyalsa_handle.card = 0;
	_tinyalsa_handle.device = 0;
	
	config->channels = 2;
	config->rate = 48000;
	config->period_size = FRAG_SIZE;
	config->period_count = FRAG_COUNT;
	config->start_threshold = 0;
	config->stop_threshold = 0;
	config->silence_threshold = 0;

	_tinyalsa_handle.pcm = pcm_open(_tinyalsa_handle.card, _tinyalsa_handle.device, PCM_OUT, config);
	if (!_tinyalsa_handle.pcm || !pcm_is_ready(_tinyalsa_handle.pcm)) {
		serprintf("TINYALSA: Unable to open PCM device %u (%s)\n",
				_tinyalsa_handle.device, pcm_get_error(_tinyalsa_handle.pcm));
		return NULL;
	}

	return &_tinyalsa_handle;
}

// ************************************************************
//
//      _get_obuffer
//
// ***********************************************************
static int _get_obuffer( int *size, int *free )
{
	struct timespec dummy;
	unsigned int avail;
	
DBG serprintf( "TINYALSA_get_obuffer()\r\n" );
	if (_tinyalsa_handle.pcm)
		*size = pcm_frames_to_bytes(_tinyalsa_handle.pcm, pcm_get_buffer_size(_tinyalsa_handle.pcm));
	else
		*size = 0;

	if (pcm_get_htimestamp(_tinyalsa_handle.pcm, &avail, &dummy)) {
serprintf( "OSS_getobuffer error: %s\r\n", pcm_get_error(_tinyalsa_handle.pcm) );		       
		*free = 0;
	} else {
		*free = pcm_frames_to_bytes(_tinyalsa_handle.pcm, avail);
	}

DBG serprintf( "TINYALSA_get_obuffer(): %d, %d\r\n", *size, *free);
	return 0;

}

// ************************************************************
//
//      _sync
//
// ***********************************************************
static int _sync( void )
{
DBG serprintf( "TINYALSA_sync\r\n" );

	return 0;
}

int audio_interface_close(void **ctx)
{
	if( *ctx != &_tinyalsa_handle ) {
		return audio_interface_close_mixer(&_tinyalsa_handle.mixer);
	}

	struct audio_interface_tinyalsa *tinyalsa = *ctx;

DBG serprintf( "TINYALSA_close()\r\n" );
	if (tinyalsa->pcm)
		pcm_close(tinyalsa->pcm);
	*ctx = NULL;

	return 0;
}

int audio_interface_start(void *ctx)
{
DBG serprintf( "TINYALSA: start\n" );
	return 0;
}

int audio_interface_stop(void *ctx)
{
DBG serprintf( "TINYALSA: stop\n" );
	return 0;
}

int audio_interface_can_write(void *ctx)
{
	struct audio_interface_tinyalsa *tinyalsa = ctx;

//DBG serprintf( "TINYALSA: audio_interface_can_write\n");

	if (!pcm_is_ready(tinyalsa->pcm))
		return 0;

	return 1;
}

int audio_interface_write(void *ctx, unsigned char *data, int data_length)
{
	struct audio_interface_tinyalsa *tinyalsa = ctx;
//DBG serprintf( "TINYALSA: audio_interface_write %d\n", data_length);

	if (pcm_write(tinyalsa->pcm, data, data_length)) {
		serprintf("TINYALSA_write: Error playing sample\n");
		return -1;
	}
	return 0;
}

int audio_interface_set_output_params(void *ctx, int freq, int channels, int bits, int format )
{
	struct audio_interface_tinyalsa *tinyalsa = ctx;
	struct pcm_config *config = &tinyalsa->config;
DBG serprintf( "TINYALSA: audio_interface_set_output_params\n");

	unsigned int period_size = FRAG_SIZE;
	unsigned int period_count = FRAG_COUNT;
	
	switch(format) {	
		case AFMT_S16_LE:
		default:
			config->format = PCM_FORMAT_S16_LE;
			break;
	}
	
	config->channels = channels;
	config->rate = freq;
	config->period_size = period_size;
	config->period_count = period_count;
	config->start_threshold = 0;
	config->stop_threshold = 0;
	config->silence_threshold = 0;
	
	if (tinyalsa->pcm)
		pcm_close(tinyalsa->pcm);
	
	tinyalsa->pcm = pcm_open(tinyalsa->card, tinyalsa->device, PCM_OUT, config);
	if (!tinyalsa->pcm || !pcm_is_ready(tinyalsa->pcm)) {
		serprintf("TINYALSA: Unable to reopen PCM device %u with new configuration(%s)\n",
				tinyalsa->device, pcm_get_error(tinyalsa->pcm));
		return -1;
	}
	
	return 0;
}

int audio_interface_get_delay(void *ctx)
{
	struct audio_interface_tinyalsa *tinyalsa = ctx;
	struct pcm_config *config = &tinyalsa->config;
	/* VP ??? */
	return (UINT64)(FRAG_SIZE * FRAG_COUNT) / (config->rate * config->channels * 2);
}

void audio_interface_flush_output(void *ctx) 
{
}

int audio_interface_preload(void *ctx)
{
	return -1;
}

int audio_interface_mute(void *ctx, BOOL fade)
{
DBG serprintf( "audio_interface_mute\r\n" );
return 0;
	struct audio_interface_tinyalsa_mixer *mixer = &_tinyalsa_handle.mixer;
	if (mixer_ctl_set_value(mixer->master_switch_ctl, 0, 1)) {
		serprintf("TINYALSA_mute: invalid value\n");
                return -1;
	}

	return -1;
}

int audio_interface_unmute(void *ctx, BOOL fade, BOOL threaded)
{
DBG serprintf( "audio_interface_unmute\r\n" );
return 0;
	struct audio_interface_tinyalsa_mixer *mixer = &_tinyalsa_handle.mixer;
	if (mixer_ctl_set_value(mixer->master_switch_ctl, 0, 0)) {
		serprintf("TINYALSA_umute: invalid value\n");
                return -1;
	}

	return 0;
}

#define VOLUME_MAX	99

int audio_interface_set_output_volume(void *ctx, int volume, int balance)
{
DBG serprintf( "audio_interface_set_output_volume\r\n" );
	struct audio_interface_tinyalsa_mixer *mixer = &_tinyalsa_handle.mixer;
	int min, max;
	min = mixer_ctl_get_range_min(mixer->master_volume_ctl);
	max = mixer_ctl_get_range_max(mixer->master_volume_ctl);
 
	volume = (volume - min) * (max - min) / VOLUME_MAX;

	if (mixer_ctl_set_value(mixer->master_volume_ctl, 0, volume)) {
		serprintf("TINYALSA_set_output_volume: invalid value\n");
                return -1;
	}

	if (mixer_ctl_set_value(mixer->master_volume_ctl, 1, volume)) {
		serprintf("TINYALSA_set_output_volume: invalid value\n");
                return -1;
	}

	return 0;
}

int audio_interface_set_output_volume_l_r(void *ctx, int vol_l, int vol_r)
{
DBG serprintf( "audio_interface_set_output_volume_l_r\r\n" );
	struct audio_interface_tinyalsa_mixer *mixer = &_tinyalsa_handle.mixer;
	int min, max;
	min = mixer_ctl_get_range_min(mixer->master_volume_ctl);
	max = mixer_ctl_get_range_max(mixer->master_volume_ctl);
 
	vol_l = (vol_l - min) * (max - min) / VOLUME_MAX;
	vol_r = (vol_r - min) * (max - min) / VOLUME_MAX;

	if (mixer_ctl_set_value(mixer->master_volume_ctl, 0, vol_l)) {
		serprintf("TINYALSA_set_output_volume: invalid value\n");
                return -1;
	}

	if (mixer_ctl_set_value(mixer->master_volume_ctl, 1, vol_r)) {
		serprintf("TINYALSA_set_output_volume: invalid value\n");
                return -1;
	}

	return 0;
}

int audio_interface_get_session_id(void *ctx)
{
	return 0;
}

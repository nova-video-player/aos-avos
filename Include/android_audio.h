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

/*
 * android audio defines, copied from system/core/include/system/audio.h
 */

typedef enum {
    AUDIO_OUTPUT_FLAG_NONE = 0x0,       // no attributes
    AUDIO_OUTPUT_FLAG_DIRECT = 0x1,     // this output directly connects a track
                                        // to one output stream: no software mixer
    AUDIO_OUTPUT_FLAG_PRIMARY = 0x2,    // this output is the primary output of
                                        // the device. It is unique and must be
                                        // present. It is opened by default and
                                        // receives routing, audio mode and volume
                                        // controls related to voice calls.
    AUDIO_OUTPUT_FLAG_FAST = 0x4,       // output supports "fast tracks",
                                        // defined elsewhere
    AUDIO_OUTPUT_FLAG_DEEP_BUFFER = 0x8, // use deep audio buffers
    AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD = 0x10,  // offload playback of compressed
                                                // streams to hardware codec
    AUDIO_OUTPUT_FLAG_NON_BLOCKING = 0x20, // use non-blocking write
    AUDIO_OUTPUT_FLAG_HW_AV_SYNC = 0x40, // output uses a hardware A/V synchronization source
    AUDIO_OUTPUT_FLAG_DYNAMIC_RATE = 0x80, //ARCHOS GEN10 ENHANCEMENT
    AUDIO_OUTPUT_FLAG_TTS = 0x80,          // output for streams transmitted through speaker
                                           // at a sample rate high enough to accommodate
                                           // lower-range ultrasonic playback
    AUDIO_OUTPUT_FLAG_RAW = 0x100,         // minimize signal processing
    AUDIO_OUTPUT_FLAG_SYNC = 0x200,        // synchronize I/O streams

    AUDIO_OUTPUT_FLAG_IEC958_NONAUDIO = 0x400, // Audio stream contains compressed audio in
                                               // SPDIF data bursts, not PCM.

} audio_output_flags_t;

typedef enum {
    AUDIO_STREAM_DEFAULT          = -1,
    AUDIO_STREAM_VOICE_CALL       = 0,
    AUDIO_STREAM_SYSTEM           = 1,
    AUDIO_STREAM_RING             = 2,
    AUDIO_STREAM_MUSIC            = 3,
    AUDIO_STREAM_ALARM            = 4,
    AUDIO_STREAM_NOTIFICATION     = 5,
    AUDIO_STREAM_BLUETOOTH_SCO    = 6,
    AUDIO_STREAM_ENFORCED_AUDIBLE = 7, /* Sounds that cannot be muted by user and must be routed to speaker */
    AUDIO_STREAM_DTMF             = 8,
    AUDIO_STREAM_TTS              = 9,

    AUDIO_STREAM_CNT,
    AUDIO_STREAM_MAX              = AUDIO_STREAM_CNT - 1,
} audio_stream_type_t;

typedef enum {
    AUDIO_FORMAT_PCM_SUB_16_BIT          = 0x1, /* DO NOT CHANGE - PCM signed 16 bits */
    AUDIO_FORMAT_PCM_SUB_8_BIT           = 0x2, /* DO NOT CHANGE - PCM unsigned 8 bits */
    AUDIO_FORMAT_PCM_SUB_32_BIT          = 0x3, /* PCM signed .31 fixed point */
    AUDIO_FORMAT_PCM_SUB_8_24_BIT        = 0x4, /* PCM signed 7.24 fixed point */
    AUDIO_FORMAT_PCM_SUB_FLOAT           = 0x5, /* PCM single-precision floating point */
    AUDIO_FORMAT_PCM_SUB_24_BIT_PACKED   = 0x6, /* PCM signed .23 fixed point packed in 3 bytes */
} audio_format_pcm_sub_fmt_t;
typedef enum {
    AUDIO_FORMAT_MP3_SUB_NONE            = 0x0,
} audio_format_mp3_sub_fmt_t;

typedef enum {
    AUDIO_FORMAT_AMR_SUB_NONE            = 0x0,
} audio_format_amr_sub_fmt_t;

typedef enum {
    AUDIO_FORMAT_AAC_SUB_MAIN            = 0x1,
    AUDIO_FORMAT_AAC_SUB_LC              = 0x2,
    AUDIO_FORMAT_AAC_SUB_SSR             = 0x4,
    AUDIO_FORMAT_AAC_SUB_LTP             = 0x8,
    AUDIO_FORMAT_AAC_SUB_HE_V1           = 0x10,
    AUDIO_FORMAT_AAC_SUB_SCALABLE        = 0x20,
    AUDIO_FORMAT_AAC_SUB_ERLC            = 0x40,
    AUDIO_FORMAT_AAC_SUB_LD              = 0x80,
    AUDIO_FORMAT_AAC_SUB_HE_V2           = 0x100,
    AUDIO_FORMAT_AAC_SUB_ELD             = 0x200,
} audio_format_aac_sub_fmt_t;

typedef enum {
    AUDIO_FORMAT_VORBIS_SUB_NONE         = 0x0,
} audio_format_vorbis_sub_fmt_t;

typedef enum {
    AUDIO_FORMAT_INVALID             = 0xFFFFFFFFUL,
    AUDIO_FORMAT_DEFAULT             = 0,
    AUDIO_FORMAT_PCM                 = 0x00000000UL, /* DO NOT CHANGE */
    AUDIO_FORMAT_MP3                 = 0x01000000UL,
    AUDIO_FORMAT_AMR_NB              = 0x02000000UL,
    AUDIO_FORMAT_AMR_WB              = 0x03000000UL,
    AUDIO_FORMAT_AAC                 = 0x04000000UL,
    AUDIO_FORMAT_HE_AAC_V1           = 0x05000000UL,
    AUDIO_FORMAT_HE_AAC_V2           = 0x06000000UL,
    AUDIO_FORMAT_VORBIS              = 0x07000000UL,
    AUDIO_FORMAT_OPUS                = 0x08000000UL,
    AUDIO_FORMAT_AC3                 = 0x09000000UL,
    AUDIO_FORMAT_E_AC3               = 0x0A000000UL,
    AUDIO_FORMAT_DTS                 = 0x0B000000UL,
    AUDIO_FORMAT_DTS_HD              = 0x0C000000UL,
    AUDIO_FORMAT_TRUEHD              = 0x0D000000UL,
    AUDIO_FORMAT_MAIN_MASK           = 0xFF000000UL,
    AUDIO_FORMAT_SUB_MASK            = 0x00FFFFFFUL,

    /* Aliases */
    /* note != AudioFormat.ENCODING_PCM_16BIT */
    AUDIO_FORMAT_PCM_16_BIT          = (AUDIO_FORMAT_PCM |
                                        AUDIO_FORMAT_PCM_SUB_16_BIT),
    /* note != AudioFormat.ENCODING_PCM_8BIT */
    AUDIO_FORMAT_PCM_8_BIT           = (AUDIO_FORMAT_PCM |
                                        AUDIO_FORMAT_PCM_SUB_8_BIT),
    AUDIO_FORMAT_PCM_32_BIT          = (AUDIO_FORMAT_PCM |
                                        AUDIO_FORMAT_PCM_SUB_32_BIT),
    AUDIO_FORMAT_PCM_8_24_BIT        = (AUDIO_FORMAT_PCM |
                                        AUDIO_FORMAT_PCM_SUB_8_24_BIT),
    AUDIO_FORMAT_PCM_FLOAT           = (AUDIO_FORMAT_PCM |
                                        AUDIO_FORMAT_PCM_SUB_FLOAT),
    AUDIO_FORMAT_PCM_24_BIT_PACKED   = (AUDIO_FORMAT_PCM |
                                        AUDIO_FORMAT_PCM_SUB_24_BIT_PACKED),
    AUDIO_FORMAT_AAC_MAIN            = (AUDIO_FORMAT_AAC |
                                        AUDIO_FORMAT_AAC_SUB_MAIN),
    AUDIO_FORMAT_AAC_LC              = (AUDIO_FORMAT_AAC |
                                        AUDIO_FORMAT_AAC_SUB_LC),
    AUDIO_FORMAT_AAC_SSR             = (AUDIO_FORMAT_AAC |
                                        AUDIO_FORMAT_AAC_SUB_SSR),
    AUDIO_FORMAT_AAC_LTP             = (AUDIO_FORMAT_AAC |
                                        AUDIO_FORMAT_AAC_SUB_LTP),
    AUDIO_FORMAT_AAC_HE_V1           = (AUDIO_FORMAT_AAC |
                                        AUDIO_FORMAT_AAC_SUB_HE_V1),
    AUDIO_FORMAT_AAC_SCALABLE        = (AUDIO_FORMAT_AAC |
                                        AUDIO_FORMAT_AAC_SUB_SCALABLE),
    AUDIO_FORMAT_AAC_ERLC            = (AUDIO_FORMAT_AAC |
                                        AUDIO_FORMAT_AAC_SUB_ERLC),
    AUDIO_FORMAT_AAC_LD              = (AUDIO_FORMAT_AAC |
                                        AUDIO_FORMAT_AAC_SUB_LD),
    AUDIO_FORMAT_AAC_HE_V2           = (AUDIO_FORMAT_AAC |
                                        AUDIO_FORMAT_AAC_SUB_HE_V2),
    AUDIO_FORMAT_AAC_ELD             = (AUDIO_FORMAT_AAC |
                                        AUDIO_FORMAT_AAC_SUB_ELD),
} audio_format_t;

typedef enum {
    /* output channels */
    AUDIO_CHANNEL_OUT_FRONT_LEFT            = 0x1,
    AUDIO_CHANNEL_OUT_FRONT_RIGHT           = 0x2,
    AUDIO_CHANNEL_OUT_FRONT_CENTER          = 0x4,
    AUDIO_CHANNEL_OUT_LOW_FREQUENCY         = 0x8,
    AUDIO_CHANNEL_OUT_BACK_LEFT             = 0x10,
    AUDIO_CHANNEL_OUT_BACK_RIGHT            = 0x20,
    AUDIO_CHANNEL_OUT_FRONT_LEFT_OF_CENTER  = 0x40,
    AUDIO_CHANNEL_OUT_FRONT_RIGHT_OF_CENTER = 0x80,
    AUDIO_CHANNEL_OUT_BACK_CENTER           = 0x100,
    AUDIO_CHANNEL_OUT_SIDE_LEFT             = 0x200,
    AUDIO_CHANNEL_OUT_SIDE_RIGHT            = 0x400,
    AUDIO_CHANNEL_OUT_TOP_CENTER            = 0x800,
    AUDIO_CHANNEL_OUT_TOP_FRONT_LEFT        = 0x1000,
    AUDIO_CHANNEL_OUT_TOP_FRONT_CENTER      = 0x2000,
    AUDIO_CHANNEL_OUT_TOP_FRONT_RIGHT       = 0x4000,
    AUDIO_CHANNEL_OUT_TOP_BACK_LEFT         = 0x8000,
    AUDIO_CHANNEL_OUT_TOP_BACK_CENTER       = 0x10000,
    AUDIO_CHANNEL_OUT_TOP_BACK_RIGHT        = 0x20000,

    AUDIO_CHANNEL_OUT_MONO     = AUDIO_CHANNEL_OUT_FRONT_LEFT,
    AUDIO_CHANNEL_OUT_STEREO   = (AUDIO_CHANNEL_OUT_FRONT_LEFT |
                                  AUDIO_CHANNEL_OUT_FRONT_RIGHT),
    AUDIO_CHANNEL_OUT_QUAD     = (AUDIO_CHANNEL_OUT_FRONT_LEFT |
                                  AUDIO_CHANNEL_OUT_FRONT_RIGHT |
                                  AUDIO_CHANNEL_OUT_BACK_LEFT |
                                  AUDIO_CHANNEL_OUT_BACK_RIGHT),
    AUDIO_CHANNEL_OUT_SURROUND = (AUDIO_CHANNEL_OUT_FRONT_LEFT |
                                  AUDIO_CHANNEL_OUT_FRONT_RIGHT |
                                  AUDIO_CHANNEL_OUT_FRONT_CENTER |
                                  AUDIO_CHANNEL_OUT_BACK_CENTER),
    AUDIO_CHANNEL_OUT_5POINT1  = (AUDIO_CHANNEL_OUT_FRONT_LEFT |
                                  AUDIO_CHANNEL_OUT_FRONT_RIGHT |
                                  AUDIO_CHANNEL_OUT_FRONT_CENTER |
                                  AUDIO_CHANNEL_OUT_LOW_FREQUENCY |
                                  AUDIO_CHANNEL_OUT_BACK_LEFT |
                                  AUDIO_CHANNEL_OUT_BACK_RIGHT),
    // matches the correct AudioFormat.CHANNEL_OUT_7POINT1_SURROUND definition for 7.1
    AUDIO_CHANNEL_OUT_7POINT1  = (AUDIO_CHANNEL_OUT_FRONT_LEFT |
                                  AUDIO_CHANNEL_OUT_FRONT_RIGHT |
                                  AUDIO_CHANNEL_OUT_FRONT_CENTER |
                                  AUDIO_CHANNEL_OUT_LOW_FREQUENCY |
                                  AUDIO_CHANNEL_OUT_BACK_LEFT |
                                  AUDIO_CHANNEL_OUT_BACK_RIGHT |
                                  AUDIO_CHANNEL_OUT_SIDE_LEFT |
                                  AUDIO_CHANNEL_OUT_SIDE_RIGHT),
    AUDIO_CHANNEL_OUT_ALL      = (AUDIO_CHANNEL_OUT_FRONT_LEFT |
                                  AUDIO_CHANNEL_OUT_FRONT_RIGHT |
                                  AUDIO_CHANNEL_OUT_FRONT_CENTER |
                                  AUDIO_CHANNEL_OUT_LOW_FREQUENCY |
                                  AUDIO_CHANNEL_OUT_BACK_LEFT |
                                  AUDIO_CHANNEL_OUT_BACK_RIGHT |
                                  AUDIO_CHANNEL_OUT_FRONT_LEFT_OF_CENTER |
                                  AUDIO_CHANNEL_OUT_FRONT_RIGHT_OF_CENTER |
                                  AUDIO_CHANNEL_OUT_BACK_CENTER|
                                  AUDIO_CHANNEL_OUT_SIDE_LEFT|
                                  AUDIO_CHANNEL_OUT_SIDE_RIGHT|
                                  AUDIO_CHANNEL_OUT_TOP_CENTER|
                                  AUDIO_CHANNEL_OUT_TOP_FRONT_LEFT|
                                  AUDIO_CHANNEL_OUT_TOP_FRONT_CENTER|
                                  AUDIO_CHANNEL_OUT_TOP_FRONT_RIGHT|
                                  AUDIO_CHANNEL_OUT_TOP_BACK_LEFT|
                                  AUDIO_CHANNEL_OUT_TOP_BACK_CENTER|
                                  AUDIO_CHANNEL_OUT_TOP_BACK_RIGHT),

    /* input channels */
    AUDIO_CHANNEL_IN_LEFT            = 0x4,
    AUDIO_CHANNEL_IN_RIGHT           = 0x8,
    AUDIO_CHANNEL_IN_FRONT           = 0x10,
    AUDIO_CHANNEL_IN_BACK            = 0x20,
    AUDIO_CHANNEL_IN_LEFT_PROCESSED  = 0x40,
    AUDIO_CHANNEL_IN_RIGHT_PROCESSED = 0x80,
    AUDIO_CHANNEL_IN_FRONT_PROCESSED = 0x100,
    AUDIO_CHANNEL_IN_BACK_PROCESSED  = 0x200,
    AUDIO_CHANNEL_IN_PRESSURE        = 0x400,
    AUDIO_CHANNEL_IN_X_AXIS          = 0x800,
    AUDIO_CHANNEL_IN_Y_AXIS          = 0x1000,
    AUDIO_CHANNEL_IN_Z_AXIS          = 0x2000,
    AUDIO_CHANNEL_IN_VOICE_UPLINK    = 0x4000,
    AUDIO_CHANNEL_IN_VOICE_DNLINK    = 0x8000,

    AUDIO_CHANNEL_IN_MONO   = AUDIO_CHANNEL_IN_FRONT,
    AUDIO_CHANNEL_IN_STEREO = (AUDIO_CHANNEL_IN_LEFT | AUDIO_CHANNEL_IN_RIGHT),
    AUDIO_CHANNEL_IN_ALL    = (AUDIO_CHANNEL_IN_LEFT |
                               AUDIO_CHANNEL_IN_RIGHT |
                               AUDIO_CHANNEL_IN_FRONT |
                               AUDIO_CHANNEL_IN_BACK|
                               AUDIO_CHANNEL_IN_LEFT_PROCESSED |
                               AUDIO_CHANNEL_IN_RIGHT_PROCESSED |
                               AUDIO_CHANNEL_IN_FRONT_PROCESSED |
                               AUDIO_CHANNEL_IN_BACK_PROCESSED|
                               AUDIO_CHANNEL_IN_PRESSURE |
                               AUDIO_CHANNEL_IN_X_AXIS |
                               AUDIO_CHANNEL_IN_Y_AXIS |
                               AUDIO_CHANNEL_IN_Z_AXIS |
                               AUDIO_CHANNEL_IN_VOICE_UPLINK |
                               AUDIO_CHANNEL_IN_VOICE_DNLINK),
} audio_channels_t;

typedef enum {
    AUDIO_MODE_INVALID          = -2,
    AUDIO_MODE_CURRENT          = -1,
    AUDIO_MODE_NORMAL           = 0,
    AUDIO_MODE_RINGTONE         = 1,
    AUDIO_MODE_IN_CALL          = 2,
    AUDIO_MODE_IN_COMMUNICATION = 3,

    AUDIO_MODE_CNT,
    AUDIO_MODE_MAX              = AUDIO_MODE_CNT - 1,
} audio_mode_t;

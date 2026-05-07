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
#include "stream.h"
#include "stream_sync.h"
#include "debug.h"
#include "atime.h"
#include "util.h"
#include "file.h"
#include "sysfs_ll.h"
#include "device_config.h"

#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>

#ifdef CONFIG_ANDROID
#include "androidndk_utils.h"
#endif

#define DBGCA2 DBG_IF(Debug[DBG_CA] > 1)
#define DBGS   DBG_IF(Debug[DBG_STREAM])

// Forward declaration for AC3 recoding check
extern int libavos_get_ac3_recoding_enabled(void);

// check if bit at position in value is 1
#define CHECK_BIT(value,position) (((value)>>(position)) & 1)

// audio format flag
#define ENCODING_INVALID                0
#define ENCODING_DEFAULT                1
#define ENCODING_PCM_16BIT              2
#define ENCODING_PCM_8BIT               3
#define ENCODING_PCM_FLOAT              4
#define ENCODING_AC3                    5
#define ENCODING_E_AC3                  6
#define ENCODING_DTS                    7
#define ENCODING_DTS_HD                 8
#define ENCODING_MP3                    9
#define ENCODING_AAC_LC                 10
#define ENCODING_AAC_HE_V1              11
#define ENCODING_AAC_HE_V2              12
#define ENCODING_IEC61937               13
#define ENCODING_DOLBY_TRUEHD           14
#define ENCODING_AAC_ELD                15
#define ENCODING_AAC_XHE                16
#define ENCODING_AC4                    17
#define ENCODING_E_AC3_JOC              18
#define ENCODING_DOLBY_MAT              19
#define ENCODING_OPUS                   20
#define ENCODING_PCM_24BIT_PACKED       21
#define ENCODING_PCM_32BIT              22
#define ENCODING_MPEGH_BL_L3            23
#define ENCODING_MPEGH_BL_L4            24
#define ENCODING_MPEGH_LC_L3            25
#define ENCODING_MPEGH_LC_L4            26
#define ENCODING_DTS_UHD                27
#define ENCODING_DRA                    28
#define ENCODING_DTS_HD_MA              29
#define ENCODING_DTS_UHD_P2             30
#define ENCODING_DSD                    31

typedef struct {
	char buf[64*1024];
	int pos;
} buf_t;

static int passthrough_on = -1;
static AVFormatContext *fctxt;
static buf_t b;
static AVCodecParserContext *aparser;
static AVCodecContext *avctx;

static long hdmi_audio_codecs_flag = 0; // supported audio codecs by AV receiver via HDMI

static int _spdif_frame_samples(const AUDIO_PROPERTIES *a, int passthrough_mode)
{
	// DTS frame duration is not fixed at 1536 samples. In mode 2 the raw
	// AudioTrack path consumes parser output directly, so use the parser's
	// duration for the logical clock when available. Forcing 1536 on 512-sample
	// DTS frames advances AVOS audio time 3x too fast and causes large A/V drift.
	if (passthrough_mode == 2 && a && (a->format == WAVE_FORMAT_DTS ||
		    a->format == WAVE_FORMAT_DTS_HD ||
		    a->format == WAVE_FORMAT_DTS_HD_MA)) {
		if (aparser && aparser->duration > 0) {
			return aparser->duration;
		}
		if (avctx && avctx->frame_size > 0) {
			return avctx->frame_size;
		}
		return 512;
	}

	// 1. Primary: codec-specific context metadata (avctx.frame_size).
	if (avctx && avctx->frame_size > 0) {
		return avctx->frame_size;
	}

	// 2. Secondary: parser-derived logical duration.
	// Most FFmpeg parsers (MLP/TrueHD, EAC3) populate this correctly.
	if (aparser && aparser->duration > 0) {
		return aparser->duration;
	}

	// 3. Last resort: logical base units for known formats.
	// These represent the standard logical frame duration regardless of compression ratio.
	if (a) {
		if (a->format == WAVE_FORMAT_AC3 ||
		    a->format == WAVE_FORMAT_EAC3 ||
		    a->format == WAVE_FORMAT_E_AC3_JOC) {
			return 1536; // 32ms at 48kHz
		}
		if (a->format == WAVE_FORMAT_TRUEHD) {
			return 1280; // 26.6ms logical major sync base (common parser unit)
		}
	}

	return 0;
}

// Detailed version that captures decision path for debug logging
static int _spdif_frame_samples_debug(const AUDIO_PROPERTIES *a, int passthrough_mode, int *avctx_size, int *parser_duration, int *fallback_samples)
{
	*avctx_size = 0;
	*parser_duration = 0;
	*fallback_samples = 0;

	// Mode 2 DTS uses raw parser output as the AudioTrack write unit, so its
	// logical duration must follow the parser. DTS commonly uses 512-sample
	// frames; treating every frame as 1536 samples over-advances audio time.
	if (passthrough_mode == 2 && a && (a->format == WAVE_FORMAT_DTS ||
		    a->format == WAVE_FORMAT_DTS_HD ||
		    a->format == WAVE_FORMAT_DTS_HD_MA)) {
		if (aparser && aparser->duration > 0) {
			*parser_duration = aparser->duration;
			return aparser->duration;
		}
		if (avctx && avctx->frame_size > 0) {
			*avctx_size = avctx->frame_size;
			return avctx->frame_size;
		}
		*fallback_samples = 512;
		return 512;
	}

	// 1. Primary: codec-specific context metadata (avctx.frame_size).
	if (avctx && avctx->frame_size > 0) {
		*avctx_size = avctx->frame_size;
		return avctx->frame_size;
	}

	// 2. Secondary: parser-derived logical duration.
	if (aparser && aparser->duration > 0) {
		*parser_duration = aparser->duration;
		return aparser->duration;
	}

	// 3. Last resort: logical base units for known formats.
	if (a) {
		if (a->format == WAVE_FORMAT_AC3 ||
		    a->format == WAVE_FORMAT_EAC3 ||
		    a->format == WAVE_FORMAT_E_AC3_JOC) {
			*fallback_samples = 1536;
			return 1536;
		}
		if (a->format == WAVE_FORMAT_TRUEHD) {
			*fallback_samples = 1280;
			return 1280;
		}
	}

	return 0;
}

static int spdif_put( UCHAR *data, int size, int *decoded )
{
	if ( !data || size < 10 )
		return 0;

	// Prevent segfault if format context is not initialized
	if ( !fctxt ) {
		serprintf("spdif_put: format context not initialized\n");
		return 0;
	}

	AVPacket pkt;
	av_init_packet( &pkt );
	static int pts = 1;

	pkt.pts  = pts++;
	pkt.data = data;
	pkt.size = size;

	*decoded = size;

	av_write_frame( fctxt, &pkt );
	return 0;
}

static int spdif_get( AUDIO_FRAME *frame )
{
	frame->error  = 0;
	frame->size   = 0;
	frame->format = WAVE_FORMAT_UNKNOWN;

	if ( !b.pos )
		return 0;
	frame->data = b.buf;
	frame->size = b.pos;

	// Mode 2 now uses raw data path (spdif_fakeget) and doesn't call spdif_get,
	// so this function is only used for mode 1 (IEC61937 wrapping) and AC3 recoding.
	// No fakeSize scaling needed since IEC bursts already have correct size.
	frame->fakeSize = b.pos;

	b.pos = 0;
	return 0;
}

int spdif_encapsulate( AUDIO_PROPERTIES *a, UCHAR *data, int size, AUDIO_FRAME *frame, int *decoded )
{	
	if (!size)
		return 0;

	// AC3 recoding can intentionally run without parser (encoder already emits full syncframes).
	// In passthrough mode 2, the sink expects raw codec frames (ENCODING_AC3/E_AC3/DTS),
	// not IEC61937-wrapped bursts. Bypass mux wrapping when parser is absent.
	// Guard on AC3 format since fakeSize assumes 1536 samples/frame (AC3-specific).
	// Note: if future recoding paths emit non-AC3 compressed frames in mode 2 with
	// !aparser, this branch must be extended with codec-specific timing instead of
	// falling back to IEC wrapping.
	if (passthrough_on == 2 && !aparser && libavos_get_ac3_recoding_enabled() && a->format == WAVE_FORMAT_AC3) {
		frame->data = data;
		frame->size = size;
		frame->error = 0;
		frame->format = a->format;
		{
			int samples = _spdif_frame_samples(a, passthrough_on);
			frame->fakeSize = samples > 0 ? samples * a->bytesPerFrame : size;
		}
		*decoded = size;
		DBGCA2 serprintf("Mode 2 (no parser): raw data, format=%04X size=%d, fakeSize=%d, bpf=%d, rate=%d, recode=1\n",
		                 frame->format, frame->size, frame->fakeSize, a->bytesPerFrame, a->samplesPerSec);
		return 0;
	}

DBGCA2 serprintf("spdif_encapsulate %5d", size );
	if( aparser ) {
		unsigned char *out = NULL;
		int out_size;
		if (avctx) {
			if (avctx->sample_rate == 0) avctx->sample_rate = a->samplesPerSec;
			if (avctx->ch_layout.nb_channels == 0) av_channel_layout_default(&avctx->ch_layout, a->channels);
		}
		int parsed = av_parser_parse2( 	aparser, avctx, 
						&out, &out_size, 
						data, size,
						AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0 );
DBGCA2 serprintf("  parsed %5d/%5d\n", parsed, out_size );
		*decoded = parsed;

		if (passthrough_on == 2) {
			// Mode 2: Send raw compressed data to Android, let it handle encapsulation
			// Skip IEC wrapping - Android uses codec-specific encodings (ENCODING_AC3/E_AC3/DTS)

			// Guard against zero-length parser output (incomplete frame)
			if (out_size == 0 || !out) {
				frame->size = 0;
				frame->error = 0;
				DBGCA2 serprintf("Mode 2: zero-length parser output, skipping\n");
				return 0;
			}

			frame->data = out;
			frame->size = out_size;
			frame->error = 0;
			frame->format = a->format;  // Preserve codec ID (AC3/EAC3/DTS) for downstream logic

			// fakeSize carries PCM-equivalent duration for timing. DTS mode 2 follows
			// parser duration because the raw write unit can be 512 samples; AC3/EAC3
			// and TrueHD keep their codec base-unit fallbacks when parser metadata is absent.
			{
				int avctx_size, parser_duration, fallback_samples;
				int samples = _spdif_frame_samples_debug(a, passthrough_on, &avctx_size, &parser_duration, &fallback_samples);
				if (samples > 0) {
					DBGCA2 serprintf("Mode 2 timing: format=%04X samples=%d fakeSize=%d avctx_size=%d parser_dur=%d fallback=%d\n",
						a->format, samples, samples * a->bytesPerFrame, avctx_size, parser_duration, fallback_samples);
					frame->fakeSize = samples * a->bytesPerFrame;
				} else {
					// Fallback to physical size if parser is blind
					frame->fakeSize = out_size;
				}
			}
			DBGCA2 serprintf("Mode 2: raw data, format=%04X size=%d, fakeSize=%d, bytesPerFrame=%d, rate=%d, parser=1\n",
			                 frame->format, frame->size, frame->fakeSize, a->bytesPerFrame, a->samplesPerSec);
		} else {
			// Mode 1: Manual IEC61937 wrapping via FFmpeg SPDIF muxer
			int dummy;
			spdif_put( out, out_size, &dummy );
			spdif_get( frame );
			{
				int samples = _spdif_frame_samples(a, passthrough_on);
				if (samples > 0) {
					frame->fakeSize = samples * a->bytesPerFrame;
				}
			}
			DBGCA2 serprintf("Mode 1: IEC wrapped, size=%d, fakeSize=%d format=%04X bpf=%d rate=%d\n",
				frame->size, frame->fakeSize, a->format, a->bytesPerFrame, a->samplesPerSec);
		}
		return 0;
	}
DBGCA2 serprintf("\n", size );

	spdif_put( data, size, decoded );
	spdif_get( frame );
	return 0;
}

static int spdif_free( void )
{
	if( aparser ) {
		av_parser_close( aparser );
		aparser = NULL;
	}
	if (fctxt) {
		avformat_free_context(fctxt);
		fctxt = NULL;
	}
	if (avctx) {
		avcodec_free_context(&avctx);
		avctx = NULL;
	}
	// Clear buffer position to prevent stale data
	b.pos = 0;
	memset(&b, 0, sizeof(b));

	return 0;
}

static int buf_write( void *opaque, const uint8_t *buf, int size )
{
	buf_t *b = ( buf_t* ) opaque;
	if ( b->pos + size > sizeof( b->buf ) ) {
		return 0;
	}
	memcpy( b->buf + b->pos, buf, size );
	b->pos += size;
	return size;
}

static int wave2libav_codecid( int codecid )
{
	switch ( codecid ) {
	case WAVE_FORMAT_AC3:
		// some TVs only declare EAC3 but can do AC3
		if(CHECK_BIT(hdmi_audio_codecs_flag, ENCODING_AC3) | CHECK_BIT(hdmi_audio_codecs_flag, ENCODING_E_AC3)) {
			serprintf("AC3 encoding passthrough supported\n");
			return AV_CODEC_ID_AC3;
		} else {
			serprintf("AC3 encoding passthrough NOT supported\n");
			return 0;
		}
	case WAVE_FORMAT_EAC3:
	case WAVE_FORMAT_E_AC3_JOC:
		if(CHECK_BIT(hdmi_audio_codecs_flag, ENCODING_E_AC3)) {
			serprintf("EAC3 encoding passthrough supported\n");
			return AV_CODEC_ID_EAC3;
		} else {
			serprintf("EAC3 encoding passthrough NOT supported\n");
			return 0;
		}
	case WAVE_FORMAT_DTS_HD_MA:
	case WAVE_FORMAT_DTS_HD:
	case WAVE_FORMAT_DTS:
		if(CHECK_BIT(hdmi_audio_codecs_flag, ENCODING_DTS)) {
			serprintf("DTS encoding passthrough supported\n");
			return AV_CODEC_ID_DTS;
		} else {
			serprintf("DTS encoding passthrough NOT supported\n");
			return 0;
		}
	case WAVE_FORMAT_TRUEHD:
		if(CHECK_BIT(hdmi_audio_codecs_flag, ENCODING_DOLBY_TRUEHD)) {
			serprintf("TRUEHD encoding passthrough supported\n");
			return AV_CODEC_ID_TRUEHD;
		} else {
			serprintf("TRUEHD encoding passthrough NOT supported\n");
			return 0;
		}
	default:
		return 0;
	}
}

#ifdef CONFIG_ANDROID
static int spdif_check( int codecid )
{
DBGS serprintf( "spdif_check, check codecid %d, force %d\n", codecid, passthrough_on);
	if ( !wave2libav_codecid( codecid ) ) {
		serprintf("codec not supported for passthrough...\n" );
		return 0;
	}

	return passthrough_on;
}
#else
static int spdif_check(int codecid) 
{
DBGS serprintf( "spdif_check, force %d\n", passthrough_on);
	return (passthrough_on == 1);
}
#endif

void set_hdmi_supported_audio_codecs(long flag)
{
	hdmi_audio_codecs_flag = flag;
}

long get_hdmi_supported_audio_codecs()
{
	return hdmi_audio_codecs_flag;
}

// Supporting one of DTS-HD or TrueHD is a likely indicator for supporting IEC61937 8ch 192khz
int get_hdmi_supports_iec_8ch192khz() {
    return CHECK_BIT(get_hdmi_supported_audio_codecs(), ENCODING_DTS_HD) ||
                CHECK_BIT(get_hdmi_supported_audio_codecs(), ENCODING_DOLBY_TRUEHD);
}

int get_hdmi_supports_iec() {
    return CHECK_BIT(get_hdmi_supported_audio_codecs(), ENCODING_IEC61937);
}

int spdif_init( AUDIO_PROPERTIES *a )
{
DBGS serprintf( "spdif_init\n");
	int codecid = a->format;
	spdif_free();

	if ( !spdif_check( codecid ) )
		return 0;

	const AVOutputFormat *fmt = av_guess_format( "spdif", NULL, NULL );
	if ( !fmt ) {
		serprintf( "No spdif avformat...\n" );
		return 0;
	}

	fctxt = avformat_alloc_context(  );
	fctxt->oformat = fmt;

	fctxt->pb = avio_alloc_context( b.buf, sizeof( b.buf ), AVIO_FLAG_WRITE, ( void* ) &b, NULL, buf_write, NULL );
	fctxt->pb->seekable = 0;
	fctxt->flags |= AVFMT_NOFILE | AVFMT_FLAG_IGNIDX;

	AVStream *stream = avformat_new_stream( fctxt, NULL );
	stream->id = 1;
	stream->codecpar->codec_id = wave2libav_codecid( codecid );
	stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
	stream->codecpar->sample_rate = a->samplesPerSec;
	if ( avformat_write_header( fctxt, NULL ) < 0 )
		return 0;

	// try to get a parser for this codec
	// Skip parser for AC3 recoding - the encoder already produces complete syncframes
	avctx = avcodec_alloc_context3(NULL);
	if (avctx) {
		avcodec_parameters_to_context(avctx, stream->codecpar);
	}
	if ( !libavos_get_ac3_recoding_enabled() ) {
		aparser = av_parser_init(stream->codecpar->codec_id);
		if( !aparser ) {
serprintf("cannot open parser for %04X\r\n", stream->codecpar->codec_id );
		}
	} else {
		aparser = NULL;
		DBGS serprintf("spdif_init: skipping parser for AC3 recoding (encoder produces complete frames)\n");
	}

	const char *dtsrate = NULL;
    if (a->codec_id == WAVE_FORMAT_DTS_HD_MA || a->codec_id == WAVE_FORMAT_DTS_HD) {
        if (get_hdmi_supports_iec_8ch192khz()) {
            dtsrate = "dtshd_rate=768000";
        } else {
            dtsrate = "dtshd_rate=0";
        }
    }
	if (dtsrate && av_set_options_string( &fctxt->av_class, dtsrate, "=", ":" ) < 0 )
		serprintf( "Failed2 setting dtshd rate to %s\n", dtsrate );

	return 1;
}

static int spdif_new( AUDIO_PROPERTIES *audio )
{
	if ( !audio )
		return 1;

	return 0;
}

static int spdif_open( AUDIO_PROPERTIES *audio )
{
DBGS serprintf( "spdif_open\n");
DBGS serprintf("audio format is %d, %d channels, %dkHz, %d bits, %d B/s, %d B/f\n", audio->format, audio->channels, audio->samplesPerSec/1000, audio->bitsPerSample, audio->bytesPerSec, audio->bytesPerFrame);

	int codecid = audio->format;

	if (!aparser && !libavos_get_ac3_recoding_enabled()) {
		aparser = av_parser_init(wave2libav_codecid(codecid));
		if (!aparser) {
DBGS			serprintf("cannot open parser for %04X\r\n", codecid );
		} else {
			if (!avctx) {
				avctx = avcodec_alloc_context3(NULL);
			}
			if (avctx) {
				if (avctx->sample_rate == 0) avctx->sample_rate = audio->samplesPerSec;
				if (avctx->ch_layout.nb_channels == 0) av_channel_layout_default(&avctx->ch_layout, audio->channels);
			}
		}
	}

	// Mode 1 (IEC61937 wrapping): Apply IEC-specific rate/channel/bit-depth adjustments
	// Mode 2 (raw data to Android): Must match AudioTrack configuration for timing sync
	if (passthrough_on == 1) {
		int orig_rate = audio->samplesPerSec;
		// IEC61937 container is always 16-bit, 2-channel stereo (or 8ch for high-bitrate)
		audio->bitsPerSample = 16;
		audio->channels = 2;
		audio->samplesPerSec = 48000;

		switch (codecid) {
		case WAVE_FORMAT_EAC3:
		case WAVE_FORMAT_E_AC3_JOC:
			// EAC3 IEC61937 requires 192kHz container
			audio->samplesPerSec = 192000;
			break;
		case WAVE_FORMAT_DTS_HD:
		case WAVE_FORMAT_DTS_HD_MA:
		case WAVE_FORMAT_TRUEHD:
			// High-bitrate formats need higher IEC rate
			audio->channels      = 8;
			audio->samplesPerSec = 192000;
			break;
		case WAVE_FORMAT_DTS:
		case WAVE_FORMAT_AC3:
		default:
			// Standard formats use 48kHz IEC container, but keep 32/44.1kHz streams at
			// their native rate to avoid timing drift in passthrough mode 1.
			if (orig_rate == 32000 || orig_rate == 44100) {
				audio->samplesPerSec = orig_rate;
			} else {
				audio->samplesPerSec = 48000;
			}
			break;
		}
		// Recalculate bytesPerFrame for IEC container
		audio->bytesPerFrame = audio->channels * audio->bitsPerSample / 8;
		audio->bytesPerSec = audio->samplesPerSec * audio->bytesPerFrame;
	} else if (passthrough_on == 2) {
		// Mode 2: Keep content sample rate for timing, but match channel/bit-depth to AudioTrack
		// Android handles the actual container format when using codec-specific encodings
		audio->bitsPerSample = 16;
		audio->channels = 2;
		// Keep original samplesPerSec from demuxer (typically 48kHz for AC3/EAC3/DTS content)
		// Don't force to 192kHz - that's an IEC container requirement, not needed for codec-specific encodings

		// Recalculate bytesPerFrame based on the channel/bit-depth
		audio->bytesPerFrame = audio->channels * audio->bitsPerSample / 8;
		audio->bytesPerSec = audio->samplesPerSec * audio->bytesPerFrame;
	}

DBGS serprintf("spdif_open: mode=%d final: %dch, %dHz, %dbits, %dB/f\n",
               passthrough_on, audio->channels, audio->samplesPerSec, audio->bitsPerSample, audio->bytesPerFrame);

	return 0;
}

static int spdif_close( AUDIO_PROPERTIES *audio )
{
DBGS serprintf( "spdif_close\n");
	spdif_free();
	return 0;
}

static int spdif_decode( AUDIO_PROPERTIES *audio, UCHAR *data, int size, AUDIO_FRAME *avos_frame, int *_decoded, int *_time )
{
	*_decoded = 0;
	return 0;
}

static int spdif_flush( AUDIO_PROPERTIES *audio )
{
	return 0;
}

static int spdif_delay( AUDIO_PROPERTIES *audio )
{
	return 0;
}

static int spdif_get_rc( AUDIO_PROPERTIES *audio, STREAM_RC *rc )
{
	if ( !rc )
		return 1;
	memset( rc, 0, sizeof( STREAM_RC ) );
	return 0;
}

static int spdif_is_supported( AUDIO_PROPERTIES *audio )
{
	// When AC3 recoding is enabled (mode 3), ALL audio formats must be decoded to PCM
	// using FFmpeg decoder, then filtered, then re-encoded to AC3.
	// SPDIF decoder cannot decode to PCM (it only does passthrough encapsulation),
	// so we must reject SPDIF decoder selection in AC3 recoding mode.
	if (libavos_get_ac3_recoding_enabled()) {
		DBGS serprintf("spdif_is_supported: AC3 recoding enabled, forcing FFmpeg decoder for format %04X\n", audio->format);
		return 0;
	}
	return spdif_check( audio->format );
}

static int spdif_delete( AUDIO_PROPERTIES *audio )
{
	return 0;
}

void spdif_set_passthrough( int on )
{
	passthrough_on = on;
}

int spdif_is_passthrough_on()
{
	return passthrough_on;
}

int spdif_format_passthrough_supported(int format)
{
#ifdef CONFIG_ANDROID
	switch( format ) {
	case WAVE_FORMAT_AC3:
		return CHECK_BIT(hdmi_audio_codecs_flag, ENCODING_AC3) ||
		       CHECK_BIT(hdmi_audio_codecs_flag, ENCODING_E_AC3);
	case WAVE_FORMAT_EAC3:
	case WAVE_FORMAT_E_AC3_JOC:
		return CHECK_BIT(hdmi_audio_codecs_flag, ENCODING_E_AC3);
	case WAVE_FORMAT_DTS_HD_MA:
		return CHECK_BIT(hdmi_audio_codecs_flag, ENCODING_DTS_HD_MA) ||
		       CHECK_BIT(hdmi_audio_codecs_flag, ENCODING_DTS_HD);
	case WAVE_FORMAT_DTS_HD:
		return CHECK_BIT(hdmi_audio_codecs_flag, ENCODING_DTS_HD);
	case WAVE_FORMAT_DTS:
		return CHECK_BIT(hdmi_audio_codecs_flag, ENCODING_DTS);
	case WAVE_FORMAT_TRUEHD:
		return CHECK_BIT(hdmi_audio_codecs_flag, ENCODING_DOLBY_TRUEHD);
	default:
		return 0;
	}
#else
	(void)format;
	return 1;
#endif
}

static STREAM_DEC_AUDIO stream_spdif = 
{
	.name    = "spdif",
	.new	 = spdif_new,
	.open    = spdif_open,
	.close   = spdif_close,
	.decode  = spdif_decode,
	.flush   = spdif_flush,
	.delay   = spdif_delay,
	.get_rc  = spdif_get_rc,
	.delete  = spdif_delete,
	.is_supported = spdif_is_supported,
};

static STREAM_REG_DEC_AUDIO reg_spdif_ac3		= { WAVE_FORMAT_AC3,	&stream_spdif, 6 };
static STREAM_REG_DEC_AUDIO reg_spdif_eac3              = { WAVE_FORMAT_EAC3,    &stream_spdif, 8 };
static STREAM_REG_DEC_AUDIO reg_spdif_eac3_joc          = { WAVE_FORMAT_E_AC3_JOC,&stream_spdif, 8 };
static STREAM_REG_DEC_AUDIO reg_spdif_dts		= { WAVE_FORMAT_DTS,	&stream_spdif, 8 };
static STREAM_REG_DEC_AUDIO reg_spdif_dts_hd		= { WAVE_FORMAT_DTS_HD,    &stream_spdif, 8 };
static STREAM_REG_DEC_AUDIO reg_spdif_truehd	= { WAVE_FORMAT_TRUEHD,	&stream_spdif, 8 };
static void register_spdif(void) __attribute__((constructor));
static void register_spdif(void) {
	stream_register_dec_audio_head( &reg_spdif_ac3);
	stream_register_dec_audio_head( &reg_spdif_eac3);
	stream_register_dec_audio_head( &reg_spdif_eac3_joc);
	stream_register_dec_audio_head( &reg_spdif_dts);
	stream_register_dec_audio_head( &reg_spdif_dts_hd);
	stream_register_dec_audio_head( &reg_spdif_truehd);
}

#ifdef DEBUG_MSG
DECLARE_DEBUG_PARAM("passthrough_on", passthrough_on );
#endif

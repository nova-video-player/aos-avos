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
#include "types.h"
#include "debug.h"
#include "pcm_autogain.h"

#ifdef ARM
#include "pcm_tools_arm.h"
#else
#include "pcm_tools_x86.h"
#endif

#include <stdlib.h>	/* for abs() */

#ifndef STANDALONE

#define DBG  if(Debug[DBG_AGC])
#define DBG2 if(Debug[DBG_AGC] > 1)
DECLARE_DEBUG_SWITCH("dbgagc", DBG_AGC);

#endif

#define ERR if (1)


/*****************************************************************************
 * AGC Parameters
 ****************************************************************************/

/* keep gain between 0dB and +12dB
   (256/256 = 0dB, 1024/256 = 4 = 12dB)
   the gain is coded in Q16.16 format, hence the << 16 */
#define _GAIN_MAX      (1024L << 16)
#define _GAIN_MIN      (256L  << 16)

/* Power threshold */
#define _POWER_THRESHOLD	100000000LL/4

/* Time constants */
/* Hold time */
#define _HOLD_TIME	50
/* Attack time */
#define _ATTACK_TIME	(650 * 150)
/* Release time */
#define _RELEASE_TIME	(650 * 5000)


/*****************************************************************************
 * AGC Internal
 ****************************************************************************/

static long long _agc_power_threshold	= _POWER_THRESHOLD;
static int _agc_current_gain		= _GAIN_MIN;
static int _agc_gain_incr		= 0;
static int _agc_gain_decr		= 0;
static int _agc_frameorder 		= 5;
static int _agc_framesize 		= 32;
static int _agc_hold_max		= 0;

/* The power is evaluated on the 2^_POWER_HISTORY_ORDER past frames */
#define _POWER_HISTORY_ORDER	3
#define _POWER_HISTORY		(1 << (_POWER_HISTORY_ORDER))
static long long _power_array[_POWER_HISTORY];
static int _power_idx	= 0;

static int _hold_current = 0;


/*****************************************************************************
 *
 * pcm_s16le_pga_limiter
 *
 * input:
 *	buf: 		pointer to sample buffer
 *	samples:	number of S16LE samples in buffer
 *	gain:		numerator of gain factor
 *
 * output:
 *	buf:		result is placed in same buffer
 *
 * behaviour:
 *
 * multiplies a buffer of S16LE samples by gain/256 and saturates the result.
 * gain has a valid range of 0 to 65536, reasonable range is 64 (-6dB) to
 * 25600 (+10dB).
 *
 ****************************************************************************/
__attribute__((unused))
static inline void pcm_s16le_pga_limiter( SHORT* buf, int samples, int gain )
{
	int i;

	for (i=0; i < samples; i++, buf++) {
		int tmp;

		tmp  = pcm_s16le_muldiv(*buf, gain, 8);
		*buf = pcm_limit_s16le(tmp);
	}
}

static const int clip_barrier = 0x7c00;

static inline int s32le_softclip_s16le(int sample, int barrier)
{
	static const int num_table[16] = {
		244, 232, 220, 208, 196, 184, 172, 160,
		148, 136, 124, 112, 100, 88, 76, 64
	};
	
	static const int bar_table[16] = {
		31744, 31747, 31753, 31762, 31774, 31789, 31807, 31828,
		31852, 31879, 31909, 31942, 31978, 32017, 32059, 32104,
	};

	int ovr;
	int num_idx;

	if (sample > 0)
		ovr = sample - barrier;
	else
		ovr = barrier + sample;

	num_idx = (abs(ovr) >> 6) & 15;
	
DBG2	serprintf("%8i %8i %4i ", sample, ovr, num_idx);

	ovr = (ovr * num_table[num_idx]) >> 8;
	barrier = bar_table[num_idx];

	if (sample > 0) {
DBG2		serprintf("%8i %8i\n", ovr, barrier+ovr);
		return barrier + ovr;
	}
	
DBG2	serprintf("%8i %8i\n", ovr, -barrier+ovr);
	return -barrier+ovr;
}


static void _init( void )
{
	/* Init the power history array for the first time.
	   This should be done each time the audio is stopped, but we
	   never know when it occurs... */
	int i;
	for ( i = 0 ; i < _POWER_HISTORY ; i++ ) {
		_power_array[i] = 0;
	}
	_power_idx = 0;

	/* Init other parameters */
	_agc_current_gain = _GAIN_MIN;
	_hold_current = 0;
}

/*****************************************************************************
 *
 * _calculateGain
 *
 * input:
 *	xxx: 		xxxxxxxxxxxxxxxx
 *
 * output:
 *	xxx:		xxxxxxxxxxxxxxxx
 *
 * behaviour:
 *
 *
 ****************************************************************************/

static void _calculateGain( SHORT* buf )
{
	/* Calculate power for this frame and store it in the history
	   (circular buffer) */
	long long me = pcm_s16le_mean_energy( buf, _agc_frameorder-1 );
	_power_array[_power_idx] = me;
	_power_idx = (_power_idx + 1) & (_POWER_HISTORY - 1);

	/* Calculate the power per frame */
	int i;
	long long power_mean = 0;
	for ( i = 0 ; i < _POWER_HISTORY ; i++ ) {
		power_mean += _power_array[i];
	}
	power_mean >>= _POWER_HISTORY_ORDER;

	/* Set the gain */
	if ( me < _agc_power_threshold ) {
		/* -- HOLD -- */
		if ( _hold_current > 0 ) {
			/* Don't change the gain, decrease the hold counter */
			_hold_current--;
		}
		/* -- RELEASE -- */
		else {
			_agc_current_gain += _agc_gain_incr;
		}
	}
	/* -- ATTACK -- */
	else {
		/* Decrease the gain and reset the hold counter */
		_agc_current_gain -= _agc_gain_decr;
		_hold_current = _agc_hold_max;
	}
		
	/* Keep gain between specified boundaries */
	if (_agc_current_gain > _GAIN_MAX) {
		_agc_current_gain = _GAIN_MAX;
	}
	else if (_agc_current_gain < _GAIN_MIN) {
		_agc_current_gain = _GAIN_MIN;
	}

DBG	{
	serprintf("me = %12lli  ", me);
	serprintf("power_mean = %12lli  ", power_mean);
	serprintf("_agc_current_gain >> 16 = %i  ", _agc_current_gain >> 16);
	serprintf("_hold_current  = %i\n", _hold_current);
	}
}

/*****************************************************************************
 *
 * pcm_s16le_pga_softlimiter
 *
 * input:
 *	buf: 		pointer to sample buffer
 *	samples:	number of S16LE samples in buffer
 *	gain:		numerator of gain factor
 *
 * output:
 *	buf:		result is placed in same buffer
 *
 * behaviour:
 *
 * multiplies a buffer of S16LE samples by gain/256 and saturates the result.
 * soft clipping will be used.
 * gain has a valid range of 0 to 65536, reasonable range is 64 (-6dB) to
 * 25600 (+10dB).
 *
 ****************************************************************************/
static void pcm_s16le_pga_softlimiter( SHORT* buf, int samples, int gain )
{
	int i;

	for (i=0; i < samples; i++, buf++) {
		int tmp;

		tmp = pcm_s16le_muldiv(*buf, gain, 8);
		if (abs(tmp) > clip_barrier) {
			// apply soft clipping
			tmp = s32le_softclip_s16le(tmp, clip_barrier);
		}
		*buf = pcm_limit_s16le(tmp);
	}
}

/*****************************************************************************
 *
 * pcm_apply_agc
 *
 * input:
 *     buf:             samples
 *     samples_order	number of samples (2^samples_order)
 *
 ****************************************************************************/
void pcm_apply_agc( SHORT* buf, int samples_order)
{
	int i, iMax;
	
	if ( samples_order < _agc_frameorder ) {
ERR		serprintf("%s - ERROR: samples_order < _agc_frameorder\n", __FUNCTION__ );
		return;
	}

	iMax = 1UL << (samples_order-_agc_frameorder);

	for (i=0; i < iMax; i++, buf += _agc_framesize) {
		_calculateGain( buf );
		/* gain is coded Q16.16, keep the integer part */
		pcm_s16le_pga_softlimiter(buf, _agc_framesize, _agc_current_gain >> 16);
	}
}

/*****************************************************************************
 *
 * pcm_set_agc
 *
 * input:
 *     samplerate:     current sample rate
 *
 * behaviour:
 *
 * The AGC parameters, ie the "attack" and "release" gains depend on the sample
 * rate. These values have been found by ear for 44.1kHz and derived for the
 * others (use the debug commands below).
 * The gains are coded in Q16.16 format to allow fine steps.
 *
 ****************************************************************************/
void pcm_set_agc( int samplerate )
{
	_init();

	/* -- Common settings -- */
	_agc_power_threshold = _POWER_THRESHOLD;
	
	/* -- Frequency specific settings -- */

	/* number of samples in a frame = 2^_agc_frameorder */

	switch ( samplerate ) {
	case 8000:      //TODO: check
		_agc_frameorder = 3;
	break;
	case 11025:     //TODO: check
		_agc_frameorder = 3;
	break;
	case 12000:     //TODO: check
		_agc_frameorder = 3;
	break;
	case 16000:     //TODO: check
		_agc_frameorder = 3;
	break;
	case 22050:     //TODO: check
		_agc_frameorder = 4;
	break;
	case 24000:     //TODO: check
		_agc_frameorder = 4;
	break;
	case 32000:     //TODO: check
		_agc_frameorder = 5;
	break;
	case 44100:
		_agc_frameorder = 5;		// 32 samples = 16 stereo samples = 360us @ 44.1kHz
	break;
	case 48000:
		_agc_frameorder = 5;		// 32 samples = 16 stereo samples = 333us @ 48kHz
	break;
	default:
		_agc_frameorder = 5;
	break;
	}

	_agc_framesize = 1UL << _agc_frameorder;
	_agc_hold_max = (_HOLD_TIME * (long long)samplerate * 2) / (1000 * (long long)_agc_framesize);
	/* (multiply by 2 since _agc_framesize is in samples) */

	/* Calculate gain steps (Q16.16) */
	long long decr = 1000000LL * _agc_framesize * (_GAIN_MAX - _GAIN_MIN);
	decr /= ((long long)_ATTACK_TIME * samplerate);
	_agc_gain_decr = decr;

	long long incr = 1000000LL * _agc_framesize * (_GAIN_MAX - _GAIN_MIN);
	incr /= ((long long)_RELEASE_TIME * samplerate);
	_agc_gain_incr = incr;
	
	{
	serprintf("_agc_power_threshold = %12lli\r\n", _agc_power_threshold);
	serprintf("decr = %12lli\r\n", decr);
	serprintf("_agc_gain_incr  = %i\n", _agc_gain_incr);
	serprintf("_agc_gain_decr  = %i\n", _agc_gain_decr);
	serprintf("_agc_frameorder = %i\n", _agc_frameorder);
	serprintf("_agc_framesize  = %i\n", _agc_framesize);
	serprintf("_agc_hold_max   = %i\n", _agc_hold_max);
	}
}


/*****************************************************************************
 * DEBUG
 ****************************************************************************/
#ifndef STANDALONE
#ifdef DEBUG_MSG
static void _dbg_set_frame( int argc, char* argv[] )
{
	if ( argc >= 2 ) {
		_agc_frameorder = (int)atoi( argv[1] );
	}
	serprintf("_agc_frameorder = %i\n", _agc_frameorder);
}

static void _dbg_set_incr( int argc, char* argv[] )
{
	if ( argc >= 2 ) {
		_agc_gain_incr = (int)atoi( argv[1] );
	}
	serprintf("_agc_gain_incr = %i\n", _agc_gain_incr);
}

static void _dbg_set_decr( int argc, char* argv[] )
{
	if ( argc >= 2 ) {
		_agc_gain_decr = (int)atoi( argv[1] );
	}
	serprintf("_agc_gain_decr = %i\n", _agc_gain_decr);
}

static void _dbg_targetPowerUp( void )
{
	_agc_power_threshold *= 2;
	serprintf("%12lli\r\n", _agc_power_threshold);
}

static void _dbg_targetPowerDn( void )
{
	_agc_power_threshold >>= 1;
	if ( _agc_power_threshold < 1 ) {
		_agc_power_threshold = 1;
	}
	serprintf("%12lli\r\n", _agc_power_threshold);
}

DECLARE_DEBUG_COMMAND     ("atef",    _dbg_set_frame);
DECLARE_DEBUG_COMMAND     ("atei",    _dbg_set_incr);
DECLARE_DEBUG_COMMAND     ("ated",    _dbg_set_decr);
DECLARE_DEBUG_COMMAND_VOID("atpup",   _dbg_targetPowerUp );
DECLARE_DEBUG_COMMAND_VOID("atpdn",   _dbg_targetPowerDn );
#endif
#endif

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

#ifndef _PCM_TOOLS_x86_H
#define _PCM_TOOLS_x86_H

/*****************************************************************************
 *
 * pcm_s16le_muldiv
 *
 * input:
 *	sample:		S16LE sample
 *	mul:		numerator of gain factor
 *	shift:		denominator of gain factor as 2^shift
 *
 * output:
 *	return value
 *
 * behaviour:
 *
 * multiplies one S16LE sample by mul/(2^shift). No saturation is applied
 *
 ****************************************************************************/
static inline int pcm_s16le_muldiv(SHORT sample, const int mul, const int shift)
{
	return (sample * mul) >> shift;
}

/*****************************************************************************
 *
 * pcm_limit_s16le
 *
 * input:
 *	sample:		S32LE sample
 *
 * output:
 *	S16LE return value
 *
 * behaviour:
 *
 * clips a S32LE value to S16LE.
 *
 ****************************************************************************/
static inline SHORT pcm_limit_s16le(int sample)
{
	if (sample > 32767)
		sample = 32767;
	else
	if (sample < -32768)
		sample = -32768;
		
	return sample;
}


/*****************************************************************************
 *
 * pcm_s16le_squaresum
 *
 * input:
 *	buf: 		pointer to sample buffer
 *	samples:	number of S16LE samples in buffer
 *
 * output:
 *	return value
 *
 * behaviour:
 *
 * computes the square sum of all samples in buf and returns the result
 * as a signed 64 bit value.
 *
 ****************************************************************************/
static inline long long pcm_s16le_squaresum( const SHORT* buf, int samples )
{
	int i;
	long long r = 0;

	for (i=0; i < samples; i++, buf++) {
		r += (*buf) * (*buf);
	}

	return r;
}

/*****************************************************************************
 *
 * pcm_s16le_mean_energy
 *
 * input:
 *	buf: 		pointer to sample buffer
 *	samples_order:	number of S16LE samples in buffer as 2^samples_order
 *
 * output:
 *	return value
 *
 * behaviour:
 *
 * computes the mean energy of all samples inside buf. this is done by first
 * computing the square sum and then dividing the result by the number of
 * samples. for performance reasons, the number of samples must be a power
 * of two, which is enforced by supplying the order instead of the number
 * of samples.
 *
 ****************************************************************************/
static inline long long pcm_s16le_mean_energy( const SHORT* buf, int samples_order)
{
	long long sqsum = pcm_s16le_squaresum(buf, (1UL<<samples_order));
	return sqsum >> samples_order;
}

#endif

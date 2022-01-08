#ifndef SIM
/*
    Copyright (C) 2008 Mans Rullgard

    Permission is hereby granted, free of charge, to any person
    obtaining a copy of this software and associated documentation
    files (the "Software"), to deal in the Software without
    restriction, including without limitation the rights to use, copy,
    modify, merge, publish, distribute, sublicense, and/or sell copies
    of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be
    included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
    HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
 */

        .fpu neon
        .text


@----------------------------------------------------------------------------
@ YUV422toRGB888 converts YUV 4:2:2 pixels to 24-bit RGB.  The input Y
@  values should be separated into even and odd.  The output will have the
@  even and odd pixels grouped, as well as the color channels separated.
@  YUV422toRGB888_PRE should be called earlier in the function that uses
@  this macro to allow setting up the constants needed.  Note that the
@  constants use q10-q15 (d20-d31), so those registers should be preserved
@  for the call to this macro.
@
@ Arguments:
@  dstRe (Dn) - output: 8 even red components
@  dstRo (Dn) - output: 8 odd red components
@  dstGe (Dn) - output: 8 even green components
@  dstGo (Dn) - output: 8 odd green components
@  dstBe (Dn) - output: 8 even blue components
@  dstBo (Dn) - output: 8 odd blue components
@  srcYe (Dn) - input: 8 even Y components
@  srcYo (Dn) - input: 8 odd Y components
@  srcUV (Dn) - input: 8 UV components
@  tmpYe (Qn) - working: temp reg for even Y values
@  tmpYo (Qn) - working: temp reg for odd Y values
@  tmpU (Qn) - working: temp reg for U values
@  tmpV (Qn) - working: temp reg for V values
@  tmpR (Qn) - working: temp reg for red
@  tmpG (Qn) - working: temp reg for green
@  tmpB (Qn) - working: temp reg for blue

	@ source and destination registers can duplicate
	.macro YUVtoRGB888	dstRe, dstRo, dstGe, dstGo, dstBe, dstBo, srcYe, srcYo, srcU, srcV, tmpYe, tmpYo, tmpU, tmpV, tmpR, tmpG, tmpB

	vmovl.u8	\tmpU, \srcU		@ extend to 16 bits
	vmovl.u8	\tmpV, \srcV		@ duplicate uv vector
	vmovl.u8	\tmpYe, \srcYe		@ extend to 16 bits
	vmovl.u8	\tmpYo, \srcYo		@ extend to 16 bits

	vmov.i16	\tmpR, #128
	vsub.s16	\tmpU, \tmpU, \tmpR	@ u - 128
	vsub.s16	\tmpV, \tmpV, \tmpR	@ v - 128

	vmov.i16	\tmpG, #16
	vsub.s16	\tmpYe, \tmpYe, \tmpG	@ even y - 16
	vsub.s16	\tmpYo, \tmpYo, \tmpG	@ odd  y - 16

	vmul.s16	\tmpYe, \tmpYe, q10	@ (even y -16) * 1.164
	vmul.s16	\tmpYo, \tmpYo, q10	@ (odd  y -16) * 1.164

	vmov.s16	\tmpR, \tmpYe		@ Re = (even y -16) * 1.164
	vmla.s16	\tmpR, \tmpV, q11	@ Re = (even y -16) * 1.164 + (v-128)*1.596

	vmov.s16	\tmpB, \tmpYe		@ Be = (even y -16) * 1.164
	vmla.s16	\tmpB, \tmpU, q12	@ Be = (even y -16) * 1.164 + (u-128)*2.018
	
	vmov.s16	\tmpG, \tmpYe		@ Ge = (even y -16) * 1.164
	vmls.s16	\tmpG, \tmpU, q13	@ Ge = (even y -16) * 1.164 - (u-128)*0.391
	vmls.s16	\tmpG, \tmpV, q14	@ Ge = (even y -16) * 1.164 - (u-128)*0.391 - (v-128)*0.813

	vqshrun.s16	\dstRe, \tmpR, #6	@ Re
	vqshrun.s16	\dstGe, \tmpG, #6	@ Ge
	vqshrun.s16	\dstBe, \tmpB, #6	@ Be

	vmov.s16	\tmpR, \tmpYo		@ Re = (odd  y -16) * 1.164
	vmla.s16	\tmpR, \tmpV, q11	@ Re = (odd  y -16) * 1.164 + (v-128)*1.596

	vmov.s16	\tmpB, \tmpYo		@ Be = (odd  y -16) * 1.164
	vmla.s16	\tmpB, \tmpU, q12	@ Be = (odd  y -16) * 1.164 + (u-128)*2.018
	
	vmov.s16	\tmpG, \tmpYo		@ Go = (odd  y -16) * 1.164
	vmls.s16	\tmpG, \tmpU, q13	@ Go = (odd  y -16) * 1.164 - (u-128)*0.391
	vmls.s16	\tmpG, \tmpV, q14	@ Go = (odd  y -16) * 1.164 - (u-128)*0.391 - (v-128)*0.813

	vqshrun.s16	\dstRo, \tmpR, #6	@ Ro
	vqshrun.s16	\dstGo, \tmpG, #6	@ Go
	vqshrun.s16	\dstBo, \tmpB, #6	@ Bo
	.endm

	@ source and destination registers can duplicate
	.macro YUV10btoRGB888	dstRe, dstRo, dstGe, dstGo, dstBe, dstBo, tmpYe, tmpYo, tmpU, tmpV, tmpR, tmpG, tmpB

	vmov.i16	\tmpG, #64
	vsub.s16	\tmpYe, \tmpYe, \tmpG	@ even y - 64
	vsub.s16	\tmpYo, \tmpYo, \tmpG	@ odd  y - 64

	vmul.s16	\tmpYe, \tmpYe, q10	@ (even y -64) * 1.0
	vmul.s16	\tmpYo, \tmpYo, q10	@ (odd  y -64) * 1.0

	vmov.s16	\tmpR, \tmpYe		@ Re = (even y -64) * 1.0
	vmla.s16	\tmpR, \tmpV, q11	@ Re = (even y -64) * 1.0 + (v-512)*1.13983

	vmov.s16	\tmpB, \tmpYe		@ Be = (even y -64) * 1.0
	vmla.s16	\tmpB, \tmpU, q12	@ Be = (even y -64) * 1.0 + (u-512)*2.03211
	
	vmov.s16	\tmpG, \tmpYe		@ Ge = (even y -64) * 1.0
	vmls.s16	\tmpG, \tmpU, q13	@ Ge = (even y -64) * 1.0 - (u-512)*0.39455
	vmls.s16	\tmpG, \tmpV, q14	@ Ge = (even y -64) * 1.0 - (u-512)*0.39455 - (v-512)*0.58060

	vqshrun.s16	\dstRe, \tmpR, #7	@ Re
	vqshrun.s16	\dstGe, \tmpG, #7	@ Ge
	vqshrun.s16	\dstBe, \tmpB, #7	@ Be

	vmov.s16	\tmpR, \tmpYo		@ Re = (odd  y -64) * 1.0
	vmla.s16	\tmpR, \tmpV, q11	@ Re = (odd  y -64) * 1.0 + (v-512)*1.13983

	vmov.s16	\tmpB, \tmpYo		@ Be = (odd  y -64) * 1.0
	vmla.s16	\tmpB, \tmpU, q12	@ Be = (odd  y -64) * 1.0 + (u-512)*2.03211
	
	vmov.s16	\tmpG, \tmpYo		@ Go = (odd  y -64) * 1.0
	vmls.s16	\tmpG, \tmpU, q13	@ Go = (odd  y -64) * 1.0 - (u-512)*0.39455
	vmls.s16	\tmpG, \tmpV, q14	@ Go = (odd  y -64) * 1.0 - (u-512)*0.39455 - (v-512)*0.58060

	vqshrun.s16	\dstRo, \tmpR, #7	@ Ro
	vqshrun.s16	\dstGo, \tmpG, #7	@ Go
	vqshrun.s16	\dstBo, \tmpB, #7	@ Bo
	.endm

@--------------------------------------------------------------------------
@ YUV422toRGB888_PREP sets up the constants needed for the RGB888toYUV422
@  macro below.  q10-q15 are initialized.
@
@ Arguments:
@  none
	.macro YUVtoRGB888_PREP

	vmov.i16	q10, #74	@ 1.164 * 64
	vmov.i16	q11, #102	@ 1.596 * 64
	vmov.i16	q12, #129	@ 2.018 * 64
	vmov.i16	q13, #25	@ 0.391 * 64
	vmov.i16	q14, #52	@ 0.813 * 64

	.endm

	.macro YUV10btoRGB888_PREP

	vmov.i16	q10, #32	@ 1.0 * 32
	vmov.i16	q11, #36	@ 1.13983 * 32
	vmov.i16	q12, #65	@ 2.03211 * 32
	vmov.i16	q13, #12	@ 0.39455 * 32
	vmov.i16	q14, #18	@ 0.58060 * 32

	.endm

	.macro STORE_16xBGRA remWidth, dstRgb

	@ d0 contains R values like: R0, R2, R3, R4,...
	@ d4 contains R value like : R1, R3, R5, R6,...
	@ after deinterlevaing, we will have:
	@ d0 contains first R value like: R0, R1, R2, R3,...
	@ d4 contains next R values like: R8, R9, R10, R11,...
	@now proceed deinterleaving
	vtrn.8		q0, q2
	vtrn.16		q0, q2
	vtrn.32		q0, q2
	vtrn.8		q1, q3
	vtrn.16		q1, q3
	vtrn.32		q1, q3
	cmp		\remWidth, #16
	blt		14f
	vst4.8		{d0,d1,d2,d3}, [\dstRgb]!	@ store the 8 first RGBA pixels
	vst4.8		{d4,d5,d6,d7}, [\dstRgb]!	@ store the 8 last RGBA pixels
	b		0f
14:
	cmp 		\remWidth, #14
	blt		12f
	vst4.8	{d0[0],d1[0],d2[0],d3[0]}, [\dstRgb]!
	vst4.8	{d0[1],d1[1],d2[1],d3[1]}, [\dstRgb]!
	vst4.8	{d0[2],d1[2],d2[2],d3[2]}, [\dstRgb]!
	vst4.8	{d0[3],d1[3],d2[3],d3[3]}, [\dstRgb]!
	vst4.8	{d0[4],d1[4],d2[4],d3[4]}, [\dstRgb]!
	vst4.8	{d0[5],d1[5],d2[5],d3[5]}, [\dstRgb]!
	vst4.8	{d0[6],d1[6],d2[6],d3[6]}, [\dstRgb]!
	vst4.8	{d0[7],d1[7],d2[7],d3[7]}, [\dstRgb]!
	vst4.8	{d4[0],d5[0],d6[0],d7[0]}, [\dstRgb]!
	vst4.8	{d4[1],d5[1],d6[1],d7[1]}, [\dstRgb]!
	vst4.8	{d4[2],d5[2],d6[2],d7[2]}, [\dstRgb]!
	vst4.8	{d4[3],d5[3],d6[3],d7[3]}, [\dstRgb]!
	vst4.8	{d4[4],d5[4],d6[4],d7[4]}, [\dstRgb]!
	vst4.8	{d4[5],d5[5],d6[5],d7[5]}, [\dstRgb]!
	b 	0f
12:
	cmp 		\remWidth, #12
	blt		10f
	vst4.8	{d0[0],d1[0],d2[0],d3[0]}, [\dstRgb]!
	vst4.8	{d0[1],d1[1],d2[1],d3[1]}, [\dstRgb]!
	vst4.8	{d0[2],d1[2],d2[2],d3[2]}, [\dstRgb]!
	vst4.8	{d0[3],d1[3],d2[3],d3[3]}, [\dstRgb]!
	vst4.8	{d0[4],d1[4],d2[4],d3[4]}, [\dstRgb]!
	vst4.8	{d0[5],d1[5],d2[5],d3[5]}, [\dstRgb]!
	vst4.8	{d0[6],d1[6],d2[6],d3[6]}, [\dstRgb]!
	vst4.8	{d0[7],d1[7],d2[7],d3[7]}, [\dstRgb]!
	vst4.8	{d4[0],d5[0],d6[0],d7[0]}, [\dstRgb]!
	vst4.8	{d4[1],d5[1],d6[1],d7[1]}, [\dstRgb]!
	vst4.8	{d4[2],d5[2],d6[2],d7[2]}, [\dstRgb]!
	vst4.8	{d4[3],d5[3],d6[3],d7[3]}, [\dstRgb]!
	b 	0f
10:
	cmp 		\remWidth, #10
	blt		8f
	vst4.8	{d0[0],d1[0],d2[0],d3[0]}, [\dstRgb]!
	vst4.8	{d0[1],d1[1],d2[1],d3[1]}, [\dstRgb]!
	vst4.8	{d0[2],d1[2],d2[2],d3[2]}, [\dstRgb]!
	vst4.8	{d0[3],d1[3],d2[3],d3[3]}, [\dstRgb]!
	vst4.8	{d0[4],d1[4],d2[4],d3[4]}, [\dstRgb]!
	vst4.8	{d0[5],d1[5],d2[5],d3[5]}, [\dstRgb]!
	vst4.8	{d0[6],d1[6],d2[6],d3[6]}, [\dstRgb]!
	vst4.8	{d0[7],d1[7],d2[7],d3[7]}, [\dstRgb]!
	vst4.8	{d4[0],d5[0],d6[0],d7[0]}, [\dstRgb]!
	vst4.8	{d4[1],d5[1],d6[1],d7[1]}, [\dstRgb]!
	b 	0f
8:
	cmp 		\remWidth, #8
	blt		6f
	vst4.8	{d0[0],d1[0],d2[0],d3[0]}, [\dstRgb]!
	vst4.8	{d0[1],d1[1],d2[1],d3[1]}, [\dstRgb]!
	vst4.8	{d0[2],d1[2],d2[2],d3[2]}, [\dstRgb]!
	vst4.8	{d0[3],d1[3],d2[3],d3[3]}, [\dstRgb]!
	vst4.8	{d0[4],d1[4],d2[4],d3[4]}, [\dstRgb]!
	vst4.8	{d0[5],d1[5],d2[5],d3[5]}, [\dstRgb]!
	vst4.8	{d0[6],d1[6],d2[6],d3[6]}, [\dstRgb]!
	vst4.8	{d0[7],d1[7],d2[7],d3[7]}, [\dstRgb]!
	b 	0f
6:
	cmp 		\remWidth, #6
	blt		4f
	vst4.8	{d0[0],d1[0],d2[0],d3[0]}, [\dstRgb]!
	vst4.8	{d0[1],d1[1],d2[1],d3[1]}, [\dstRgb]!
	vst4.8	{d0[2],d1[2],d2[2],d3[2]}, [\dstRgb]!
	vst4.8	{d0[3],d1[3],d2[3],d3[3]}, [\dstRgb]!
	vst4.8	{d0[4],d1[4],d2[4],d3[4]}, [\dstRgb]!
	vst4.8	{d0[5],d1[5],d2[5],d3[5]}, [\dstRgb]!
	b 	0f
4:
	cmp 		\remWidth, #4
	blt		2f
	vst4.8	{d0[0],d1[0],d2[0],d3[0]}, [\dstRgb]!
	vst4.8	{d0[1],d1[1],d2[1],d3[1]}, [\dstRgb]!
	vst4.8	{d0[2],d1[2],d2[2],d3[2]}, [\dstRgb]!
	vst4.8	{d0[3],d1[3],d2[3],d3[3]}, [\dstRgb]!
	b 	0f
2:
	cmp 		\remWidth, #2
	blt		0f
	vst4.8	{d0[0],d1[0],d2[0],d3[0]}, [\dstRgb]!
	vst4.8	{d0[1],d1[1],d2[1],d3[1]}, [\dstRgb]!
0:
	.endm

@ void neon_memcpy_shift(uint8_t *dst, uint16_t *src,const int length)
@ right shift the src and copy the result to dst

	.global neon_memcpy_shift

neon_memcpy_shift:
        push        {r0, r5, lr}

1:
        cmp        r2, #16
        blt        2f        

        vld2.16     {q0, q1}, [r1]!
        vqshrun.s16 d4, q0, #2
        vqshrun.s16 d5, q1, #2
        vst2.8      {d4, d5}, [r0]!

        sub        r2, r2, #16
        b          1b
2:
        cmp        r2, #8
        blt        3f
        
        vld1.16     {q0}, [r1]!
        vqshrun.s16 d4, q0, #2
        vst1.8      {d4}, [r0]!

        sub        r2, r2, #8
        b          2b
3:
        cmp        r2, #4
        blt        4f
        
        vld1.16     {d0}, [r1]!
        vqshrun.s16 d4, q0, #2
        vst1.8      {d4[0]}, [r0]!
        vst1.8      {d4[1]}, [r0]!
        vst1.8      {d4[2]}, [r0]!
        vst1.8      {d4[3]}, [r0]!

        sub        r2, r2, #4
        b          3b
4:
        cmp        r2, #2
        blt        5f
        
        vld2.16     {d0[0], d1[0]}, [r1]!
        vqshrun.s16 d4, q0, #2
        vst1.8      {d4[0]}, [r0]!
        vst1.8      {d4[4]}, [r0]!

        sub        r2, r2, #2
        b          4b
5:
	cmp        r2, #1
        blt        6f
        
        vld1.16     {d0[0]}, [r1]!
        vqshrun.s16 d4, q0, #2
        vst1.8      {d4[0]}, [r0]!

        sub        r2, r2, #1
        b          5b
6:
        pop         {r0, r5, pc}

@ void neon_scale_420P10b_YUV(uint8_t *dst, uint16_t *src,const int length)
@ scale 10bY value to 8bY right shift the src and copy the result to dst

	.global neon_scale_420P10b_YUV

neon_scale_420P10b_YUV:
        push        {r0, r5, lr}
        vmov.i32    q7, #220
1:
        cmp        r2, #16
        blt        2f        

        vld2.16     {q0, q1}, [r1]!

        vmov.i32    q2, #16384		@ load 16 * 1024
        vmovl.u16   q6, d0		@ extend to 32 bits
        vmla.s32    q2, q6, q7		@ R = Y * 220 + 16* 1024

        vmov.i32    q3, #16384
        vmovl.u16   q6, d1
        vmla.s32    q3, q6, q7

        vmov.i32    q4, #16384
        vmovl.u16   q6, d2
        vmla.s32    q4, q6, q7

        vmov.i32    q5, #16384
        vmovl.u16   q6, d3
        vmla.s32    q5, q6, q7

        vqshrun.s32 d0, q2, #10
        vqshrun.s32 d1, q3, #10
        vqshrun.s32 d2, q4, #10
        vqshrun.s32 d3, q5, #10
  
        vqshrun.s16 d4, q0, #0
        vqshrun.s16 d5, q1, #0
        vst2.8      {d4, d5}, [r0]!

        sub        r2, r2, #16
        b          1b
2:
        cmp        r2, #8
        blt        3f
        
        vld1.16     {q0}, [r1]!

        vmov.i32    q2, #16384		@ load 16 * 1024
        vmovl.u16   q6, d0		@ extend to 32 bits
        vmla.s32    q2, q6, q7		@ R = Y * 220 + 16* 1024

        vmov.i32    q3, #16384
        vmovl.u16   q6, d1
        vmla.s32    q3, q6, q7

        vqshrun.s32 d0, q2, #10
        vqshrun.s32 d1, q3, #10
        vqshrun.s16 d4, q0, #0

        vst1.8      {d4}, [r0]!

        sub        r2, r2, #8
        b          2b
3:
        cmp        r2, #4
        blt        4f
        
        vld1.16     {d0}, [r1]!

        vmov.i32    q2, #16384		@ load 16 * 1024
        vmovl.u16   q6, d0		@ extend to 32 bits
        vmla.s32    q2, q6, q7		@ R = Y * 220 + 16* 1024

        vmov.i32    q3, #16384
        vmovl.u16   q6, d1
        vmla.s32    q3, q6, q7

        vqshrun.s32 d0, q2, #10
        vqshrun.s32 d1, q3, #10
        vqshrun.s16 d4, q0, #0

        vst1.8      {d4[0]}, [r0]!
        vst1.8      {d4[1]}, [r0]!
        vst1.8      {d4[2]}, [r0]!
        vst1.8      {d4[3]}, [r0]!

        sub        r2, r2, #4
        b          3b
4:
        cmp        r2, #2
        blt        5f
        
        vld2.16     {d0[0], d1[0]}, [r1]!

        vmov.i32    q2, #16384		@ load 16 * 1024
        vmovl.u16   q6, d0		@ extend to 32 bits
        vmla.s32    q2, q6, q7		@ R = Y * 220 + 16* 1024

        vmov.i32    q3, #16384
        vmovl.u16   q6, d1
        vmla.s32    q3, q6, q7

        vqshrun.s32 d0, q2, #10
        vqshrun.s32 d1, q3, #10
        vqshrun.s16 d4, q0, #0

        vst1.8      {d4[0]}, [r0]!
        vst1.8      {d4[4]}, [r0]!

        sub        r2, r2, #2
        b          4b
5:
	cmp        r2, #1
        blt        6f
        
        vld1.16     {d0[0]}, [r1]!

        vmov.i32    q2, #16384		@ load 16 * 1024
        vmovl.u16   q6, d0		@ extend to 32 bits
        vmla.s32    q2, q6, q7		@ R = Y * 220 + 16* 1024

        vqshrun.s32 d0, q2, #10
        vqshrun.s16 d4, q0, #0

        vst1.8      {d4[0]}, [r0]!

        sub        r2, r2, #1
        b          5b
6:
        pop         {r0, r5, pc}

@ void neon_copy_pack_shift(uint16_t *dst, const uint16_t *srcV, const uint16_t *srcU, const int length)
@ right shift the src and copy the result to dst

	.global neon_copy_pack_shift

neon_copy_pack_shift:
        push        {r0, lr}

1:
        cmp        r3, #32
        blt        2f        

        vld1.16     {q0}, [r1]! @V
        vld1.16     {q1}, [r1]! @V
        vld1.16     {q2}, [r2]! @U
        vld1.16     {q3}, [r2]! @U
        vqshrun.s16 d10, q0, #2	@V >> 2
        vqshrun.s16 d11, q1, #2  @V >> 2
        vqshrun.s16 d8, q2, #2 @U >> 2
        vqshrun.s16 d9, q3, #2 @U >> 2

        vst2.8      {d8, d10}, [r0]!
	vst2.8      {d9, d11}, [r0]!

        sub        r3, r3, #32
        b          1b
2:
        cmp        r3, #16
        blt        3f    
        
        vld1.16     {q0}, [r1]! @V
        vld1.16     {q2}, [r2]! @U
        vqshrun.s16 d10, q0, #2	@V >> 2
        vqshrun.s16 d8, q2, #2 @U >> 2

        vst2.8      {d8, d10}, [r0]!

        sub        r3, r3, #16
        b          2b
3:
        cmp        r3, #8
        blt        4f    
        
        vld1.16     {d0}, [r1]! @V
        vld1.16     {d4}, [r2]! @U
        vqshrun.s16 d10, q0, #2	@V >> 2
        vqshrun.s16 d9, q2, #2 @U >> 2

        vst2.8      {d9[0], d10[0]}, [r0]!
        vst2.8      {d9[1], d10[1]}, [r0]!
        vst2.8      {d9[2], d10[2]}, [r0]!
        vst2.8      {d9[3], d10[3]}, [r0]!

        sub        r3, r3, #8
        b          3b
4:
        cmp        r3, #4
        blt        5f    
        
        vld2.16     {d0[0], d1[0]}, [r1]! @V
        vld2.16     {d4[0], d5[0]}, [r2]! @U
        vqshrun.s16 d10, q0, #2	@V >> 2
        vqshrun.s16 d9, q2, #2 @U >> 2

        vst2.8      {d9[0], d10[0]}, [r0]!
        vst2.8      {d9[4], d10[4]}, [r0]!

        sub        r3, r3, #4
        b          4b
5:
        cmp        r3, #2
        blt        6f    
        
        vld1.16     {d0[0]}, [r1]! @V
        vld1.16     {d4[0]}, [r2]! @U
        vqshrun.s16 d10, q0, #2	@V >> 2
        vqshrun.s16 d9, q2, #2 @U >> 2

        vst2.8      {d9[0], d10[0]}, [r0]!

        sub        r3, r3, #2
        b          5b
6:
        cmp        r3, #1
        blt        7f
        
        vld1.16     {d0[0]}, [r1]!
        vld1.16     {d4[0]}, [r2]! @U
        vqshrun.s16 d10, q0, #2
        vqshrun.s16 d9, q2, #2
        vst2.8      {d9[0], d10[0]}, [r0]!

        sub        r3, r3, #1
        b          6b

7:
        pop         {r0, pc}

@ neon_yuv420_to_BGRA32(uint8_t *yuv, uint8_t *y, uint8_t *u, uint8_t *v,
@                  int w, int h, int yw, int cw, int dw)

#define bgra  r0
#define y1    r1
#define y2    r2
#define iu    r3
#define iv    r4
#define rw   r5
#define l    r6
#define pxSz r7
#define bgra2 r8

#define ye   d0
#define yo   d1
#define du  d30
#define dv  d31

        .global neon_yuv420_to_BGRA32
neon_yuv420_to_BGRA32:
        push            {r4-r11,lr}
        add             r4,  sp,  #36
        ldm             r4, {r4-r8}
        dmb

	mov 		pxSz, #4
	mla		bgra2, l, pxSz, bgra

	YUVtoRGB888_PREP

	vld1.8          {du}, [iu]              @ u0
	vld1.8          {dv}, [iv]              @ v0

	pld             [y1]
	vld2.8          {ye, yo},   [y1]        @ y0 interleaved

	YUVtoRGB888 d2,d6,d1,d5,d0,d4,ye,yo,du,dv,q3,q4,q5,q6,q7,q8,q9 @convert YUYV value into 16 RGBA pixels

	vmov.i8		d3, #0xFF		@ force alpha to 0xFF for first 8 RGBA pixels
	vmov.i8		d7, #0xFF		@ force alpha to 0xFF for last 8 RGBA pixels

	STORE_16xBGRA   rw, bgra

	pld             [y2]
	vld2.8          {ye, yo},   [y2]        @ y0 interleaved

	YUVtoRGB888 d2,d6,d1,d5,d0,d4,ye,yo,du,dv,q3,q4,q5,q6,q7,q8,q9 @convert YUYV value into 16 RGBA pixels

	vmov.i8		d3, #0xFF		@ force alpha to 0xFF for first 8 RGBA pixels
	vmov.i8		d7, #0xFF		@ force alpha to 0xFF for last 8 RGBA pixels

	STORE_16xBGRA   rw, bgra2

        pop             {r4-r11,pc}

@ neon_yuv420_to_RGBX(uint8_t *yuv, uint8_t *y, uint8_t *u, uint8_t *v,
@                  int w, int h, int yw, int cw, int dw)

#define bgra  r0
#define y1    r1
#define y2    r2
#define iu    r3
#define iv    r4
#define rw   r5
#define l    r6
#define pxSz r7
#define bgra2 r8

#define ye   d0
#define yo   d1
#define du  d30
#define dv  d31

        .global neon_yuv420_to_RGBX32
neon_yuv420_to_RGBX32:
        push            {r4-r11,lr}
        add             r4,  sp,  #36
        ldm             r4, {r4-r8}
        dmb

	mov 		pxSz, #4
	mla		bgra2, l, pxSz, bgra

	YUVtoRGB888_PREP

	vld1.8          {du}, [iu]              @ u0
	vld1.8          {dv}, [iv]              @ v0

	pld             [y1]
	vld2.8          {ye, yo},   [y1]        @ y0 interleaved

	YUVtoRGB888 d0,d4,d1,d5,d2,d6,ye,yo,du,dv,q3,q4,q5,q6,q7,q8,q9 @convert YUYV value into 16 RGBA pixels

	vmov.i8		d3, #0xFF		@ force alpha to 0xFF for first 8 RGBA pixels
	vmov.i8		d7, #0xFF		@ force alpha to 0xFF for last 8 RGBA pixels

	STORE_16xBGRA   rw, bgra

	pld             [y2]
	vld2.8          {ye, yo},   [y2]        @ y0 interleaved

	YUVtoRGB888 d0,d4,d1,d5,d2,d6,ye,yo,du,dv,q3,q4,q5,q6,q7,q8,q9 @convert YUYV value into 16 RGBA pixels

	vmov.i8		d3, #0xFF		@ force alpha to 0xFF for first 8 RGBA pixels
	vmov.i8		d7, #0xFF		@ force alpha to 0xFF for last 8 RGBA pixels

	STORE_16xBGRA   rw, bgra2

        pop             {r4-r11,pc}

@ neon_yuv42010b_to_BGRA32(uint8_t *yuv, uint8_t *y, uint8_t *u, uint8_t *v,
@                  int w, int h, int yw, int cw, int dw)

#define bgra  r0
#define y1    r1
#define y2    r2
#define iu    r3
#define iv    r4
#define rw   r5
#define l    r6
#define pxSz r7
#define bgra2 r8

#define ye   d0
#define yo   d1
#define du  d30
#define dv  d31

        .global neon_yuv42010b_to_BGRA32
neon_yuv42010b_to_BGRA32:
        push            {r4-r11,lr}
        add             r4,  sp,  #36
        ldm             r4, {r4-r8}
        dmb

	mov 		pxSz, #4
	mla		bgra2, l, pxSz, bgra

	YUV10btoRGB888_PREP

	vld1.16         {q5}, [iu]              @ u0
	vld1.16         {q6}, [iv]              @ v0

	vmov.i16	q7, #512
	vsub.s16	q5, q5, q7	@ u - 512
	vsub.s16	q6, q6, q7	@ v - 512

	pld             [y1]
	vld2.16         {q3, q4},   [y1]        @ y0 interleaved

	YUV10btoRGB888 d2,d6,d1,d5,d0,d4,q3,q4,q5,q6,q7,q8,q9 @convert YUYV value into 8 RGBA pixels

	vmov.i16	d3, #0xFF		@ force alpha to 0xFF for first 8 RGBA pixels
	vmov.i16	d7, #0xFF		@ force alpha to 0xFF for last 8 RGBA pixels

	STORE_16xBGRA   rw, bgra

	pld             [y2]
	vld2.16         {q3, q4},   [y2]        @ y0 interleaved

	YUV10btoRGB888 d2,d6,d1,d5,d0,d4,q3,q4,q5,q6,q7,q8,q9 @convert YUYV value into 8 RGBA pixels

	vmov.i16	d3, #0xFF		@ force alpha to 0xFF for first 8 RGBA pixels
	vmov.i16	d7, #0xFF		@ force alpha to 0xFF for last 8 RGBA pixels

	STORE_16xBGRA   rw, bgra2

        pop             {r4-r11,pc}

@ neon_yuv42010b_to_RGBX(uint8_t *yuv, uint8_t *y, uint8_t *u, uint8_t *v,
@                  int w, int h, int yw, int cw, int dw)

#define bgra  r0
#define y1    r1
#define y2    r2
#define iu    r3
#define iv    r4
#define rw   r5
#define l    r6
#define pxSz r7
#define bgra2 r8

#define ye   d0
#define yo   d1
#define du  d30
#define dv  d31

        .global neon_yuv42010b_to_RGBX32
neon_yuv42010b_to_RGBX32:
        push            {r4-r11,lr}
        add             r4,  sp,  #36
        ldm             r4, {r4-r8}
        dmb

	mov 		pxSz, #4
	mla		bgra2, l, pxSz, bgra

	YUV10btoRGB888_PREP

	vld1.16         {q5}, [iu]              @ u0
	vld1.16         {q6}, [iv]              @ v0

	vmov.i16	q7, #512
	vsub.s16	q5, q5, q7	@ u - 512
	vsub.s16	q6, q6, q7	@ v - 512

	pld             [y1]
	vld2.16         {q3, q4},   [y1]        @ y0 interleaved

	YUV10btoRGB888 d0,d4,d1,d5,d2,d6,q3,q4,q5,q6,q7,q8,q9 @convert YUYV value into 8 RGBA pixels

	vmov.i16	d3, #0xFF		@ force alpha to 0xFF for first 8 RGBA pixels
	vmov.i16	d7, #0xFF		@ force alpha to 0xFF for last 8 RGBA pixels

	STORE_16xBGRA   rw, bgra

	pld             [y2]
	vld2.16         {q3, q4},   [y2]        @ y0 interleaved

	YUV10btoRGB888 d0,d4,d1,d5,d2,d6,q3,q4,q5,q6,q7,q8,q9 @convert YUYV value into 8 RGBA pixels

	vmov.i16	d3, #0xFF		@ force alpha to 0xFF for first 8 RGBA pixels
	vmov.i16	d7, #0xFF		@ force alpha to 0xFF for last 8 RGBA pixels

	STORE_16xBGRA   rw, bgra2

        pop             {r4-r11,pc}


@ neon_yuv422_to_BGRA32(uint8_t *rgb, uint8_t *y, uint8_t *u, uint8_t *v,
@                  int w, int h, int yw, int cw, int dw)

#define bgra  r0
#define y_422 r1
#define u_422 r2
#define v_422 r3
#define l_422 r4

#define ye   d0
#define yo   d1
#define du  d30
#define dv  d31

        .global neon_yuv422_to_BGRA32
neon_yuv422_to_BGRA32:
        push            {r4-r11,lr}
        add             r4,  sp,  #36
        ldm             r4, {r4-r8}
        dmb

	YUVtoRGB888_PREP

	vld1.8          {du}, [u_422]              @ u0
	vld1.8          {dv}, [v_422]              @ v0

	pld             [y_422]
	vld2.8          {ye, yo},   [y_422]        @ y0 interleaved

	YUVtoRGB888 d2,d6,d1,d5,d0,d4,ye,yo,du,dv,q3,q4,q5,q6,q7,q8,q9 @convert YUYV value into 16 RGBA pixels

	vmov.i8		d3, #0xFF		@ force alpha to 0xFF for first 8 RGBA pixels
	vmov.i8		d7, #0xFF		@ force alpha to 0xFF for last 8 RGBA pixels

	STORE_16xBGRA   l_422, bgra

        pop             {r4-r11,pc}

@ neon_yuv422_to_RGBX32(uint8_t *rgb, uint8_t *y, uint8_t *u, uint8_t *v,
@                  int w, int h, int yw, int cw, int dw)

#define bgra  r0
#define y_422 r1
#define u_422 r2
#define v_422 r3
#define l_422 r4

#define ye   d0
#define yo   d1
#define du  d30
#define dv  d31

        .global neon_yuv422_to_RGBX32
neon_yuv422_to_RGBX32:
        push            {r4-r11,lr}
        add             r4,  sp,  #36
        ldm             r4, {r4-r8}
        dmb

	YUVtoRGB888_PREP

	vld1.8          {du}, [u_422]              @ u0
	vld1.8          {dv}, [v_422]              @ v0

	pld             [y_422]
	vld2.8          {ye, yo},   [y_422]        @ y0 interleaved

	YUVtoRGB888 d0,d4,d1,d5,d2,d6,ye,yo,du,dv,q3,q4,q5,q6,q7,q8,q9 @convert YUYV value into 16 RGBA pixels

	vmov.i8		d3, #0xFF		@ force alpha to 0xFF for first 8 RGBA pixels
	vmov.i8		d7, #0xFF		@ force alpha to 0xFF for last 8 RGBA pixels

	STORE_16xBGRA   l_422, bgra

        pop             {r4-r11,pc}

@ neon_nv12_to_BGRA32(uint8_t *yuv, uint8_t *y, uint8_t *uv,
@                  int w, int h, int yw, int cw, int dw)

#define bgra      r0
#define y1        r1
#define y2        r2
#define iuv       r3
#define nv12_rw   r4
#define nv12_l    r5
#define pxSz      r7
#define bgra2     r8

#define ye   d0
#define yo   d1
#define du  d30
#define dv  d31

        .global neon_nv12_to_BGRA32
neon_nv12_to_BGRA32:
        push            {r4-r11,lr}
        add             r4,  sp,  #36
        ldm             r4, {r4-r8}
        dmb

	mov 		pxSz, #4
	mla		bgra2, nv12_l, pxSz, bgra

	YUVtoRGB888_PREP

	vld2.8          {du, dv}, [iuv]              @ u0

	pld             [y1]
	vld2.8          {ye, yo},   [y1]        @ y0 interleaved

	YUVtoRGB888 d2,d6,d1,d5,d0,d4,ye,yo,du,dv,q3,q4,q5,q6,q7,q8,q9 @convert YUYV value into 16 RGBA pixels

	vmov.i8		d3, #0xFF		@ force alpha to 0xFF for first 8 RGBA pixels
	vmov.i8		d7, #0xFF		@ force alpha to 0xFF for last 8 RGBA pixels

	STORE_16xBGRA   nv12_rw, bgra

	pld             [y2]
	vld2.8          {ye, yo},   [y2]        @ y0 interleaved

	YUVtoRGB888 d2,d6,d1,d5,d0,d4,ye,yo,du,dv,q3,q4,q5,q6,q7,q8,q9 @convert YUYV value into 16 RGBA pixels

	vmov.i8		d3, #0xFF		@ force alpha to 0xFF for first 8 RGBA pixels
	vmov.i8		d7, #0xFF		@ force alpha to 0xFF for last 8 RGBA pixels

	STORE_16xBGRA   nv12_rw, bgra2

        pop             {r4-r11,pc}

@ neon_nv12_to_RGBX32(uint8_t *yuv, uint8_t *y, uint8_t *uv,
@                  int w, int h, int yw, int cw, int dw)

#define bgra      r0
#define y1        r1
#define y2        r2
#define iuv       r3
#define nv12_rw   r4
#define nv12_l    r5
#define pxSz      r7
#define bgra2     r8

#define ye   d0
#define yo   d1
#define du  d30
#define dv  d31

        .global neon_nv12_to_RGBX32
neon_nv12_to_RGBX32:
        push            {r4-r11,lr}
        add             r4,  sp,  #36
        ldm             r4, {r4-r8}
        dmb

	mov 		pxSz, #4
	mla		bgra2, nv12_l, pxSz, bgra

	YUVtoRGB888_PREP

	vld2.8          {du, dv}, [iuv]              @ u0

	pld             [y1]
	vld2.8          {ye, yo},   [y1]        @ y0 interleaved

	YUVtoRGB888 d0,d4,d1,d5,d2,d6,ye,yo,du,dv,q3,q4,q5,q6,q7,q8,q9 @convert YUYV value into 16 RGBA pixels

	vmov.i8		d3, #0xFF		@ force alpha to 0xFF for first 8 RGBA pixels
	vmov.i8		d7, #0xFF		@ force alpha to 0xFF for last 8 RGBA pixels

	STORE_16xBGRA   nv12_rw, bgra

	pld             [y2]
	vld2.8          {ye, yo},   [y2]        @ y0 interleaved

	YUVtoRGB888 d0,d4,d1,d5,d2,d6,ye,yo,du,dv,q3,q4,q5,q6,q7,q8,q9 @convert YUYV value into 16 RGBA pixels

	vmov.i8		d3, #0xFF		@ force alpha to 0xFF for first 8 RGBA pixels
	vmov.i8		d7, #0xFF		@ force alpha to 0xFF for last 8 RGBA pixels

	STORE_16xBGRA   nv12_rw, bgra2

        pop             {r4-r11,pc}

@ yuv420_to_yuv422(uint8_t *yuv, uint8_t *y, uint8_t *u, uint8_t *v,
@                  int w, int h, int yw, int cw, int dw)

#define yuv  r0
#define y    r1
#define u    r2
#define v    r3
#define w    r4
#define h    r5
#define yw   r6
#define cw   r7
#define dw   r8

#define tyuv r9
#define ty   r10
#define tu   r11
#define tv   r12
#define i    lr

        .global neon_yuv420_to_YUYV
neon_yuv420_to_YUYV:
        push            {r4-r11,lr}
        add             r4,  sp,  #36
        ldm             r4, {r4-r8}
        dmb
1:
        mov             tu,   u
        mov             tv,   v
        vld1.64         {d2}, [u,:64], cw               @ u0
        vld1.64         {d3}, [v,:64], cw               @ v0
        mov             tyuv, yuv
        mov             ty,   y
        vzip.8          d2,   d3                        @ u0v0
        mov             i,    #16
2:                      
        pld             [y, #64]
        vld1.64         {d0, d1},   [y,:128], yw        @ y0
        pld             [u, #64]
        subs            i,    i,    #4
        vld1.64         {d6},       [u,:64],  cw        @ u2
        pld             [y, #64]
        vld1.64         {d4, d5},   [y,:128], yw        @ y1
        pld             [v, #64]
        vld1.64         {d7},       [v,:64],  cw        @ v2
        pld             [y, #64]
        vld1.64         {d16,d17},  [y,:128], yw        @ y2
        vzip.8          d6,   d7                        @ u2v2
        pld             [u, #64]
        vld1.64         {d22},      [u,:64],  cw        @ u4
        pld             [v, #64]
        vld1.64         {d23},      [v,:64],  cw        @ v4
        pld             [y, #64]
        vld1.64         {d20,d21},  [y,:128], yw        @ y3
        vmov            q9,   q3                        @ u2v2
        vzip.8          d22,  d23                       @ u4v4
        vrhadd.u8       q3,   q1,   q3                  @ u1v1
        vzip.8          q0,   q1                        @ y0u0y0v0
        vmov            q12,  q11                       @ u4v4
        vzip.8          q2,   q3                        @ y1u1y1v1
        vrhadd.u8       q11,  q9,   q11                 @ u3v3
        vst1.64         {d0-d3},    [yuv,:128], dw      @ y0u0y0v0
        vzip.8          q8,   q9                        @ y2u2y2v2
        vst1.64         {d4-d7},    [yuv,:128], dw      @ y1u1y1v1
        vzip.8          q10,  q11                       @ y3u3y3v3
        vst1.64         {d16-d19},  [yuv,:128], dw      @ y2u2y2v2
        vmov            q1,   q12
        vst1.64         {d20-d23},  [yuv,:128], dw      @ y3u3y3v3
        bgt             2b

        subs            w,    w,    #16
        add             yuv,  tyuv, #32
        add             y,    ty,   #16
        add             u,    tu,   #8
        add             v,    tv,   #8
        bgt             1b

        ldr             w,    [sp, #36]
        subs            h,    h,    #16
        add             yuv,  yuv,  dw, lsl #4
        sub             yuv,  yuv,  w,  lsl #1
        add             y,    y,    yw, lsl #4
        sub             y,    y,    w
        add             u,    u,    cw, lsl #3
        sub             u,    u,    w,  asr #1
        add             v,    v,    cw, lsl #3
        sub             v,    v,    w,  asr #1
        bgt             1b

        pop             {r4-r11,pc}

        .global neon_yuv420_to_UYVY
neon_yuv420_to_UYVY:
        push            {r4-r11,lr}
        add             r4,  sp,  #36
        ldm             r4, {r4-r8}
        dmb
1:
        mov             tu,   u
        mov             tv,   v
        vld1.64         {d2}, [u,:64], cw               @ u0
        vld1.64         {d3}, [v,:64], cw               @ v0
        mov             tyuv, yuv
        mov             ty,   y
        vzip.8          d2,   d3                        @ u0v0
        mov             i,    #16
2:                      
        pld             [y, #64]
        vld1.64         {d0, d1},   [y,:128], yw        @ y0
        pld             [u, #64]
        subs            i,    i,    #4
        vld1.64         {d6},       [u,:64],  cw        @ u2
        pld             [y, #64]
        vld1.64         {d4, d5},   [y,:128], yw        @ y1
        pld             [v, #64]
        vld1.64         {d7},       [v,:64],  cw        @ v2
        pld             [y, #64]
        vld1.64         {d16,d17},  [y,:128], yw        @ y2
        vzip.8          d6,   d7                        @ u2v2
        pld             [u, #64]
        vld1.64         {d22},      [u,:64],  cw        @ u4
        pld             [v, #64]
        vld1.64         {d23},      [v,:64],  cw        @ v4
        pld             [y, #64]
        vld1.64         {d20,d21},  [y,:128], yw        @ y3
        vmov            q9,   q3                        @ u2v2
        vzip.8          d22,  d23                       @ u4v4
        vrhadd.u8       q3,   q1,   q3                  @ u1v1
        vzip.8          q1,   q0                        @ y0u0y0v0
        vswp		q0,   q1
	
	vmov            q12,  q11                       @ u4v4
        vzip.8          q3,   q2                        @ y1u1y1v1
        vswp		q2,   q3
	
	vrhadd.u8       q11,  q9,   q11                 @ u3v3
        vst1.64         {d0-d3},    [yuv,:128], dw      @ y0u0y0v0
        vzip.8          q9,   q8                        @ y2u2y2v2
        vswp		q8,   q9
	
	vst1.64         {d4-d7},    [yuv,:128], dw      @ y1u1y1v1
        vzip.8          q11,  q10                       @ y3u3y3v3
        vswp		q10,  q11
	
	vst1.64         {d16-d19},  [yuv,:128], dw      @ y2u2y2v2
        vmov            q1,   q12
        vst1.64         {d20-d23},  [yuv,:128], dw      @ y3u3y3v3
        bgt             2b

        subs            w,    w,    #16
        add             yuv,  tyuv, #32
        add             y,    ty,   #16
        add             u,    tu,   #8
        add             v,    tv,   #8
        bgt             1b

        ldr             w,    [sp, #36]
        subs            h,    h,    #16
        add             yuv,  yuv,  dw, lsl #4
        sub             yuv,  yuv,  w,  lsl #1
        add             y,    y,    yw, lsl #4
        sub             y,    y,    w
        add             u,    u,    cw, lsl #3
        sub             u,    u,    w,  asr #1
        add             v,    v,    cw, lsl #3
        sub             v,    v,    w,  asr #1
        bgt             1b

        pop             {r4-r11,pc}

        .global neon_yuv422_to_UYVY
neon_yuv422_to_UYVY:
        push            {r4-r11,lr}
        add             r4,  sp,  #36
        ldm             r4, {r4-r8}
        dmb
1:
        mov             tu,   u
        mov             tv,   v
        mov             tyuv, yuv
        mov             ty,   y
        mov             i,    #16
2:                      
     	pld             [u, #64]
        vld1.64         {d2}, [u,:64], cw               @ u0
        pld             [v, #64]
        vld1.64         {d3}, [v,:64], cw               @ v0
        vzip.8          d2,   d3                        @ u0v0
        pld             [y, #64]
        vld1.64         {d0, d1},   [y,:128], yw        @ y0
       
        subs            i,    i,    #4
     	pld             [u, #64]
        vld1.64         {d6},       [u,:64],  cw        @ u1
        pld             [v, #64]
        vld1.64         {d7},       [v,:64],  cw        @ v1
        vzip.8          d6,   d7                        @ u1v1
        pld             [y, #64]
        vld1.64         {d4, d5},   [y,:128], yw        @ y1

     	pld             [u, #64]
        vld1.64         {d18},       [u,:64],  cw       @ u2
        pld             [v, #64]
        vld1.64         {d19},       [v,:64],  cw       @ v2
        vzip.8          d18,   d19                      @ u2v2
        pld             [y, #64]
        vld1.64         {d16,d17},  [y,:128], yw        @ y2


        pld             [u, #64]
        vld1.64         {d22},      [u,:64],  cw        @ u3
        pld             [v, #64]
        vld1.64         {d23},      [v,:64],  cw        @ v3
        vzip.8          d22,  d23                       @ u3v3
        pld             [y, #64]
        vld1.64         {d20,d21},  [y,:128], yw        @ y3
        
        vzip.8          q1,   q0                        @ y0u0y0v0
        vswp		q0,   q1
	
        vzip.8          q3,   q2                        @ y1u1y1v1
        vswp		q2,   q3

        vst1.64         {d0-d3},    [yuv,:128], dw      @ y0u0y0v0 store

        vzip.8          q9,   q8                        @ y2u2y2v2
        vswp		q8,   q9
	
	vst1.64         {d4-d7},    [yuv,:128], dw      @ y1u1y1v1 store
        
	vzip.8          q11,  q10                       @ y3u3y3v3
        vswp		q10,  q11
	
	vst1.64         {d16-d19},  [yuv,:128], dw      @ y2u2y2v2 store

        vst1.64         {d20-d23},  [yuv,:128], dw      @ y3u3y3v3 store
        
	bgt             2b

        subs            w,    w,    #16
        add             yuv,  tyuv, #32
        add             y,    ty,   #16
        add             u,    tu,   #8
        add             v,    tv,   #8
        bgt             1b

        ldr             w,    [sp, #36]
        subs            h,    h,    #16
        
	add             yuv,  yuv,  dw, lsl #4
        sub             yuv,  yuv,  w,  lsl #1
        
	add             y,    y,    yw, lsl #4
        sub             y,    y,    w
	
        add             u,    u,    cw, lsl #4
        sub             u,    u,    w,  asr #1
        
	add             v,    v,    cw, lsl #4
        sub             v,    v,    w,  asr #1
        bgt             1b

        pop             {r4-r11,pc}


	.macro	TURN_2_UYUV_PIXELS src0, src1, dst0, dst1, darker
	vld1.32		{d5[0]}, [\src0]	@load first pixel
	vld1.32		{d6[0]}, [\src1]	@load second one
	
	vrhadd.u8	d4, d5, d6		@perform (U1+U2)/2 and (V1+V2)/2, other fields not used

	cmp		\darker, #1		@test if darker is enabled
	bne		1f			@if it is not, go to store activity
	vmov.i8		d7, #80			@else
	vqsub.u8	d5, d5, d7		@perform Y = Y -80 (U and V not used)
	vqsub.u8	d6, d6, d7		@for both pixels

1:	adr		r7, YUV_tlb		@load destination pixels lookup table regarding computed values
	vld1.32		d7, [r7]		

	vtbl.i8		d8, {d4,d5,d6}, d7	@and fill the output data register regarding this table

	vst1.32		{d8[0]}, [\dst0]!	@finally, store the first pixel
	vst1.32		{d8[1]}, [\dst1]!	@ and the second one
	.endm

YUV_tlb:	.word 0x0B020900		@ pix1 = Dn[0] | Dn+1[1] << 8 | Dn[2] << 16 | Dn+1[3] <<24
		.word 0x13021100		@ pix2 = Dn[0] | Dn+2[1] << 8 | Dn[2] << 16 | Dn+2[3] <<24

	.global neon_copy_yuv_rotated_4
neon_copy_yuv_rotated_4:

	push	{r4-r12,lr}		@store used arm registers
	vpush	{q4-q7}			@store used neon registers
	ldr	r4, [sp, #0x68]		@get dst1
	ldr	r5, [sp, #0x6C]		@get dst2
	ldr	r6, [sp, #0x70]		@get darker

	TURN_2_UYUV_PIXELS r0, r1, r4, r5, r6 @rotate the two first pixels
	
	TURN_2_UYUV_PIXELS r2, r3, r4, r5, r6 @ rotate the two last

	vpop	{q4-q7}			@restore used neon registers
	pop	{r4-r12,pc}		@restore used arm registers	


#endif

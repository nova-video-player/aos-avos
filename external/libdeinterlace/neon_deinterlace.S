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


	.macro	COMPUTE_C tmp
        vmovl.u8    q3, d4
        vmovl.u8    q4, d5

        vabd.s16    q5, q3, q4 @abs(src1[x+0]-src2[x])

        vpaddl.s16  q6,q5
        mov         r7, #0
        vmov.u32    d13[1], r7
        vpadd.s32  d10,d12,d13
        vpadd.s32  d12,d10,d10

        vmov.32     r7, d12[0]
        vmov.32     \tmp, r7 
	.endm

        .macro	COMPUTE_C_STD x1, x2, tmp
        vext.8      d4, d0, d1, #\x1
        vext.8      d5, d2, d3, #\x2
        COMPUTE_C \tmp
        .endm

        .macro	COMPUTE_C_UP_SRC1 x1, x2, tmp
        vext.8      d4, d1, d0, #\x1
        vext.8      d5, d2, d3, #\x2
        COMPUTE_C \tmp
        .endm

        .macro	COMPUTE_C_UP_SRC2 x1, x2, tmp
        vext.8      d4, d0, d1, #\x1
        vext.8      d5, d3, d2, #\x2
        COMPUTE_C \tmp
        .endm

        .macro	COMPUTE_C_UP_SRC1_SRC2 x1, x2, tmp
        vext.8      d4, d1, d0, #\x1
        vext.8      d5, d3, d2, #\x2
        COMPUTE_C \tmp
        .endm

        .macro COMPUTE_DST   x1, x2, dst1, dst2
        vext.8      d10, d0, d1, #\x1
        vext.8      d11, d2, d3, #\x2
        vhadd.u8    \dst1, d10, d11
        .endm

        .global neon_XDeint8x8FieldC
        .func neon_XDeint8x8FieldC
neon_XDeint8x8FieldC:
        push        {r0,r1,r2,r7,lr}
        vld1.8      d0, [r1]! @load 16*8bits from src1
        vld1.8      d1, [r1]

        vld1.8      d2, [r2]! @load 16*8bits from src2
        vld1.8      d3, [r2]

        @C0
        COMPUTE_C_STD            0, 2, d20[0]
        COMPUTE_C_STD            1, 3, d20[1]
        COMPUTE_C_STD            2, 4, d21[0]
        COMPUTE_C_STD            3, 5, d21[1]
        COMPUTE_C_STD            4, 6, d22[0]
        COMPUTE_C_STD            5, 7, d22[1]
        COMPUTE_C_UP_SRC2        6, 0, d23[0]
        COMPUTE_C_UP_SRC2        7, 1, d23[1]

        @C1
        COMPUTE_C_STD            1, 1, d24[0]
        COMPUTE_C_STD            2, 2, d24[1]
        COMPUTE_C_STD            3, 3, d25[0]
        COMPUTE_C_STD            4, 4, d25[1]
        COMPUTE_C_STD            5, 5, d26[0]
        COMPUTE_C_STD            6, 6, d26[1]
        COMPUTE_C_STD            7, 7, d27[0]
        COMPUTE_C_UP_SRC1_SRC2   0, 0, d27[1]

        @C2
        COMPUTE_C_STD            2, 0, d28[0]
        COMPUTE_C_STD            3, 1, d28[1]
        COMPUTE_C_STD            4, 2, d29[0]
        COMPUTE_C_STD            5, 3, d29[1]
        COMPUTE_C_STD            6, 4, d30[0]
        COMPUTE_C_STD            7, 5, d30[1]
        COMPUTE_C_UP_SRC1        0, 6, d31[0]
        COMPUTE_C_UP_SRC1        1, 7, d31[1]

        COMPUTE_DST      4, 4, d16
        @if( c1 > c2 && c0 >= c1 )
        vcgt.s32   q2, q12, q14
        vcgt.s32   q3, q13, q15
        vcge.s32   q4, q10, q12
        vcge.s32   q5, q11, q13
        vtst.32    q2, q2, q4 
        vtst.32    q3, q3, q5
        vmovn.i32  d4, q2
        vmovn.i32  d5, q3
        vmovn.i16  d4, q2
        @dst[x] = (src1[5] + src2[3]) >> 1;
        COMPUTE_DST      5, 3, d12
        vbit       d16, d12, d4
        @if( c1 > c0 && c2 >= c1 )
        vcgt.s32   q2, q12, q10
        vcgt.s32   q3, q13, q11
        vcge.s32   q4, q14, q12
        vcge.s32   q5, q15, q13
        vtst.32    q2, q2, q4 
        vtst.32    q3, q3, q5
        vmovn.i32  d4, q2
        vmovn.i32  d5, q3
        vmovn.i16  d4, q2
        @dst[x] = (src1[3] + src2[5]) >> 1;
        COMPUTE_DST      3, 5, d12
        vbit       d16, d12, d4

        vst1.8    d16, [r0]!
        
        pop         {r0,r1,r2,r7,pc}
        .endfunc

        .global neon_XDeint8x8FieldC_pix
        .func neon_XDeint8x8FieldC_pix
neon_XDeint8x8FieldC_pix:
        push        {r1,r2,r7,lr}
        vld1.8      d0, [r1] @load 8*8bits from src1
        vld1.8      d1, [r2] @load 8*8bits from src2

        mov         r7, #0
        vmov.u8     d0[6], r7
        vmov.u8     d0[7], r7
        vmov.u8     d1[6], r7
        vmov.u8     d1[7], r7


        vmovl.u8    q1, d0
        vmovl.u8    q2, d1

        vsub.s16    q6, q1, q2 @src1[x+0]-src2[x]
        vabs.s16    q5, q6 @abs(src1[x+0]-src2[x])

        vpaddl.s16  q6,q5
        vpadd.s32  d10,d12,d13
        vpadd.s32  d12,d10,d10

        vmov.32     r0, d12[0]

        pop         {r1,r2,r7,pc}
        .endfunc

        .global neon_XDeint8x8FieldEC
        .func   neon_XDeint8x8FieldEC
neon_XDeint8x8FieldEC:
@ dst[x] = (src[x] + src[2*i_src+x] ) >> 1;
        push        {r0,r1,r2,lr}
        add         r2, r2, r2
        vld1.8      {d0}, [r1], r2
        vld1.8      {d1}, [r1]

        vmovl.u8    q4, d0
        vmovl.u8    q5, d1

        vadd.i16   q4, q4, q5 @src1[x] + src1[2*i_src1+x]
        
        vqshrn.u16  d0, q4, #1 @(src1[x] + src1[2*i_src1+x]) >> 1
        vst1.8      d0, [r0]
        pop         {r0,r1,r2,pc}
        .endfunc

        .global neon_XDeint8x8MergeC
        .func   neon_XDeint8x8MergeC
neon_XDeint8x8MergeC:
@ dst[x] = (src1[x] + 6*src2[x] + src1[i_src1+x] + 4 ) >> 3;
        push        {r0,r1,lr}
        vld1.8      {d0}, [r1], r3
        vld1.8      {d1}, [r1]
        vld1.8      {d2}, [r2]
        vmov.i16    q2, #6
        vmov.i16    q3, #4

        vmovl.u8    q4, d0
        vmovl.u8    q5, d1
        vmovl.u8    q6, d2

        vadd.i16   q4, q4, q5 @src1[x] + src1[i_src1+x]
        vmla.i16   q3, q6, q2 @6*src2[x] + 4
        vadd.i16   q3, q3, q4 @src1[x] + 6*src2[x] + src1[i_src1+x] + 4 
        
        vqshrn.u16    d0, q3, #3 @(src1[x] + 6*src2[x] + src1[i_src1+x] + 4 ) >> 3
        vst1.8      d0, [r0]
        pop         {r0,r1,pc}
        .endfunc

        .global neon_XDeint8x8DetectC
        .func   neon_XDeint8x8DetectC

neon_XDeint8x8DetectC:
        push        {r0,r5,lr}

        vld1.8      {d0}, [r0], r1 @src
        vld1.8      {d2}, [r0], r1 @src[i_src]
        vld1.8      {d4}, [r0], r1 @src[2*i_src]
        vld1.8      {d6}, [r0]     @src[3*i_src]

        vsubl.u8     q4, d0, d2
        vsubl.u8     q5, d2, d4
        vsubl.u8     q6, d0, d4
        vsubl.u8     q7, d2, d6

        vmull.s16    q8, d8, d8
        vmull.s16    q9, d9, d9
        vmull.s16    q10, d10, d10
        vmull.s16    q11, d11, d11

        vadd.s32     q8, q8, q10
        vadd.s32     q9, q9, q11

        vmull.s16    q12, d12, d12
        vmull.s16    q13, d13, d13
        vmull.s16    q14, d14, d14
        vmull.s16    q15, d15, d15

        vadd.s32     q12, q12, q14
        vadd.s32     q13, q13, q15

        vadd.s32     q8, q8, q9
        vadd.s32     d16, d16, d17
        vpadd.s32    d8, d16, d16

        vadd.s32     q12, q12, q13
        vadd.s32     d24, d24, d25
        vpadd.s32    d14, d24, d24

        vmov.32      r0, d8[0]
        str          r0, [r2]

        vmov.32      r5, d14[0]
        str          r5, [r3]

        pop         {r0,r5,pc}
        .endfunc


#endif

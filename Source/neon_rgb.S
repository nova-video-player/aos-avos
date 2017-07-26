#ifndef SIM
@   File:  neon_rgb.S
@
@   Revision:  November 4, 2009
@		neon_rgba8888_to_rgb565
@		neon_rgb565_to_rgba8888
@              	neon_copy_aligned32_rotated_CW270
@		neon_copy_aligned32
@
        .fpu neon
        .text

@ neon_rgba8888_to_rgb565( uint32_t *src, uint16_t *dst, int w, int h, int sw, int dw)

#define src   r0
#define dst   r1
#define w     r2
#define h     r3

#define sw    r4
#define dw    r5

#define tsrc  r7
#define tdst  r8
#define tsrc2 r9
#define tdst2 r10
#define tw    r11

        .global neon_rgba8888_to_rgb565
        .func   neon_rgba8888_to_rgb565
neon_rgba8888_to_rgb565:
        push            {r4-r11,lr}
        add             r4,  sp,  #36
        ldm             r4, {r4-r5}
        dmb

1:	
       	mov             tdst, dst				@ tdst = dst
 	add		tdst2, tdst, dw;     
        mov             tsrc, src				@ tsrc = src
 	add		tsrc2, tsrc, sw;     
        mov             tw,   w					@ tw   = w
2:	
	@ load 16 RGBA pixels
	pld          	[tsrc, #64]
	vld4.8		{d0, d2, d4, d6}, [tsrc,:128]!        	@ rgba -> d0,d2,d4,d6
 	pld          	[tsrc, #64]
	vld4.8 		{d8,d10,d12,d14}, [tsrc,:128]!        	@ rgba -> d8,d10,d12,d14
	
	@ load 16 RGBA pixels (2nd line)
	pld          	[tsrc2, #64]
	vld4.8		{d16,d18,d20,d22}, [tsrc2,:128]!        @ rgba -> d16,d18,d20,d22
 	pld          	[tsrc2, #64]
	vld4.8 		{d24,d26,d28,d30}, [tsrc2,:128]!        @ rgba -> d24,d26,d28,d30

 	subs            tw,    tw,    #16			@ w = w - 16

	@ put RGB data in MSB	
	vshll.u8	q0, d0, #8
	vshll.u8	q1, d2, #8
	vshll.u8	q2, d4, #8
	
	@ put RGB data in MSB	
	vshll.u8	q4,  d8, #8
	vshll.u8	q5, d10, #8
	vshll.u8	q6, d12, #8

	@ put RGB data in MSB	
	vshll.u8	q8, d16, #8
	vshll.u8	q9, d18, #8
	vshll.u8	q10,d20, #8
	
	@ put RGB data in MSB	
	vshll.u8	q12, d24, #8
	vshll.u8	q13, d26, #8
	vshll.u8	q14, d28, #8
	
	@ Pack 5-6-5 RGB values
	vsri.16		q0, q1, #5				@ R+G   -> q0
	vsri.16		q0, q2, #11				@ R+G+B -> q0
	vsri.16		q4, q5, #5				@ R+G   -> q4
	vsri.16		q4, q6, #11				@ R+G+B -> q4
	
	@ Pack 5-6-5 RGB values
	vsri.16		q8,  q9, #5				@ R+G   -> q8
	vsri.16		q8,  q10, #11				@ R+G+B -> q8
	vsri.16		q12, q13, #5				@ R+G   -> q12
	vsri.16		q12, q14, #11				@ R+G+B -> q12
	
	@ write 16 RGB565 pixels
	vst1.64        {d0-d1},   [tdst,:128]!       
	vst1.64        {d8-d9},   [tdst,:128]!       
	
	@ write 16 RGB565 pixels
	vst1.64        {d16-d17},   [tdst2,:128]!       
	vst1.64        {d24-d25},   [tdst2,:128]!       
    
	bgt 2b							@ if( w > 0 ) goto 2
	
	subs		h, h, #2				@ h = h - 2;
	
	add		src, src, sw, lsl #1;			@ src = src + 2 * sw;
	add		dst, dst, dw, lsl #1;			@ dst = dst + 2 * dw;
	
	bgt 1b							@ if( h > 0 ) goto 1
	
	pop             {r4-r11,pc}
        .endfunc

@ neon_rgb565_to_rgba8888( uint16_t *src, uint32_t *dst, int w, int h, int sw, int dw, int alpha)

#define src   r0
#define dst   r1
#define w     r2
#define h     r3

#define sw    r4
#define dw    r5
@#define alpha r6

#define tsrc  r7
#define tdst  r8
#define tsrc2 r9
#define tdst2 r10
#define tw    r11

        .global neon_rgb565_to_rgba8888
        .func   neon_rgb565_to_rgba8888
neon_rgb565_to_rgba8888:
        push            {r4-r11,lr}
        add             r4,  sp,  #36
        ldm             r4, {r4-r6}
        dmb

	@set common alpha value
	vdup.8		q3,  r6
	vdup.8		q7,  r6
	vdup.8		q11, r6
	vdup.8		q15, r6
	
1:	
       	mov             tdst, dst				@ tdst = dst
 	add		tdst2, tdst, dw;     
        mov             tsrc, src				@ tsrc = src
 	add		tsrc2, tsrc, sw;     
        mov             tw,   w					@ tw   = w
2:	
	@ load 16 RGB565 pixels
	pld          	[tsrc, #64]
	vld1.64        {d0-d1},   [tsrc,:128]!       
	pld          	[tsrc, #64]
	vld1.64        {d8-d9},   [tsrc,:128]!       
	
	@ load 16 RGB565 pixels (2nd line)
	pld          	[tsrc2, #64]
	vld1.64        {d16-d17}, [tsrc2,:128]!       
	pld          	[tsrc2, #64]
	vld1.64        {d24-d25}, [tsrc2,:128]!       

 	subs            tw,    tw,    #16			@ w = w - 16

	@ Unpack 5-6-5 RGB values
	vshrn.u16	 d2,  q0, #5				@ G xxGGGGGG
	vshrn.u16	 d4,  q0, #0				@ B xxxBBBBB
	vshr.u16	 q0,  q0, #11				@ R xxxRRRRR
	vshrn.u16	 d0,  q0, #0				@ 
	
	vshrn.u16	d10,  q4, #5				@ G xxGGGGGG
	vshrn.u16	d12,  q4, #0				@ B xxxBBBBB
	vshr.u16	 q4,  q4, #11				@ R xxxRRRRR
	vshrn.u16	 d8,  q4, #0				@ 

	vshrn.u16	d18,  q8, #5				@ G xxGGGGGG
	vshrn.u16	d20,  q8, #0				@ B xxxBBBBB
	vshr.u16	 q8,  q8, #11				@ R xxxRRRRR
	vshrn.u16	d16,  q8, #0				@ 

	vshrn.u16	d26, q12, #5				@ G xxGGGGGG
	vshrn.u16	d28, q12, #0				@ B xxxBBBBB
	vshr.u16	q12, q12, #11				@ R xxxRRRRR
	vshrn.u16	d24, q12, #0				@ 

	@ shift RGB data to MSB	
	vshl.u8		 d0,  d0, #3
	vshl.u8		 d2,  d2, #2
	vshl.u8		 d4,  d4, #3

	vshl.u8		 d8,  d8, #3
	vshl.u8		d10, d10, #2
	vshl.u8		d12, d12, #3

	vshl.u8		d16, d16, #3
	vshl.u8		d18, d18, #2
	vshl.u8		d20, d20, #3

	vshl.u8		d24, d24, #3
	vshl.u8		d26, d26, #2
	vshl.u8		d28, d28, #3
	
	@ write 16 RGBA pixels
	vst4.8		{d0, d2, d4, d6}, [tdst,:128]!        	@ rgba -> d0,d2,d4,d6
	vst4.8 		{d8,d10,d12,d14}, [tdst,:128]!        	@ rgba -> d8,d10,d12,d14
	
	@ write 16 RGBA pixels
	vst4.8		{d16,d18,d20,d22}, [tdst2,:128]!        @ rgba -> d16,d18,d20,d22
 	vst4.8 		{d24,d26,d28,d30}, [tdst2,:128]!        @ rgba -> d24,d26,d28,d30
	bgt 2b							@ if( w > 0 ) goto 2
	
	subs		h, h, #2				@ h = h - 2;
	
	add		src, src, sw, lsl #1;			@ src = src + sw * 2;
	add		dst, dst, dw, lsl #1;			@ dst = dst + dw * 2;
	
	bgt 1b							@ if( h > 0 ) goto 1

	pop             {r4-r11,pc}
        .endfunc

@add Rotation stuff
@----------------------------------------------------------------------------
@ MACROS
@----------------------------------------------------------------------------

#define	srcBuffer	r0
#define	dstBuffer	r1
#define	areaHeight	r2
#define	areaWidth	r3
#define	srcStride	r4
#define	dstStride	r5
#define globalAlpha	r6
#define	step		r7
#define temp		r8
#define areaHeight_orig	r9
#define areaWidth_orig	r10
#define srcBuffer_orig	r11
#define dstBuffer_orig	r12	

@------------------------------------------------------------
@
@	LOAD_MATRIX_8x8 
@	Load a block of 8x8 RGBA pixels from a src buffer
@	that is needs stride byte to go to same pixel, one 
@	line below.
@	It uses d0-d31 neon registers
@	It takes care of alignement for burst optimization
@	Minimum alignement is RGBA element size (4 bytes)
@
@------------------------------------------------------------

	.macro LOAD_MATRIX_8x8	srcBuffer, stride
	and	temp, \srcBuffer, #0xF	@aligned on 0x10?
	cmp	temp, #0
	andeq   temp, \stride, #0xF
	cmpeq	temp, #0
	bne	1f
	sub	step, \stride, #0x20
	pld	[srcBuffer]
	vld2.32 {d0, d1}, [\srcBuffer]!
	vld2.32 {d8, d9}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d2, d3}, [\srcBuffer]!
	vld2.32 {d10, d11}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d4, d5}, [\srcBuffer]!
	vld2.32 {d12, d13}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d6, d7}, [\srcBuffer]!
	vld2.32 {d14, d15}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d16, d17}, [\srcBuffer]!
	vld2.32 {d24, d25}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d18, d19}, [\srcBuffer]!
	vld2.32 {d26, d27}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d20, d21}, [\srcBuffer]!
	vld2.32 {d28, d29}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d22, d23}, [\srcBuffer]!
	vld2.32 {d30, d31}, [\srcBuffer]!
	b 	3f
1:
	and	temp, \srcBuffer, #0x7	@aligned on 0x8?
	cmp	temp, #0
	andeq   temp, \stride, #0x7
	cmpeq	temp, #0
	bne	2f
	sub	step, \stride, #0x20
	pld	[srcBuffer]
	vld2.32 {d0[0], d1[0]}, [\srcBuffer]!
	vld2.32 {d0[1], d1[1]}, [\srcBuffer]!
	vld2.32 {d8[0], d9[0]}, [\srcBuffer]!
	vld2.32 {d8[1], d9[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d2[0], d3[0]}, [\srcBuffer]!
	vld2.32 {d2[1], d3[1]}, [\srcBuffer]!
	vld2.32 {d10[0], d11[0]}, [\srcBuffer]!
	vld2.32 {d10[1], d11[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d4[0], d5[0]}, [\srcBuffer]!
	vld2.32 {d4[1], d5[1]}, [\srcBuffer]!
	vld2.32 {d12[0], d13[0]}, [\srcBuffer]!
	vld2.32 {d12[1], d13[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d6[0], d7[0]}, [\srcBuffer]!
	vld2.32 {d6[1], d7[1]}, [\srcBuffer]!
	vld2.32 {d14[0], d15[0]}, [\srcBuffer]!
	vld2.32 {d14[1], d15[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d16[0], d17[0]}, [\srcBuffer]!
	vld2.32 {d16[1], d17[1]}, [\srcBuffer]!
	vld2.32 {d24[0], d25[0]}, [\srcBuffer]!
	vld2.32 {d24[1], d25[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d18[0], d19[0]}, [\srcBuffer]!
	vld2.32 {d18[1], d19[1]}, [\srcBuffer]!
	vld2.32 {d26[0], d27[0]}, [\srcBuffer]!
	vld2.32 {d26[1], d27[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d20[0], d21[0]}, [\srcBuffer]!
	vld2.32 {d20[1], d21[1]}, [\srcBuffer]!
	vld2.32 {d28[0], d29[0]}, [\srcBuffer]!
	vld2.32 {d28[1], d29[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d22[0], d23[0]}, [\srcBuffer]!
	vld2.32 {d22[1], d23[1]}, [\srcBuffer]!
	vld2.32 {d30[0], d31[0]}, [\srcBuffer]!
	vld2.32 {d30[1], d31[1]}, [\srcBuffer]!
	b 	3f
2:
	and	temp, \srcBuffer, #0x3	@aligned on 0x4?
	cmp	temp, #0
	andeq   temp, \stride, #0x3
	cmpeq	temp, #0
	bne	3f
	sub	step, \stride, #0x20
	pld	[srcBuffer]
	vld1.32 {d0[0]}, [\srcBuffer]!
	vld1.32 {d1[0]}, [\srcBuffer]!
	vld1.32 {d0[1]}, [\srcBuffer]!
	vld1.32 {d1[1]}, [\srcBuffer]!
	vld1.32 {d8[0]}, [\srcBuffer]!
	vld1.32 {d9[0]}, [\srcBuffer]!
	vld1.32 {d8[1]}, [\srcBuffer]!
	vld1.32 {d9[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld1.32 {d2[0]}, [\srcBuffer]!
	vld1.32 {d3[0]}, [\srcBuffer]!
	vld1.32 {d2[1]}, [\srcBuffer]!
	vld1.32 {d3[1]}, [\srcBuffer]!
	vld1.32 {d10[0]}, [\srcBuffer]!
	vld1.32 {d11[0]}, [\srcBuffer]!
	vld1.32 {d10[1]}, [\srcBuffer]!
	vld1.32 {d11[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld1.32 {d4[0]}, [\srcBuffer]!
	vld1.32 {d5[0]}, [\srcBuffer]!
	vld1.32 {d4[1]}, [\srcBuffer]!
	vld1.32 {d5[1]}, [\srcBuffer]!
	vld1.32 {d12[0]}, [\srcBuffer]!
	vld1.32 {d13[0]}, [\srcBuffer]!
	vld1.32 {d12[1]}, [\srcBuffer]!
	vld1.32 {d13[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld1.32 {d6[0]}, [\srcBuffer]!
	vld1.32 {d7[0]}, [\srcBuffer]!
	vld1.32 {d6[1]}, [\srcBuffer]!
	vld1.32 {d7[1]}, [\srcBuffer]!
	vld1.32 {d14[0]}, [\srcBuffer]!
	vld1.32 {d15[0]}, [\srcBuffer]!
	vld1.32 {d14[1]}, [\srcBuffer]!
	vld1.32 {d15[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld1.32 {d16[0]}, [\srcBuffer]!
	vld1.32 {d17[0]}, [\srcBuffer]!
	vld1.32 {d16[1]}, [\srcBuffer]!
	vld1.32 {d17[1]}, [\srcBuffer]!
	vld1.32 {d24[0]}, [\srcBuffer]!
	vld1.32 {d25[0]}, [\srcBuffer]!
	vld1.32 {d24[1]}, [\srcBuffer]!
	vld1.32 {d25[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld1.32 {d18[0]}, [\srcBuffer]!
	vld1.32 {d19[0]}, [\srcBuffer]!
	vld1.32 {d18[1]}, [\srcBuffer]!
	vld1.32 {d19[1]}, [\srcBuffer]!
	vld1.32 {d26[0]}, [\srcBuffer]!
	vld1.32 {d27[0]}, [\srcBuffer]!
	vld1.32 {d26[1]}, [\srcBuffer]!
	vld1.32 {d27[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld1.32 {d20[0]}, [\srcBuffer]!
	vld1.32 {d21[0]}, [\srcBuffer]!
	vld1.32 {d20[1]}, [\srcBuffer]!
	vld1.32 {d21[1]}, [\srcBuffer]!
	vld1.32 {d28[0]}, [\srcBuffer]!
	vld1.32 {d29[0]}, [\srcBuffer]!
	vld1.32 {d28[1]}, [\srcBuffer]!
	vld1.32 {d29[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld1.32 {d22[0]}, [\srcBuffer]!
	vld1.32 {d23[0]}, [\srcBuffer]!
	vld1.32 {d22[1]}, [\srcBuffer]!
	vld1.32 {d23[1]}, [\srcBuffer]!
	vld1.32 {d30[0]}, [\srcBuffer]!
	vld1.32 {d31[0]}, [\srcBuffer]!
	vld1.32 {d30[1]}, [\srcBuffer]!
	vld1.32 {d31[1]}, [\srcBuffer]!
3:
	.endm

@------------------------------------------------------------
@
@	LOAD_MATRIX_4x4 
@	Load a block of 4x4 RGBA pixels from a srcBuffer buffer
@	that is needs stride byte to go to same pixel, one 
@	line below.
@	It uses d0-d7 neon registers
@	It takes care of alignement for burst optimization
@	Minimum alignement is RGBA element size (4 bytes)
@
@------------------------------------------------------------

	.macro LOAD_MATRIX_4x4	srcBuffer, stride
	and	temp, \srcBuffer, #0xF	@aligned on 0x10?
	cmp	temp, #0
	andeq   temp, \stride, #0xF
	cmpeq	temp, #0
	bne	1f
	sub	step, \stride, #0x10
	pld	[srcBuffer]
	vld2.32 {d0, d1}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d2, d3}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d4, d5}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d6, d7}, [\srcBuffer]!
	b 	3f
1:
	and	temp, \srcBuffer, #0x7	@aligned on 0x8?
	cmp	temp, #0
	andeq   temp, \stride, #0x7
	cmpeq	temp, #0
	bne	2f
	sub	step, \stride, #0x10
	pld	[srcBuffer]
	vld2.32 {d0[0], d1[0]}, [\srcBuffer]!
	vld2.32 {d0[1], d1[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d2[0], d3[0]}, [\srcBuffer]!
	vld2.32 {d2[1], d3[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d4[0], d5[0]}, [\srcBuffer]!
	vld2.32 {d4[1], d5[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld2.32 {d6[0], d7[0]}, [\srcBuffer]!
	vld2.32 {d6[1], d7[1]}, [\srcBuffer]!
	b	3f
2:
	and	temp, \srcBuffer, #0x3	@aligned on 0x4?
	cmp	temp, #0
	andeq   temp, \stride, #0x3
	cmpeq	temp, #0
	bne	3f
	sub	step, \stride, #0x10
	pld	[srcBuffer]
	vld1.32 {d0[0]}, [\srcBuffer]!
	vld1.32 {d1[0]}, [\srcBuffer]!
	vld1.32 {d0[1]}, [\srcBuffer]!
	vld1.32 {d1[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld1.32 {d2[0]}, [\srcBuffer]!
	vld1.32 {d3[0]}, [\srcBuffer]!
	vld1.32 {d2[1]}, [\srcBuffer]!
	vld1.32 {d3[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld1.32 {d4[0]}, [\srcBuffer]!
	vld1.32 {d5[0]}, [\srcBuffer]!
	vld1.32 {d4[1]}, [\srcBuffer]!
	vld1.32 {d5[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld1.32 {d6[0]}, [\srcBuffer]!
	vld1.32 {d7[0]}, [\srcBuffer]!
	vld1.32 {d6[1]}, [\srcBuffer]!
	vld1.32 {d7[1]}, [\srcBuffer]!
3:
	.endm

@------------------------------------------------------------
@
@	LOAD_MATRIX_2x2 
@	Load a block of 2x2 RGBA pixels from a srcBuffer buffer
@	that is needs stride byte to go to same pixel, one 
@	line below.
@	It uses d0-d1 neon registers
@	It takes care of alignement for burst optimization
@	Minimum alignement is RGBA element size (4 bytes)
@
@------------------------------------------------------------

	.macro LOAD_MATRIX_2x2	srcBuffer, stride
	and	temp, \srcBuffer, #0x7	@aligned on 0x8?
	cmp	temp, #0
	andeq   temp, \stride, #0x7
	cmpeq	temp, #0
	bne	1f
	sub	step, \stride, #0x8
	pld	[srcBuffer]
	vld1.32 {d0}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld1.32 {d1}, [\srcBuffer]!
	b	2f
1:	
	and	temp, \srcBuffer, #0x3	@aligned on 0x4?
	cmp	temp, #0
	andeq   temp, \stride, #0x3
	cmpeq	temp, #0
	bne	2f
	sub	step, \stride, #0x8
	pld	[srcBuffer]
	vld1.32 {d0[0]}, [\srcBuffer]!
	vld1.32 {d0[1]}, [\srcBuffer]!
	add	\srcBuffer, \srcBuffer, step
	pld	[srcBuffer]
	vld1.32 {d1[0]}, [\srcBuffer]!
	vld1.32 {d1[1]}, [\srcBuffer]!
2:
	.endm

@------------------------------------------------------------
@
@	SAVE_270CW_MATRIX_2x2 
@	SAve a block of 2x2 RGBA pixels to a dstBuffer buffer
@	that is needs stride byte to go to same pixel, one 
@	line below. During copy, the block is rotated by 
@	270 degrees clockwise
@	It uses d0-d1 neon registers
@	It takes care of alignement for burst optimization
@	Minimum alignement is RGBA element size (4 bytes)
@
@------------------------------------------------------------

	.macro SAVE_270CW_MATRIX_2x2	dstBuffer, dstStride
	and	temp, \dstBuffer, #0x7	@aligned on 0x8?
	cmp	temp, #0
	andeq   temp, \dstStride, #0x7
	cmpeq	temp, #0
	bne	1f
	add	step, \dstStride, #0x8	
	pld	[dstBuffer]
	vst2.32	{d0[0],d1[0]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst2.32	{d0[1],d1[1]}, [\dstBuffer]!
	b	2f
1:
	and	temp, \dstBuffer, #0x3	@aligned on 0x4?
	cmp	temp, #0
	andeq   temp, \dstStride, #0x3
	cmpeq	temp, #0
	bne	2f
	add	step, \dstStride, #0x8	
	pld	[dstBuffer]
	vst1.32	{d0[0]}, [\dstBuffer]!
	vst1.32 {d1[0]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst1.32	{d0[1]}, [\dstBuffer]!
	vst1.32 {d1[1]}, [\dstBuffer]!
2:
	.endm

@------------------------------------------------------------
@
@	SAVE_270CW_MATRIX_4x4 
@	SAve a block of 4x4 RGBA pixels to a dstBuffer buffer
@	that is needs stride byte to go to same pixel, one 
@	line below. During copy, the block is rotated by 
@	270 degrees clockwise
@	It uses d0-d7 neon registers
@	It takes care of alignement for burst optimization
@	Minimum alignement is RGBA element size (4 bytes)
@
@------------------------------------------------------------

	.macro SAVE_270CW_MATRIX_4x4	dstBuffer, dstStride
	and	temp, \dstBuffer, #0xF		@aligned on 0x10?
	cmp	temp, #0
	andeq   temp, \dstStride, #0xF
	cmpeq	temp, #0
	bne	1f
	add	step, \dstStride, #0x10
	pld	[dstBuffer]
	vst4.32	{d0[0],d2[0],d4[0],d6[0]}, [\dstBuffer]!	
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst4.32	{d1[0],d3[0],d5[0],d7[0]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst4.32	{d0[1],d2[1],d4[1],d6[1]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst4.32	{d1[1],d3[1],d5[1],d7[1]}, [\dstBuffer]!	
	b	3f
1:
	and	temp, \dstBuffer, #0x7		@aligned on 0x8?
	cmp	temp, #0
	andeq   temp, \dstStride, #0x7
	cmpeq	temp, #0
	bne	2f
	add	step, \dstStride, #0x10
	pld	[dstBuffer]
	vst2.32	{d0[0],d2[0]}, [\dstBuffer]!
	vst2.32 {d4[0],d6[0]}, [\dstBuffer]!	
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst2.32	{d1[0],d3[0]}, [\dstBuffer]!
	vst2.32 {d5[0],d7[0]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst2.32	{d0[1],d2[1]}, [\dstBuffer]!
	vst2.32 {d4[1],d6[1]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst2.32	{d1[1],d3[1]}, [\dstBuffer]!
	vst2.32 {d5[1],d7[1]}, [\dstBuffer]!	
	b	3f
2:
	and	temp, \dstBuffer, #0x3		@aligned on 0x4?
	cmp	temp, #0
	andeq   temp, \dstStride, #0x3
	cmpeq	temp, #0
	bne	3f
	add	step, \dstStride, #0x10
	pld	[dstBuffer]
	vst1.32	{d0[0]}, [\dstBuffer]!
	vst1.32 {d2[0]}, [\dstBuffer]!
	vst1.32 {d4[0]}, [\dstBuffer]!
	vst1.32 {d6[0]}, [\dstBuffer]!	
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst1.32	{d1[0]}, [\dstBuffer]!
	vst1.32 {d3[0]}, [\dstBuffer]!
	vst1.32 {d5[0]}, [\dstBuffer]!
	vst1.32 {d7[0]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst1.32	{d0[1]}, [\dstBuffer]!
	vst1.32 {d2[1]}, [\dstBuffer]!
	vst1.32 {d4[1]}, [\dstBuffer]!
	vst1.32 {d6[1]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst1.32	{d1[1]}, [\dstBuffer]!
	vst1.32 {d3[1]}, [\dstBuffer]!
	vst1.32 {d5[1]}, [\dstBuffer]!
	vst1.32 {d7[1]}, [\dstBuffer]!
3:
	.endm

@------------------------------------------------------------
@
@	SAVE_270CW_MATRIX_8x8 
@	SAve a block of 8x8 RGBA pixels to a dstBuffer buffer
@	that is needs stride byte to go to same pixel, one 
@	line below. During copy, the block is rotated by 
@	270 degrees clockwise
@	It uses d0-d31 neon registers
@	It takes care of alignement for burst optimization
@	Minimum alignement is RGBA element size (4 bytes)
@
@------------------------------------------------------------

	.macro SAVE_270CW_MATRIX_8x8	dstBuffer, dstStride
	and	temp, \dstBuffer, #0xF		@aligned on 0x10?
	cmp	temp, #0
	andeq   temp, \dstStride, #0xF
	cmpeq	temp, #0
	bne	1f
	add	step, \dstStride, #0x20
	pld	[dstBuffer]
	vst4.32	{d0[0],d2[0],d4[0],d6[0]}, [\dstBuffer]!
	vst4.32	{d16[0],d18[0],d20[0],d22[0]}, [\dstBuffer]!	
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst4.32	{d1[0],d3[0],d5[0],d7[0]}, [\dstBuffer]!
	vst4.32	{d17[0],d19[0],d21[0],d23[0]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst4.32	{d0[1],d2[1],d4[1],d6[1]}, [\dstBuffer]!
	vst4.32	{d16[1],d18[1],d20[1],d22[1]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst4.32	{d1[1],d3[1],d5[1],d7[1]}, [\dstBuffer]!
	vst4.32	{d17[1],d19[1],d21[1],d23[1]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst4.32	{d8[0],d10[0],d12[0],d14[0]}, [\dstBuffer]!
	vst4.32	{d24[0],d26[0],d28[0],d30[0]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst4.32	{d9[0],d11[0],d13[0],d15[0]}, [\dstBuffer]!
	vst4.32	{d25[0],d27[0],d29[0],d31[0]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst4.32	{d8[1],d10[1],d12[1],d14[1]}, [\dstBuffer]!
	vst4.32	{d24[1],d26[1],d28[1],d30[1]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst4.32	{d9[1],d11[1],d13[1],d15[1]}, [\dstBuffer]!
	vst4.32	{d25[1],d27[1],d29[1],d31[1]}, [\dstBuffer]!
	b	3f
1:
	and	temp, \dstBuffer, #0x7		@aligned on 0x8?
	cmp	temp, #0
	andeq   temp, \dstStride, #0x7
	cmpeq	temp, #0
	bne 	2f
	
	add	step, \dstStride, #0x20
	pld	[dstBuffer]
	vst2.32	{d0[0],d2[0]}, [\dstBuffer]!
	vst2.32 {d4[0],d6[0]}, [\dstBuffer]!
	vst2.32 {d16[0],d18[0]}, [\dstBuffer]!
	vst2.32 {d20[0],d22[0]}, [\dstBuffer]!	
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst2.32	{d1[0],d3[0]}, [\dstBuffer]!
	vst2.32 {d5[0],d7[0]}, [\dstBuffer]!
	vst2.32	{d17[0],d19[0]}, [\dstBuffer]!
	vst2.32 {d21[0],d23[0]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst2.32	{d0[1],d2[1]}, [\dstBuffer]!
	vst2.32 {d4[1],d6[1]}, [\dstBuffer]!
	vst2.32	{d16[1],d18[1]}, [\dstBuffer]!
	vst2.32 {d20[1],d22[1]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst2.32	{d1[1],d3[1]}, [\dstBuffer]!
	vst2.32 {d5[1],d7[1]}, [\dstBuffer]!
	vst2.32	{d17[1],d19[1]}, [\dstBuffer]!
	vst2.32 {d21[1],d23[1]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst2.32	{d8[0],d10[0]}, [\dstBuffer]!
	vst2.32 {d12[0],d14[0]}, [\dstBuffer]!
	vst2.32	{d24[0],d26[0]}, [\dstBuffer]!
	vst2.32 {d28[0],d30[0]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst2.32	{d9[0],d11[0]}, [\dstBuffer]!
	vst2.32 {d13[0],d15[0]}, [\dstBuffer]!
	vst2.32	{d25[0],d27[0]}, [\dstBuffer]!
	vst2.32 {d29[0],d31[0]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst2.32	{d8[1],d10[1]}, [\dstBuffer]!
	vst2.32 {d12[1],d14[1]}, [\dstBuffer]!
	vst2.32	{d24[1],d26[1]}, [\dstBuffer]!
	vst2.32 {d28[1],d30[1]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst2.32	{d9[1],d11[1]}, [\dstBuffer]!
	vst2.32 {d13[1],d15[1]}, [\dstBuffer]!
	vst2.32	{d25[1],d27[1]}, [\dstBuffer]!
	vst2.32 {d29[1],d31[1]}, [\dstBuffer]!
	b 	3f
2:
	and	temp, \dstBuffer, #0x3		@aligned on 0x4?
	cmp	temp, #0
	andeq   temp, \dstStride, #0x3
	cmpeq	temp, #0
	bne	3f
	add	step, \dstStride, #0x20
	pld	[dstBuffer]
	vst1.32	{d0[0]}, [\dstBuffer]!
	vst1.32	{d2[0]}, [\dstBuffer]!
	vst1.32 {d4[0]}, [\dstBuffer]!
	vst1.32	{d6[0]}, [\dstBuffer]!
	vst1.32 {d16[0]}, [\dstBuffer]!
	vst1.32	{d18[0]}, [\dstBuffer]!
	vst1.32 {d20[0]}, [\dstBuffer]!
	vst1.32	{d22[0]}, [\dstBuffer]!	
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst1.32	{d1[0]}, [\dstBuffer]!
	vst1.32	{d3[0]}, [\dstBuffer]!
	vst1.32 {d5[0]}, [\dstBuffer]!
	vst1.32	{d7[0]}, [\dstBuffer]!
	vst1.32	{d17[0]}, [\dstBuffer]!
	vst1.32	{d19[0]}, [\dstBuffer]!
	vst1.32 {d21[0]}, [\dstBuffer]!
	vst1.32	{d23[0]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst1.32	{d0[1]}, [\dstBuffer]!
	vst1.32	{d2[1]}, [\dstBuffer]!
	vst1.32 {d4[1]}, [\dstBuffer]!
	vst1.32	{d6[1]}, [\dstBuffer]!
	vst1.32	{d16[1]}, [\dstBuffer]!
	vst1.32	{d18[1]}, [\dstBuffer]!
	vst1.32 {d20[1]}, [\dstBuffer]!
	vst1.32	{d22[1]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst1.32	{d1[1]}, [\dstBuffer]!
	vst1.32	{d3[1]}, [\dstBuffer]!
	vst1.32 {d5[1]}, [\dstBuffer]!
	vst1.32	{d7[1]}, [\dstBuffer]!
	vst1.32	{d17[1]}, [\dstBuffer]!
	vst1.32	{d19[1]}, [\dstBuffer]!
	vst1.32 {d21[1]}, [\dstBuffer]!
	vst1.32	{d23[1]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst1.32	{d8[0]}, [\dstBuffer]!
	vst1.32	{d10[0]}, [\dstBuffer]!
	vst1.32 {d12[0]}, [\dstBuffer]!
	vst1.32	{d14[0]}, [\dstBuffer]!
	vst1.32	{d24[0]}, [\dstBuffer]!
	vst1.32	{d26[0]}, [\dstBuffer]!
	vst1.32 {d28[0]}, [\dstBuffer]!
	vst1.32	{d30[0]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst1.32	{d9[0]}, [\dstBuffer]!
	vst1.32	{d11[0]}, [\dstBuffer]!
	vst1.32 {d13[0]}, [\dstBuffer]!
	vst1.32	{d15[0]}, [\dstBuffer]!
	vst1.32	{d25[0]}, [\dstBuffer]!
	vst1.32	{d27[0]}, [\dstBuffer]!
	vst1.32 {d29[0]}, [\dstBuffer]!
	vst1.32	{d31[0]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst1.32	{d8[1]}, [\dstBuffer]!
	vst1.32	{d10[1]}, [\dstBuffer]!
	vst1.32 {d12[1]}, [\dstBuffer]!
	vst1.32	{d14[1]}, [\dstBuffer]!
	vst1.32	{d24[1]}, [\dstBuffer]!
	vst1.32	{d26[1]}, [\dstBuffer]!
	vst1.32 {d28[1]}, [\dstBuffer]!
	vst1.32	{d30[1]}, [\dstBuffer]!
	sub	\dstBuffer, \dstBuffer, step
	pld	[dstBuffer]
	vst1.32	{d9[1]}, [\dstBuffer]!
	vst1.32	{d11[1]}, [\dstBuffer]!
	vst1.32 {d13[1]}, [\dstBuffer]!
	vst1.32	{d15[1]}, [\dstBuffer]!
	vst1.32	{d25[1]}, [\dstBuffer]!
	vst1.32	{d27[1]}, [\dstBuffer]!
	vst1.32 {d29[1]}, [\dstBuffer]!
	vst1.32	{d31[1]}, [\dstBuffer]!
3:	
	.endm

@------------------------------------------------------------
@
@	UPDATE_AREA_CW270_32 
@	It updates all variable after a 270CW rotated block 
@	of blk_Sizexblk_Size has been processed. If next block  
@	has the same size, it branches on Copy_loop otherwise, 
@	it recursively call the 270CW main process until all pixels
@	has been processed
@
@------------------------------------------------------------

	.macro	UPDATE_AREA_CW270_32	blk_Size, Copy_loop

	sub	areaWidth, areaWidth, \blk_Size
	cmp	areaWidth, \blk_Size
	bge	\Copy_loop
	sub	areaHeight, areaHeight, \blk_Size
	cmp	areaHeight, \blk_Size
	bge	1f
	b	2f
1:
	mov	areaWidth, areaWidth_orig
	b	\Copy_loop
2:
	@ process first area
	mov	temp, areaHeight
	mov 	areaHeight, areaHeight_orig

	UPDATE_BUFFER_ADDRESS_CW270_32
	push	{globalAlpha}
	push	{dstStride}
	push	{srcStride}
	bl	neon_copy_aligned32_rotated_CW270	@recursive call
	pop	{srcStride}
	pop	{dstStride}
	pop	{globalAlpha}
	@ process second area
	mov	areaHeight, temp
	mov	temp, areaWidth
	mov 	areaWidth, areaWidth_orig
	UPDATE_BUFFER_ADDRESS_CW270_32
	push	{globalAlpha}
	push	{dstStride}
	push	{srcStride}
	sub	areaWidth, areaWidth, temp 	@ reduce area width since this part has already been processed
	bl	neon_copy_aligned32_rotated_CW270	@recursive call
	pop	{srcStride}
	pop	{dstStride}
	pop	{globalAlpha}
	b 	exit
	.endm

@------------------------------------------------------------
@
@	UPDATE_BUFFER_ADDRESS_CW270_32 
@	It updates buffer addresses regarding global dimension
@	and already processed area 
@
@------------------------------------------------------------

	.macro	UPDATE_BUFFER_ADDRESS_CW270_32
	sub	dstBuffer, areaWidth_orig, areaWidth
	mul	dstBuffer, dstStride, dstBuffer
	sub	dstBuffer, dstBuffer_orig, dstBuffer
	add	dstBuffer, dstBuffer, areaHeight_orig, lsl #2
	sub	dstBuffer, dstBuffer, areaHeight, lsl #2

	sub	srcBuffer, areaHeight_orig, areaHeight
	mla	srcBuffer, srcStride, srcBuffer, srcBuffer_orig
	add	srcBuffer, srcBuffer, areaWidth_orig, lsl #2
	sub	srcBuffer, srcBuffer, areaWidth, lsl #2
	.endm

@------------------------------------------------------------
@
@	GLOBAL_ALPHA_CHANNEL_2x2 
@	It apply a global alpha on all pixels in a 2x2 block
@	A = A - (255 - Alpha) saturated
@
@------------------------------------------------------------

	.macro GLOBAL_ALPHA_CHANNEL_2x2 alphaChannel
	vmov.i32 q4, #0xFF000000
	vmov.i32 d10, #0x00000000
	vmov.i8  d10[3], \alphaChannel
	vmov.i8  d10[7], \alphaChannel

	vqsub.u8	d8, d8, d10
	vqsub.u8	d9, d9, d10

	@ qx = qx - q4 
	vqsub.u8	q0, q0, q4
	
	.endm

@------------------------------------------------------------
@
@	GLOBAL_ALPHA_CHANNEL_4x4
@	It apply a global alpha on all pixels in a 4x4 block
@	A = A - (255 - Alpha) saturated
@	Note that 8x8 block can not be processed since no more 
@	neon registers are free
@
@------------------------------------------------------------
	.macro GLOBAL_ALPHA_CHANNEL_4x4 alphaChannel
	vmov.i32 q4, #0xFF000000
	vmov.i32 d10, #0x00000000
	vmov.8  d10[3], \alphaChannel
	vmov.8  d10[7], \alphaChannel

	vqsub.u8	d8, d8, d10
	vqsub.u8	d9, d9, d10

	@ qx = qx - q4 
	vqsub.u8	q0, q0, q4
	vqsub.u8	q1, q1, q4
	vqsub.u8	q2, q2, q4
	vqsub.u8	q3, q3, q4
	
	.endm

@------------------------------------------------------------
@
@	EXECUTE_FUNCTION
@	Main macro. Regarding area size to be processed, it 
@	branches on corresponding block processing
@
@------------------------------------------------------------

	.macro EXECUTE_FUNCTION ProcessBlock8, ProcessBlock4, ProcessBlock2
	push	{r4-r12,lr}		@store used registers
	vpush	{q4-q7}
	ldr	srcStride, [sp, #0x68]		@ get srcStride
	ldr	dstStride, [sp, #0x6C]		@ get dstStride
	ldr	globalAlpha, [sp, #0x70]

	cmp	areaWidth,	#1
	ble	exit
	cmp	areaHeight,	#1
	ble	exit	

	mov	areaWidth_orig, areaWidth
	mov	areaHeight_orig, areaHeight
	mov 	srcBuffer_orig, srcBuffer
	mov	dstBuffer_orig, dstBuffer

	cmp	areaHeight, #8
	cmpge	areaWidth, #8
	bge	\ProcessBlock8
	cmp	areaHeight, #4
	cmpge	areaWidth, #4
	bge	\ProcessBlock4
	cmp	areaHeight, #2
	cmpge	areaWidth, #2
	bge	\ProcessBlock2
	b	exit	
	.endm

exit:
	@write length bits to 0 and stride bits to 0 as described in ARM EABI
	fmrx 	r10,FPSCR           ;
	bic  	r10,r10,#0x00370000
	fmxr 	FPSCR,r10

	vpop	{q4-q7}
	pop 	{r4-r12,pc}

@ r0 = srcBuffer
@ r1 = dstBuffer
@ r2 = areaWidth
@ r3 = areaHeight
@ lr = srcStride
@ lr+4 = dstStride
@ lr+8 = globalAlpha

@------------------------------------------------------------
@
@	neon_copy_aligned32_rotated_CW270
@	Function that rotate a RGBA buffer and apply a global 
@	alpha if needed.
@	Recursive function
@
@------------------------------------------------------------

	.global neon_copy_aligned32_rotated_CW270
        .func   neon_copy_aligned32_rotated_CW270

neon_copy_aligned32_rotated_CW270:

	EXECUTE_FUNCTION	Copy8x8_CW270_32,Copy4x4_CW270_32, Copy2x2_CW270_32
	
Copy8x8_CW270_32:
	cmp	globalAlpha, #255
	bne	Copy4x4_CW270_32
	UPDATE_BUFFER_ADDRESS_CW270_32
	LOAD_MATRIX_8x8 srcBuffer, srcStride
	SAVE_270CW_MATRIX_8x8 dstBuffer, dstStride
	UPDATE_AREA_CW270_32	#8, Copy8x8_CW270_32

Copy4x4_CW270_32:
	UPDATE_BUFFER_ADDRESS_CW270_32
	LOAD_MATRIX_4x4 srcBuffer, srcStride
	GLOBAL_ALPHA_CHANNEL_4x4 globalAlpha
	SAVE_270CW_MATRIX_4x4 dstBuffer, dstStride
	UPDATE_AREA_CW270_32	#4, Copy4x4_CW270_32
	
Copy2x2_CW270_32:
	UPDATE_BUFFER_ADDRESS_CW270_32	 
	LOAD_MATRIX_2x2 srcBuffer, srcStride
	GLOBAL_ALPHA_CHANNEL_2x2 globalAlpha
	SAVE_270CW_MATRIX_2x2 dstBuffer, dstStride
	UPDATE_AREA_CW270_32	#2, Copy2x2_CW270_32
	.endfunc 

@------------------------------------------------------------
@
@	neon_copy_aligned32
@	Function that copya RGBA buffer and apply a global 
@	alpha if needed.
@	Linear function
@
@------------------------------------------------------------

	.global neon_copy_aligned32
        .func   neon_copy_aligned32

neon_copy_aligned32:
	push	{r4-r12,lr}		@store used registers
	vpush	{q4-q7}

	vmov.i32 q8, #0xFF000000
	vmov.i32 d18, #0x00000000
	vmov.i8  d18[3], r3
	vmov.i8  d18[7], r3
	vqsub.u8	d16, d16, d18
	vqsub.u8	d17, d17, d18

	mov	r4, r2
copyLoop:
	cmp	r4, #0x80
	bge	80f
	cmp	r4, #0x40
	bge	40f
	cmp	r4, #0x20
	bge	20f
	cmp	r4, #0x10
	bge	10f
	cmp	r4, #0x8
	bge	8f
	cmp	r4, #0x4
	bge	4f
	b	exitCopy

@ load src pixels
80:	vld1.32 {d0}, [r1]!
	vld1.32 {d1}, [r1]!
	vld1.32 {d2}, [r1]!
	vld1.32 {d3}, [r1]!
	vld1.32 {d4}, [r1]!
	vld1.32 {d5}, [r1]!
	vld1.32 {d6}, [r1]!
	vld1.32 {d7}, [r1]!
40:	vld1.32 {d8}, [r1]!
	vld1.32 {d9}, [r1]!
	vld1.32 {d10}, [r1]!
	vld1.32 {d11}, [r1]!
20:	vld1.32 {d12}, [r1]!
	vld1.32 {d13}, [r1]!
10:	vld1.32 {d14}, [r1]!
8: 	vld1.32 {d15}, [r1]!
	b alphaProcess
4:	ldr r5, [r1]!
	
@ Apply global alpha
alphaProcess:
	cmp	r4, #0x80
	bge	80f
	cmp	r4, #0x40
	bge	40f
	cmp	r4, #0x20
	bge	20f
	cmp	r4, #0x10
	bge	10f
	cmp	r4, #0x8
	bge	8f
	cmp	r4, #0x4
	bge	4f

80:	vqsub.u8	q0, q0, q8
	vqsub.u8	q1, q1, q8
	vqsub.u8	q2, q2, q8	
	vqsub.u8	q3, q3, q8
40:	vqsub.u8	q4, q4, q8	
	vqsub.u8	q5, q5, q8
20:	vqsub.u8	q6, q6, q8	
10:	vqsub.u8	q7, q7, q8	
8:	vsub.u8		d15, d15, d16
	b storeBuffer
4:	
	mov	r6, r3, lsl #24
	sub	r5, r5, r6	

storeBuffer:
@write dstBuffer buffer
	cmp	r4, #0x80
	bge	80f
	cmp	r4, #0x40
	bge	40f
	cmp	r4, #0x20
	bge	20f
	cmp	r4, #0x10
	bge	10f
	cmp	r4, #0x8
	bge	8f
	cmp	r4, #0x4
	bge	4f

80:	vst1.32 {d0}, [r0]!
	vst1.32 {d1}, [r0]!
	vst1.32 {d2}, [r0]!
	vst1.32 {d3}, [r0]!
	vst1.32 {d4}, [r0]!
	vst1.32 {d5}, [r0]!
	vst1.32 {d6}, [r0]!
	vst1.32 {d7}, [r0]!
	sub r4, r4, #0x40
40:	vst1.32 {d8}, [r0]!
	vst1.32 {d9}, [r0]!
	vst1.32 {d10}, [r0]!
	vst1.32 {d11}, [r0]!
	sub r4, r4, #0x20
20:	vst1.32 {d12}, [r0]!
	vst1.32 {d13}, [r0]!
	sub r4, r4, #0x10
10:	vst1.32 {d14}, [r0]!
	sub r4, r4, #0x8
8:	vst1.32 {d15}, [r0]!
	sub r4, r4, #0x8
	b copyLoop
4:	str r5, [r0]!

exitCopy:
	@write length bits to 0 and stride bits to 0 as described in ARM EABI
	fmrx 	r10,FPSCR           ;
	bic  	r10,r10,#0x00370000
	fmxr 	FPSCR,r10

	vpop	{q4-q7}
	pop 	{r4-r12,pc}

	.endfunc 


#endif

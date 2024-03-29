#ifndef SIM
/***************************************************************************
 Copyright (c) 2009,2010, Code Aurora Forum. All rights reserved.

 Licensed under the Apache License, Version 2.0 (the "License"); you
 may not use this file except in compliance with the License.  You may
 obtain a copy of the License at

 http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 implied.  See the License for the specific language governing
 permissions and limitations under the License.
 ***************************************************************************/
        .fpu neon
        .text

        .align 4
        .global neon_memset16

neon_memset16:
        cmp             r2, #0
        bxeq            lr

        /* Keep in mind that r2 -- the count argument -- is for the
         * number of 16-bit items to copy.
         */
        lsl             r2, r2, #1

        push            {r0}

        /* If we have < 8 bytes, just do a quick loop to handle that */
        cmp             r2, #8
        bgt             memset_gt4
memset_smallcopy_loop:
        strh            r1, [r0], #2
        subs            r2, r2, #2
        bne             memset_smallcopy_loop
memset_smallcopy_done:
        pop             {r0}
        bx              lr

memset_gt4:
        /*
         * Duplicate the r1 lowest 16-bits across r1. The idea is to have
         * a register with two 16-bit-values we can copy. We do this by
         * duplicating lowest 16-bits of r1 to upper 16-bits.
         */
        orr             r1, r1, r1, lsl #16
        /*
         * If we're copying > 64 bytes, then we may want to get
         * onto a 16-byte boundary to improve speed even more.
         */
        cmp             r2, #64
        blt             memset_route
        ands            r12, r0, #0xf
        beq             memset_route
        /*
         * Determine the number of bytes to move forward to get to the 16-byte
         * boundary.  Note that this will be a multiple of 4, since we
         * already are word-aligned.
         */
        rsb             r12, r12, #16
        sub             r2, r2, r12
        lsls            r12, r12, #29
        strmi           r1, [r0], #4
        strcs           r1, [r0], #4
        strcs           r1, [r0], #4
        lsls            r12, r12, #2
        strhcs          r1, [r0], #2
memset_route:
        /*
         * Decide where to route for the maximum copy sizes.  Note that we
         * build q0 and q1 depending on if we'll need it, so that's
         * interwoven here as well.
         */
        vdup.u32        d0, r1
        cmp             r2, #16
        blt             memset_8
        vmov            d1, d0
        cmp             r2, #64
        blt             memset_16
        vmov            q1, q0
        cmp             r2, #128
        blt             memset_32
memset_128:
        mov             r12, r2, lsr #7
memset_128_loop:
        vst1.64         {q0, q1}, [r0]!
        vst1.64         {q0, q1}, [r0]!
        vst1.64         {q0, q1}, [r0]!
        vst1.64         {q0, q1}, [r0]!
        subs            r12, r12, #1
        bne             memset_128_loop
        ands            r2, r2, #0x7f
        beq             memset_end
memset_32:
        movs            r12, r2, lsr #5
        beq             memset_16
memset_32_loop:
        subs            r12, r12, #1
        vst1.64         {q0, q1}, [r0]!
        bne             memset_32_loop
        ands            r2, r2, #0x1f
        beq             memset_end
memset_16:
        movs            r12, r2, lsr #4
        beq             memset_8
memset_16_loop:
        subs            r12, r12, #1
        vst1.32         {q0}, [r0]!
        bne             memset_16_loop
        ands            r2, r2, #0xf
        beq             memset_end
        /*
         * memset_8 isn't a loop, since we try to do our loops at 16
         * bytes and above.  We should loop there, then drop down here
         * to finish the <16-byte versions.  Same for memset_4 and
         * memset_1.
         */
memset_8:
        cmp             r2, #8
        blt             memset_4
        subs            r2, r2, #8
        vst1.32         {d0}, [r0]!
memset_4:
        cmp             r2, #4
        blt             memset_2
        subs            r2, r2, #4
        str             r1, [r0], #4
memset_2:
        cmp             r2, #0
        ble             memset_end
        strh            r1, [r0], #2
memset_end:
        pop             {r0}
        bx              lr

/***************************************************************************
 Copyright (c) 2009,2010, Code Aurora Forum. All rights reserved.

 Licensed under the Apache License, Version 2.0 (the "License"); you
 may not use this file except in compliance with the License.  You may
 obtain a copy of the License at

 http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 implied.  See the License for the specific language governing
 permissions and limitations under the License.
 ***************************************************************************/

	.align 4
	.global neon_memset32

	/* r0 = buffer, r1 = value, r2 = times to write */
neon_memset32:
	cmp		r2, #1
	streq		r1, [r0], #4
	bxeq		lr

	cmp		r2, #4
	bgt		memset32_neon_start
	cmp		r2, #0
	bxeq		lr
memset32_neon_small:
	str		r1, [r0], #4
	subs		r2, r2, #1
	bne		memset32_neon_small
	bx		lr
memset32_neon_start:
	cmp		r2, #16
	blt		memset32_dropthru
	vdup.32		q0, r1
	vmov		q1, q0
	cmp		r2, #32
	blt		memset32_16
	cmp		r2, #64
	blt		memset32_32
	cmp		r2, #128
	blt		memset32_64
memset32_128:
	movs		r12, r2, lsr #7
memset32_loop128:
	subs		r12, r12, #1
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	bne		memset32_loop128
	ands		r2, r2, #0x7f
	bxeq		lr
memset32_64:
	movs		r12, r2, lsr #6
	beq		memset32_32
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	ands		r2, r2, #0x3f
	bxeq		lr
memset32_32:
	movs		r12, r2, lsr #5
	beq		memset32_16
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
	ands		r2, r2, #0x1f
	bxeq		lr
memset32_16:
	movs		r12, r2, lsr #4
	beq		memset32_dropthru
	and		r2, r2, #0xf
	vst1.64		{q0, q1}, [r0]!
	vst1.64		{q0, q1}, [r0]!
memset32_dropthru:
	rsb		r2, r2, #15
	add		pc, pc, r2, lsl #2
	nop
	str		r1, [r0, #56]
	str		r1, [r0, #52]
	str		r1, [r0, #48]
	str		r1, [r0, #44]
	str		r1, [r0, #40]
	str		r1, [r0, #36]
	str		r1, [r0, #32]
	str		r1, [r0, #28]
	str		r1, [r0, #24]
	str		r1, [r0, #20]
	str		r1, [r0, #16]
	str		r1, [r0, #12]
	str		r1, [r0, #8]
	str		r1, [r0, #4]
	str		r1, [r0, #0]
	bx		lr

	.end
		
#endif

#ifndef SIM
        .fpu neon
        .text

        .global neon_memcpy
        .func   neon_memcpy
neon_memcpy:
        push            {r4-r11}
        mov             r3, r0
1:      subs            r2, r2, #128
        pld             [r1, #64]
        pld             [r1, #256]
        pld             [r1, #320]
        ldm             r1!, {r4-r11}
        vld1.64         {d0-d3},   [r1,:128]!
        vld1.64         {d4-d7},   [r1,:128]!
        vld1.64         {d16-d19}, [r1,:128]!
        stm             r3!, {r4-r11}
        vst1.64         {d0-d3},   [r3,:128]!
        vst1.64         {d4-d7},   [r3,:128]!
        vst1.64         {d16-d19}, [r3,:128]!
        bgt             1b
        pop             {r4-r11}
        bx              lr 
        .endfunc

       .macro ple_set_chan cn
        mcr             p15, 0, \cn, c11, c2, 0
        .endm

        .macro ple_stop
        mcr             p15, 0, r0, c11, c3, 0
        .endm

        .macro ple_start
        mcr             p15, 0, r0, c11, c3, 1
        .endm

        .macro ple_clear
        mcr             p15, 0, r0, c11, c3, 2
        .endm

        .macro ple_control rd, dt, ic, ie, wy
        mov             \rd, #(\dt<<30|\ic<<29|\ie<<28)
        orr             \rd, \wy
        mcr             p15, 0, \rd, c11, c4, 0
        .endm

        .macro ple_set_addr va
        mcr             p15, 0, \va, c11, c5, 0
        .endm

        .macro ple_set_size sz
        mcr             p15, 0, \sz, c11, c7, 0
        .endm

#undef  PLE_SIZE
#define PLE_SIZE 4096

        .global neon_memcpy_ple
	.func neon_memcpy_ple
neon_memcpy_ple:
        push            {lr}
        mov             ip, #1
        ple_set_chan    ip
        ple_stop
        ple_control     r3, 0, 0, 0, ip
        mov             lr, #PLE_SIZE
        add             r3, r1, lr
        ple_set_addr    r3
        ple_set_size    lr
        ple_start
        mov             ip, #0
1:      ple_set_chan    ip
        ple_stop
        ple_control     r3, 0, 0, 0, ip
        add             r3, r1, lr, lsl #1
        ple_set_addr    r3
        ple_set_size    lr
        ple_start
2:      subs            lr, lr, #128
        pld             [r1, #320]
        vld1.64         {d0-d3},   [r1,:128]!
        vld1.64         {d4-d7},   [r1,:128]!
        vld1.64         {d16-d19}, [r1,:128]!
        vld1.64         {d20-d23}, [r1,:128]!
        vst1.64         {d0-d3},   [r0,:128]!
        vst1.64         {d4-d7},   [r0,:128]!
        vst1.64         {d16-d19}, [r0,:128]!
        vst1.64         {d20-d23}, [r0,:128]!
        bgt             2b
        subs            r2, r2, #PLE_SIZE
        eor             ip, ip, #1
        mov             lr, #PLE_SIZE
        bgt             1b
        pop             {pc}
        .endfunc

        .global neon_memset
        .func   neon_memset
neon_memset:
        push            {r4-r11}
        mov             r3,  r0
        vdup.8          q0,  r1
        vmov            q1,  q0
        orr             r4,  r1, r1, lsl #8
        orr             r4,  r4, r4, lsl #16
        mov             r5,  r4
        mov             r6,  r4
        mov             r7,  r4
        mov             r8,  r4
        mov             r9,  r4
        mov             r10, r4
        mov             r11, r4
        add             r12, r3,  r2, lsr #2
1:      subs            r2,  r2, #128
        pld             [r3, #64]
        stm             r3!, {r4-r11}
        vst1.64         {d0-d3},   [r12,:128]!
        vst1.64         {d0-d3},   [r12,:128]!
        vst1.64         {d0-d3},   [r12,:128]!
        bgt             1b
        pop             {r4-r11}
        bx              lr
        .endfunc
#endif

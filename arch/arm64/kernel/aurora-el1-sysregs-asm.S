/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>
.text
.global aurora_el1_sysregs_begin
aurora_el1_sysregs_begin:
SYM_FUNC_START(aurora_el1_sysreg_read)
    mov x4, x0
    adr x9, .Lprobe_dispatch
    ldr x9, [x9, x1, lsl #3]
    br x9
.Lprobe_case_0:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_0:
    .inst 0xd5384240
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_1:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_1:
    .inst 0x00000000
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_2:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_2:
    .inst 0xd5380000
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_3:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_3:
    .inst 0xd53800a0
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_4:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_4:
    .inst 0xd5380420
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_5:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_5:
    .inst 0xd5381000
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_6:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_6:
    .inst 0xd5382040
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_7:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_7:
    .inst 0xd5382000
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_8:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_8:
    .inst 0xd5381020
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_9:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_9:
    .inst 0xd53be000
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_10:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_10:
    .inst 0xd53be040
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_11:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_11:
    .inst 0xd53be320
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_12:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_12:
    .inst 0xd53be220
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_13:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_13:
    .inst 0xd539f1a0
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_14:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_14:
    .inst 0xd538f000
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_15:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_15:
    .inst 0xd538f020
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_16:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_16:
    .inst 0xd53df500
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
.Lprobe_case_17:
    bti j
    movz x0, #47824
    movk x0, #47824, lsl #16
    movk x0, #47824, lsl #32
    movk x0, #47824, lsl #48
    mov x1, #0
    mov x2, #0
    mov x3, #0
.Lprobe_site_17:
    .inst 0xd53ff080
    stp x0, x1, [x4]
    stp x2, x3, [x4, #16]
    ret
SYM_FUNC_END(aurora_el1_sysreg_read)
.global aurora_el1_sysregs_end
aurora_el1_sysregs_end:
.balign 8
.Lprobe_dispatch:
    .quad .Lprobe_case_0
    .quad .Lprobe_case_1
    .quad .Lprobe_case_2
    .quad .Lprobe_case_3
    .quad .Lprobe_case_4
    .quad .Lprobe_case_5
    .quad .Lprobe_case_6
    .quad .Lprobe_case_7
    .quad .Lprobe_case_8
    .quad .Lprobe_case_9
    .quad .Lprobe_case_10
    .quad .Lprobe_case_11
    .quad .Lprobe_case_12
    .quad .Lprobe_case_13
    .quad .Lprobe_case_14
    .quad .Lprobe_case_15
    .quad .Lprobe_case_16
    .quad .Lprobe_case_17
.section .rodata
.balign 8
.global aurora_el1_sysregs_sites
aurora_el1_sysregs_sites:
    .quad .Lprobe_site_0
    .quad .Lprobe_site_1
    .quad .Lprobe_site_2
    .quad .Lprobe_site_3
    .quad .Lprobe_site_4
    .quad .Lprobe_site_5
    .quad .Lprobe_site_6
    .quad .Lprobe_site_7
    .quad .Lprobe_site_8
    .quad .Lprobe_site_9
    .quad .Lprobe_site_10
    .quad .Lprobe_site_11
    .quad .Lprobe_site_12
    .quad .Lprobe_site_13
    .quad .Lprobe_site_14
    .quad .Lprobe_site_15
    .quad .Lprobe_site_16
    .quad .Lprobe_site_17

.set noat
.set noreorder
.set gp=64

.include "macros.inc"

.section .text, "ax"

glabel rsqrtf
    mtv     $f12, S000        # x -> VFPU

    vrsq.s  S000, S000        # y = 1/sqrt(x)

    mfv     $f0, S000         # return value

    jr      $ra
    nop
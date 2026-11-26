| Address error test: word access to an odd address must report the fault.
        .text
        .even
_start:
        move.w  #0x2700,%sr
        lea.l   stack_top,%sp
        move.w  0x00000501,%d0
        move.w  #0x600d,0xfff7fe
        stop    #0x2700

        .section .bss
        .even
stack:
        .ds.b   0x400
stack_top:
        .even
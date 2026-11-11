| Timer 2 compare interrupt test
        .equ IVR,     0xfff300
        .equ IMR,     0xfff304
        .equ USTCNT1, 0xfff900
        .equ UBAUD1,  0xfff902
        .equ UTX1,    0xfff906
        .equ TCTL2,   0xfff610
        .equ TPRER2,  0xfff612
        .equ TCMP2,   0xfff614
        .equ TSTAT2,  0xfff61a

        .text
        .even
_start:
        move.w  #0x2700,%sr
        lea.l   stack_top,%sp

        move.w  #0x0000,USTCNT1
        move.w  #0xe100,USTCNT1
        move.w  #0x0126,UBAUD1

        | Reset ILCR configures Timer 2 at level 3 -> vector 0x43.
        move.b  #0x40,IVR
        move.l  #timer_handler,0x10c
        move.l  #0x00ffffdf,IMR

        moveq.l #0,%d7
        move.w  #0x0004,TCTL2
        move.w  #206,TPRER2
        move.w  #100,TCMP2
        move.w  #0x0015,TCTL2

        move.w  #0x2000,%sr
wait:
        cmp.l   #3,%d7
        blt     wait

        move.w  #0x2700,%sr
        lea.l   message,%a0
        bsr     print
        move.w  #0x600d,0xfff7fe
        stop    #0x2700

timer_handler:
        addq.l  #1,%d7
        move.w  #0,TSTAT2
        rte

print:
        move.b  (%a0)+,%d0
        beq     print_done
print_wait:
        move.w  UTX1,%d1
        btst.l  #13,%d1
        beq     print_wait
        move.w  #0x0800,%d1
        or.w    %d0,%d1
        move.w  %d1,UTX1
        bra     print
print_done:
        rts

        .section .bss
        .even
stack:
        .ds.b   0x400
stack_top:
        .even

        .section .data
message:
        .asciz  "Timer 2 interrupt OK!\r\n"
        .even

| Timer 1 compare interrupt test
        .equ IVR,     0xfff300
        .equ IMR,     0xfff304
        .equ USTCNT1, 0xfff900
        .equ UBAUD1,  0xfff902
        .equ UTX1,    0xfff906
        .equ TCTL1,   0xfff600
        .equ TPRER1,  0xfff602
        .equ TCMP1,   0xfff604
        .equ TSTAT1,  0xfff60a

        .text
        .even
_start:
        move.w  #0x2700,%sr
        lea.l   stack_top,%sp

        move.w  #0x0000,USTCNT1
        move.w  #0xe100,USTCNT1
        move.w  #0x0126,UBAUD1

        move.b  #0x40,IVR
        move.l  #timer_handler,0x118
        move.l  #0x00fffffd,IMR

        moveq.l #0,%d7
        move.w  #0x0004,TCTL1
        move.w  #206,TPRER1
        move.w  #100,TCMP1
        move.w  #0x0015,TCTL1

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
        move.w  #0,TSTAT1
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
        .asciz  "Timer 1 interrupt OK!\r\n"
        .even

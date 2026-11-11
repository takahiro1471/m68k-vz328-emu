| UART2 polling output test
        .equ USTCNT2, 0xfff910
        .equ UBAUD2,  0xfff912
        .equ UTX2,    0xfff916

        .text
        .even
_start:
        move.w  #0x2700,%sr
        lea.l   stack_top,%sp

        move.w  #0x0000,USTCNT2
        move.w  #0xe100,USTCNT2
        move.w  #0x0126,UBAUD2

        lea.l   message,%a0
next:
        move.b  (%a0)+,%d0
        beq     done
wait_tx:
        move.w  UTX2,%d1
        btst.l  #13,%d1
        beq     wait_tx
        move.w  #0x0800,%d1
        or.w    %d0,%d1
        move.w  %d1,UTX2
        bra     next

done:
        move.w  #0x600d,0xfff7fe
        stop    #0x2700

        .section .bss
        .even
stack:
        .ds.b   0x400
stack_top:
        .even

        .section .data
message:
        .asciz  "Hello from UART 2!\r\n"
        .even

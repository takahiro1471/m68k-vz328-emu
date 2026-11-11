| Minimal UART1 receive interrupt test.
| The handler only reads URX (which pops the FIFO) and counts the interrupt;
| it writes no peripheral register.  This checks that an URX read clears the
| level-triggered RX interrupt as soon as the FIFO becomes empty.
        .equ IVR,     0xfff300
        .equ IMR,     0xfff304
        .equ USTCNT1, 0xfff900
        .equ UBAUD1,  0xfff902
        .equ URX1,    0xfff904
        .equ UTX1,    0xfff906

        .text
        .even
_start:
        move.w  #0x2700,%sr
        lea.l   stack_top,%sp

        move.w  #0x0000,USTCNT1
        move.w  #0xe100,USTCNT1
        move.w  #0x0126,UBAUD1

        move.b  #0x40,IVR
        move.l  #uart_handler,0x110
        move.l  #0x00fffffb,IMR

        | Drain the transmitter before enabling receive interrupts.
wait_tx_empty:
        move.w  UTX1,%d1
        btst.l  #15,%d1
        beq     wait_tx_empty
        btst.l  #10,%d1
        bne     wait_tx_empty

        moveq.l #0,%d7
        move.w  #0xe108,USTCNT1
        move.w  #0x2000,%sr
wait:
        cmp.l   #3,%d7
        bne     wait

        move.w  #0x2700,%sr
        lea.l   message,%a0
        bsr     print
        move.w  #0x600d,0xfff7fe
        stop    #0x2700

uart_handler:
        addq.l  #1,%d7
        move.w  URX1,%d0
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
        .asciz  "Minimal UART IRQ OK!\r\n"
        .even

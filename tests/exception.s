| Unhandled exception test: the default vector table points at a ROM stub
| which writes 0xDEAD to the exit port, so the emulator exits with status 2.
        .text
        .even
_start:
        move.w  #0x2700,%sr
        lea.l   stack_top,%sp
        illegal

        .section .bss
        .even
stack:
        .ds.b   0x400
stack_top:
        .even

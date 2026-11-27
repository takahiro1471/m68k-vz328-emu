| The S-record entry is 0x43C, but the code at 0x400 exits cleanly.
| Used to verify the --start-at-400 override.
        .text
        .even
        move.w  #0x600d,0xfff7fe
hold:
        bra.s   hold
        .space  0x32
        .global start
start:
        illegal
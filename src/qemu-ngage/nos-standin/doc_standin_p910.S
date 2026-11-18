@ Sony Ericsson P910i: a stand-in for the TrueFFS core inside udiskonchip.dll: hack H-p910-doc.
@
@ udiskonchip.dll is the C: media driver with M-Systems TrueFFS linked in; every TrueFFS call of the
@ media driver goes through one dispatcher, dispatch(ctx r0, func r1, IOreq* r2) at 0x500A27C4 (Thumb)
@ [V: docs/research/p910i-diskonchip.md]. The board copies this code over it (NOR image, before reset);
@ the DiskOnChip itself is not modelled. Sector requests go to the board's block device in the
@ DiskOnChip window (p910i_soc.c, physical 0xB0001800 = virtual 0x5E001800: the media driver runs in
@ EFile's thread in user mode, and 0x5E000000 is the window's user-accessible alias):
@   +00 W command (1 read, 2 write, 3 info), +04 partition, +08 sector, +0C count, +10 buffer (virtual),
@   +14 R result (0 ok), +18 R size in sectors (after info).
@ Function codes (same as the 9500's flCall): 0x32A mount, 0x336 info (word 0 = sectors, 56-byte
@ buffer), 0x332 read, 0x330 write, 0x331 delete, 0x32F defragment, 0x32D dismount, 0x2CA format,
@ 0x2C2 (after every operation); 0x32F answers 63 (nothing to do). IOreq: +0 handle (always 0 = C:), +4 flags, +12 data, +16 sector,
@ +20 count. Status 0 = ok, 20 = error.
@ Build: src/qemu-ngage/nos-standin/build.sh -> ../hw/arm/ngage_p910_doc_stub.h

        .equ    BLK,            0x5e001800

        .text
        .thumb
        .global _start
_start:
        bx      pc                              @ the dispatcher is called with BL from Thumb code
        nop
        .arm
        .global doc_dispatch
doc_dispatch:
        mov     r0, r1                          @ func
        mov     r1, r2                          @ IOreq
        stmfd   sp!, {r4-r6, lr}
        ldr     r4, =BLK
        ldr     r3, [r1]                        @ handle: only volume 0 (C:)
        cmp     r3, #0
        movne   r0, #20
        bne     9f
        mov     r5, #0                          @ partition 0
        ldr     r2, =0x336
        cmp     r0, r2
        beq     info
        ldr     r2, =0x332
        cmp     r0, r2
        moveq   r6, #1
        beq     xfer
        ldr     r2, =0x330
        cmp     r0, r2
        moveq   r6, #2
        beq     xfer
        ldr     r2, =0x32f                      @ defragment: 63, which the media driver turns into
        cmp     r0, r2                          @ -4 (0x500AFBBA): ELocal's DOCBGTH then sleeps on its
        moveq   r0, #63                         @ semaphore; 0 made it call again at once, for ever
        beq     9f
        @ mount, dismount, delete, format, 0x2C2: nothing to do
        mov     r0, #0
9:      ldmfd   sp!, {r4-r6, lr}
        bx      lr

info:   ldr     r6, [r1, #12]                   @ irData: a 56-byte buffer
        mov     r2, #0
        mov     r3, #0
1:      str     r2, [r6, r3]
        add     r3, r3, #4
        cmp     r3, #0x38
        blo     1b
        str     r5, [r4, #4]
        mov     r2, #3
        str     r2, [r4]                        @ info
        ldr     r2, [r4, #0x18]
        str     r2, [r6]                        @ word 0 = sectors
        mov     r0, #0
        b       9b

xfer:   str     r5, [r4, #4]                    @ partition
        ldr     r2, [r1, #16]
        str     r2, [r4, #8]                    @ sector
        ldr     r2, [r1, #20]
        str     r2, [r4, #12]                   @ count
        ldr     r2, [r1, #12]
        str     r2, [r4, #16]                   @ buffer
        str     r6, [r4]                        @ go
        ldr     r0, [r4, #0x14]
        cmp     r0, #0
        movne   r0, #20
        b       9b
        .ltorg

@ Nokia 9210i (RAE-5): a stand-in for the TrueFFS core inside Medata.pdd ("Media.DiskOnChip"): hack H-rae5-doc.
@
@ Medata.pdd is the C: media driver with M-Systems TrueFFS 4.x linked in (ARM code). Every TrueFFS call of
@ its wrapper object (0x5008CBEC..0x5008D6F8) goes through one dispatcher, flCall(ctx r0, func r1, IOreq* r2)
@ at 0x5008C730, which also runs the DiskOnChip init (0x5008CA84) on first use [V listing]. The board copies
@ this code over it (ROM image, before reset); the DiskOnChip itself is not modelled. Sector requests go to
@ the board's block device at physical 0x0503F000 = the kernel's peripheral window VA 0x5803F000
@ (nokia9210.c rae5_blk_*):
@   +00 W command (1 read, 2 write, 3 info), +08 sector, +0C count, +10 buffer (virtual),
@   +14 R result (0 ok), +18 R size in sectors (after info).
@ Function codes [V callers]: 4 mount, 0x12 volume info (irData word 0 = sectors; the caller's buffer is
@ 44 bytes: only word 0 is written), 5 read sectors, 6 write sectors, 8 delete sectors, 3 defragment,
@ 0x10 format, 1 dismount, 0x13 after every operation. IOreq: +0 handle (always 0 = C:), +4 flags,
@ +12 data, +16 sector, +20 count. Status 0 = ok, 20 = error (the original's answer to a bad handle).
@ Build: src/qemu-ngage/nos-standin/build.sh -> ../hw/arm/ngage_rae5_doc_stub.h

        .equ    BLK,            0x5803f000

        .text
        .arm
        .global _start
_start:                                         @ called with BL from ARM code
        stmfd   sp!, {r4-r6, lr}
        ldr     r4, =BLK
        ldr     r3, [r2]                        @ handle: only volume 0 (C:)
        cmp     r3, #0
        movne   r0, #20
        bne     9f
        cmp     r1, #0x12
        beq     info
        cmp     r1, #5
        moveq   r6, #1
        beq     xfer
        cmp     r1, #6
        moveq   r6, #2
        beq     xfer
        @ mount, dismount, delete, defragment, format, 0x13: nothing to do
        mov     r0, #0
9:      ldmfd   sp!, {r4-r6, lr}
        bx      lr

info:   mov     r3, #3
        str     r3, [r4]                        @ info
        ldr     r3, [r4, #0x18]
        ldr     r6, [r2, #12]                   @ irData
        str     r3, [r6]                        @ word 0 = sectors
        mov     r0, #0
        b       9b

xfer:   ldr     r3, [r2, #16]
        str     r3, [r4, #8]                    @ sector
        ldr     r3, [r2, #20]
        str     r3, [r4, #12]                   @ count
        ldr     r3, [r2, #12]
        str     r3, [r4, #16]                   @ buffer
        str     r6, [r4]                        @ go
        ldr     r0, [r4, #0x14]
        cmp     r0, #0
        movne   r0, #20
        b       9b
        .ltorg

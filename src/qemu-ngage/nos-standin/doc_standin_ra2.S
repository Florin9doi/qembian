@ Nokia 9500 (RA-2): a stand-in for M-Systems' TrueFFS library DiskOnChip.dll: hack H-ra2-doc.
@
@ The phone's own MedDoc.pdd (unchanged) asks DiskOnChip.dll for its partitions; the board copies this
@ code over DiskOnChip.dll's text after its entry point (0x500623D8: the entry at 0x500623D4 is one
@ branch the kernel calls on load, kept; the core runs from SDRAM) and puts a branch
@ at each of its exports. Every sector request goes to the board's block device (nokia9500.c,
@ physical 0xFFF0F000 = virtual 0x5800F000 in the kernel's peripheral mapping):
@   +00 W command (1 read, 2 write, 3 info), +04 partition, +08 sector, +0C count, +10 buffer (virtual),
@   +14 R result (0 ok), +18 R partition size in sectors.
@ The API (research agent's static reading of MedDoc.pdd, docs/devices/nokia-9500-ra2/bring-up.md):
@   ord 1 flCall(func, IOreq*): IOreq {handle (partition << 4 | socket), flags, path, data, sector,
@         count}; 0x327 count volumes (-> flags), 0x32A mount, 0x336 volume info (word 0 = sectors;
@         at most 0x3C bytes), 0x332 read, 0x330 write, 0x331 delete, 0x32F, 0x32D dismount,
@         0x2CB format, 0x2C2; status 0 ok, 20 bad handle.
@   ord 2 (unused by MedDoc) -> 1; ord 3 flExit; ord 4 flSetEnvAll(var, value) -> 0 (its thunk passes a stack slot as *prev itself);
@   ord 5 setDocBase(linear).
@ Build: src/qemu-ngage/nos-standin/build.sh -> ../hw/arm/ngage_ra2_doc_stub.h

        .equ    BLK,            0x5800f000
        .equ    NPARTS,         3

        .text
        .arm
        .global _start
_start:
        .global doc_flcall
doc_flcall:                                     @ r0 = function, r1 = IOreq
        stmfd   sp!, {r4-r6, lr}
        ldr     r4, =BLK
        ldr     r2, =0x327
        cmp     r0, r2
        moveq   r2, #NPARTS
        streq   r2, [r1, #4]                    @ count volumes -> irFlags
        moveq   r0, #0
        beq     9f
        ldr     r3, [r1]                        @ handle
        ands    r2, r3, #15                     @ socket must be 0
        movne   r0, #20
        bne     9f
        mov     r5, r3, lsr #4
        and     r5, r5, #15                     @ partition
        cmp     r5, #NPARTS
        movhs   r0, #20
        bhs     9f
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
        @ mount, dismount, delete, 0x32F, format, 0x2C2: nothing to do
        mov     r0, #0
9:      ldmfd   sp!, {r4-r6, pc}

info:   ldr     r6, [r1, #12]                   @ irData: 0x3C-byte buffer
        mov     r2, #0
        mov     r3, #0
1:      str     r2, [r6, r3]                    @ clear 0x30 bytes (never more than 0x3C)
        add     r3, r3, #4
        cmp     r3, #0x30
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

        .global doc_ord2
doc_ord2:
        mov     r0, #1
        mov     pc, lr

        .global doc_exit
doc_exit:
        mov     pc, lr

        .global doc_setenv
doc_setenv:                                     @ (var, value): the export takes two arguments [V];
                                                @ r2 is left over (0x4014 seen), never a pointer
        mov     r0, #0
        mov     pc, lr

        .global doc_setbase
doc_setbase:
        mov     pc, lr
        .ltorg

@ Nokia 9500 (RA-2): a stand-in for Bal.dll's DSP node and stream calls: hack H-ra2-bal.
@
@ No DSP is emulated. Bal.dll (the DSP/BIOS Bridge client library used by DeviceManagementServer,
@ Dasfgwc, Gwc and the codec plug-ins) creates nodes and streams on the "Hwa" channel and waits for
@ the DSP to complete them, for ever (Bal export 2 at 0x50925ECC; export 22 next). The board copies
@ this code over the bodies of exports 2, 13 and 3 (0x50925ECC.., nothing in Bal branches into that
@ range) and points those export-directory slots (0x50926BC0 + 4 * (ord - 1)) here:
@   ords 2,4,5,9,14,15,17,18,19,22-27            return 0 (node/stream operations succeed, do nothing;
@                                                 9 = load the node from Dsp_image.dof: BridgeApi's
@                                                 COFF parser looped on it for minutes)
@   ord 8                                        zero the 0x10C-byte attribute block at r1, return 0
@   ords 3, 13                                   complete the caller's TRequestStatus (r1) with 0
@   ord 11 (this, TRequestStatus& r1, r2)         a DSP message receive (Gwc's active object, 0x50929B00):
@                                                 the status is made pending and kept at this+0x2C
@                                                 (written only by the real export 2); no message comes
@   ord 10 (this)                                 its cancel: completes the kept status with KErrCancel
@                                                 (CActive::Cancel waits for that signal; bal_ok there
@                                                 hung DeviceManagementServer in Gwc 0x50929264)
@ Others stay real (the processor channel, in-process objects). docs/research/ra2-dsp-bridge.md s. 5.
@ dms_ready: DeviceManagementServer's thunk 0x50935E64 (`ldr r0, [r0, #0x60]; b 0x5093731C`, the audio
@ plug-in's "ready" to the server's object at +0x60) branches here: without a DSP the plug-in reports
@ ready synchronously while the server's ConstructL is still building it (+0x5C before +0x60, 0x50935DC8),
@ so +0x60 is NULL (data abort at 0x50937328, DMS restarted for ever, then a kernel fault in Ehwaomap).
@ A NULL owner drops the notification.
@ Build: src/qemu-ngage/nos-standin/build.sh -> ../hw/arm/ngage_ra2_bal_stub.h

        .equ    RQCOMPLETE_SLOT, 0x500effac     @ EUser ord 926 User::RequestComplete(TRequestStatus*&, TInt)

        .text
        .arm
        .global _start
_start:
        .global bal_ok
bal_ok: mov     r0, #0
        bx      lr

        .global bal_attr                        @ ord 8 (this, 0x10c-byte out)
bal_attr:
        mov     r2, #0
        mov     r3, #0
1:      str     r2, [r1, r3]
        add     r3, r3, #4
        cmp     r3, #0x10c
        blo     1b
        mov     r0, #0
        bx      lr

        .global bal_async                       @ ords 3, 13 (this, TRequestStatus& r1, ...)
bal_async:
        stmfd   sp!, {r1, lr}                   @ [sp] = the TRequestStatus* variable
        mov     r0, sp
        mov     r1, #0                          @ KErrNone
        ldr     ip, =RQCOMPLETE_SLOT
        ldr     ip, [ip]
        mov     lr, pc
        bx      ip
        ldmfd   sp!, {r1, lr}
        mov     r0, #0
        bx      lr

        .global bal_msg_get                     @ ord 11 (this, TRequestStatus* r1, r2)
bal_msg_get:
        str     r1, [r0, #0x2c]
        ldr     r2, =0x80000001                 @ KRequestPending
        str     r2, [r1]
        mov     r0, #0
        bx      lr

        .global bal_msg_cancel                  @ ord 10 (this)
bal_msg_cancel:
        ldr     r1, [r0, #0x2c]
        cmp     r1, #0
        beq     1f
        ldr     r2, [r1]
        ldr     r3, =0x80000001
        cmp     r2, r3
        bne     1f
        mov     r2, #0
        str     r2, [r0, #0x2c]
        stmfd   sp!, {r1, lr}                   @ [sp] = the TRequestStatus* variable
        mov     r0, sp
        mvn     r1, #2                          @ KErrCancel (-3)
        ldr     ip, =RQCOMPLETE_SLOT
        ldr     ip, [ip]
        mov     lr, pc
        bx      ip
        ldmfd   sp!, {r1, lr}
1:      mov     r0, #0
        bx      lr

        .global dms_ready                       @ r0 = [server + 0x60] (DMS 0x50935E64)
dms_ready:
        cmp     r0, #0
        bxeq    lr                              @ the server object is not built yet: drop it
        ldr     ip, =0x5093731c                 @ the real "ready" handler
        bx      ip
        .ltorg

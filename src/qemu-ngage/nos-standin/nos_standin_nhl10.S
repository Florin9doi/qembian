@ NOS stand-in stub for the Nokia 6600 (NHL-10 V 05.53) on the nokia6600 board: H-nos-standin.
@
@ As on the N-Gage (nos_standin.S): the NOS code is scrambled in the flash file, so the board
@ copies this stub over the NOS entry (virtual 0x52882024 = flash 0x82024; nosstarter.ext
@ calls it with r0 = -1 after copying the NOS data to 0x80002210 and zeroing its bss
@ 0x80006D44 + 0xC0358). docs/devices/nokia-6600-nhl10/bring-up.md.
@
@ Device (physical 0x0C1F0000, virtual 0x581F0000): +00 call (slot << 8 | method),
@ +04..+10 r0..r3, +14..+20 stack arguments, +28 result, +2C a TDfc to queue.
@
@ The interfaces are added as the boot asks for them; each names its evidence in
@ docs/devices/nokia-6600-nhl10/bring-up.md. ekern addresses were ported from the N-Gage's by
@ code matching (eka1/tools/port_addr.py; unique full-score hits) [I until a run confirms].
@ The Nokia 3230 (RM-51, -M nokia3230) uses this stub too: build.sh assembles a second copy with its
@ addresses as --defsym (the same ekern export ordinals, its NOS bss, enosdriver export 2).

        .equ    DEV,            0x581f0000
        .ifndef KERN_VARIANT
        .equ    KERN_VARIANT,   0x50322260      @ export 426 (N-Gage 469): Kern::Variant()
        .endif
        .ifndef SET_POWER_MODEL
        .equ    SET_POWER_MODEL,0x5032b33c      @ export 344 (N-Gage 394): Power::SetPowerModel()
        .endif
        .ifndef POWER_MODEL_CTOR
        .equ    POWER_MODEL_CTOR,0x5032aff8     @ export 451 (N-Gage 493): DPowerModel::DPowerModel()
        .endif
        .ifndef KERN_ADD_DFC
        .equ    KERN_ADD_DFC,   0x503361d8      @ export 7: Kern::Add(TDfc&)
        .endif
        .ifndef RAM
        .equ    RAM,            0x80006d44      @ the NOS's bss (zeroed by nosstarter): free
        .endif
        .equ    PM_OBJ,         RAM + 0x100     @ the power model object
        .equ    HW_OBJ,         RAM + 0x300     @ DPlatChunkHw (0x2C bytes), H-nhl10-hwchunk
        .ifndef HWCHUNK_CTOR
        .equ    HWCHUNK_CTOR,   0x5031d374      @ export 454: DPlatChunkHw::DPlatChunkHw() (ecust's import)
        .endif
        .ifndef HWCHUNK_CREATE
        .equ    HWCHUNK_CREATE, 0x5031d3ec      @ export 93: DPlatChunkHw create(this, phys, size, attribs)
        .endif
        .equ    NMETHODS,       64
        .ifndef HWCHUNK_SIZE
        .equ    HWCHUNK_SIZE,   0x46000         @ H-nhl10-hwchunk: address space the NOS's chunks took [I]
        .endif
        .equ    RTC_DONE,       RAM + 0x1f0     @ the RTC hand-over was made (N-Gage Q49)
        .equ    RTC_DFC,        RAM + 0x600     @ H-uem-rtc: the hand-over TDfc {0, 0, callback, argument}
        .equ    RX_FLAG,        RAM + 0x1f8     @ board: a delivery waits (ngage.c nos_rx_flag)
        .equ    RX_DFC,         RAM + 0x200     @ the delivery TDfc (ngage.c nos_rx_dfc)
        .equ    RX_RING_IDX,    RAM + 0x2f0     @ next ring slot
        .equ    RX_RING,        RAM + 0x1000    @ 32 frame buffers of 0x220 bytes (kernel memory)
        .equ    RX_SLOT,        0x220
        .ifndef ENOS_DELIVER
        .equ    ENOS_DELIVER,   0x5040f164      @ enosdriver.ldd export 2: frame up to isa_if_driver [V]
        .endif

        .text
        .arm
        .global _start
_start:
        stmfd   sp!, {r4, lr}
        ldr     r4, =DEV
        mov     r0, #0xff00                     @ event: stand-in entered
        str     r0, [r4]

        @ the delivery DFC: TDfc = {0, 0, callback, argument} (Kern::Add's layout, as N-Gage)
        ldr     r0, =RX_DFC
        ldr     r1, =rx_dfc_fn
        str     r1, [r0, #8]
        mov     r1, #0
        str     r1, [r0, #12]

        @ power model (as N-Gage Q22): ekeyb's AddPowerHandler enqueued into a NULL model's
        @ queue (+16) and faulted (run nhl10 1). Object in RAM, built like a derived class:
        @ base constructor, then our vtable.
        ldr     r0, =PM_OBJ
        ldr     ip, =POWER_MODEL_CTOR
        mov     lr, pc
        bx      ip
        ldr     r0, =PM_OBJ
        ldr     r1, =vtbl64
        str     r1, [r0]
        ldr     ip, =SET_POWER_MODEL
        mov     lr, pc
        bx      ip                              @ Power::SetPowerModel(object)
        ldr     r1, =0xfe40
        mov     r0, #1
        str     r0, [r4, #4]
        str     r1, [r4]                        @ event: registered

        @ H-nhl10-hwchunk: ecust (0x5033b210) maps the screen (physical 0x0A097000) as a hardware
        @ chunk and faults 13 unless its address is 0x5A152000. Hardware chunks are placed one
        @ after the other from 0x5A000000; ecust's own (SDRAM 0xF000 + 0x97000, four flash windows
        @ of 0x19000) and EPBUSM's (0x2000) end at 0x5A10C000, 0x46000 short. The missing chunks
        @ are the NOS's own [I]; reserve the same amount of address space here. Physical address and
        @ attributes are a placeholder (never accessed): the UPP block 0x0C000000, ecust's
        @ device attributes 0x13 [I].
        ldr     r0, =HW_OBJ
        ldr     ip, =HWCHUNK_CTOR
        mov     lr, pc
        bx      ip
        ldr     r0, =HW_OBJ
        mov     r1, #0x0c000000
        ldr     r2, =HWCHUNK_SIZE
        mov     r3, #0x13
        ldr     ip, =HWCHUNK_CREATE
        mov     lr, pc
        bx      ip
        ldr     r0, =HW_OBJ
        str     r0, [r4, #4]
        ldr     r1, =0xfa00
        str     r1, [r4]                        @ event: hardware chunk reserved

        @ variant slots 3 (product data, N-Gage Q21: ecust faults 11 without it, then reads
        @ item 5 asynchronously), 6 and 7 (N-Gage Q45: medint's NOS services) [3: V, 6/7: I].
        @ Register(slot, object) is vtable +0x104 here (slots at +0x1C, 0..7), Get +0x108 [V]
        @ (N-Gage +0xFC).
        .irp    slot, 3, 6, 7
        ldr     ip, =KERN_VARIANT
        mov     lr, pc
        bx      ip
        ldr     r3, [r0]
        mov     r1, #\slot
        ldr     r2, =obj\slot
        mov     lr, pc
        ldr     pc, [r3, #0x104]
        str     r0, [r4, #4]
        ldr     r1, =(0xfe00 | \slot)
        str     r1, [r4]                        @ event: registered (r0 = 1 if accepted)
        .endr

        @ NOS status word (as N-Gage Q24): ecust's start-up (0x50339a38) polls the variant's
        @ getter (vtable +0x134, word +0x94) until bit 1 is set [V]; a running NOS sets bits 0
        @ (services up) and 1. Setter: vtable +0x130 (str r1, [r0, #0x94]) [V].
        ldr     ip, =KERN_VARIANT
        mov     lr, pc
        bx      ip
        ldr     r3, [r0]
        mov     r1, #3
        mov     lr, pc
        ldr     pc, [r3, #0x130]
        mov     r0, #3
        str     r0, [r4, #4]
        ldr     r1, =0xfd00
        str     r1, [r4]                        @ event: status set

        mov     r0, #0
        ldmfd   sp!, {r4, pc}
        .ltorg

@ ---- the NOS's exports to enosdriver.ldd ---------------------------------------------
@ An unnamed NOS module (image header 0x504128B0, data 0x80002210, 5 exports at 0x50412968,
@ not in the file tree) is in enosdriver.ldd's DLL references. enosdriver calls exports 1, 2
@ and 4 [V]; the board puts a jump at each NOS address to these (N-Gage analogue: Q33).
@ Export 4 (0x5288877C): enosdriver 0x5040F0D4 news a 4-byte object and calls this on it,
@ keeping it at 0x80002184: a NOS object that is only a vtable pointer [V], so it becomes a
@ trap object (slot 71). Exports 1 (0x528887E8) and 2 (0x528887B8) are plain calls with that
@ object in r0 (slot 70, methods 1 and 2).
        .global nos_export4
nos_export4:
        ldr     r1, =vtbl71
        str     r1, [r0]
        stmfd   sp!, {r0, r4, lr}
        ldr     r4, =DEV
        str     r0, [r4, #0x04]
        ldr     r1, =0xf900
        str     r1, [r4]                        @ event: NOS object constructed
        ldmfd   sp!, {r0, r4, pc}

        .global nos_export1
nos_export1:
        mov     ip, #1
        b       slot70

        .global nos_export2
nos_export2:
        mov     ip, #2
        b       slot70
        .ltorg

@ ---- frames up (the NOS's side of ISA_IF) ----------------------------------------------
@ The board queues this DFC (device +0x2C) when a frame waits. For each one: fetch (+0x30 = 1;
@ +0x34 != 0: one is there, +0x40 its length, the bytes at +0x100), copy it into the next of 32
@ static buffers in the NOS's memory (isa_if_driver queues the pointer and reads it later, from
@ other threads), hand it to enosdriver's export 2, report the result (+0x44, +0x30 = 2).
@ A heap cell from EUser `new` here came from whatever thread's heap was current: another
@ process later read the queued pointer as garbage (SelfTestServer fault in
@ ISA_IF_PROTOCOL_BASE 0x50411670; regression "isa_frame_heap").
rx_dfc_fn:
        stmfd   sp!, {r4-r8, lr}
        ldr     r4, =DEV
1:      mov     r0, #1
        str     r0, [r4, #0x30]
        ldr     r5, [r4, #0x34]
        cmp     r5, #0
        beq     9f
        ldr     r8, [r4, #0x40]                 @ the frame length
        ldr     r0, =RX_RING_IDX
        ldr     r1, [r0]
        add     r2, r1, #1
        and     r2, r2, #31
        str     r2, [r0]
        ldr     r6, =RX_RING
        mov     r0, #RX_SLOT
        mla     r6, r1, r0, r6                  @ this frame's buffer
        add     r1, r4, #0x100
        mov     r2, #0
2:      cmp     r2, r8
        bge     3f
        ldr     r3, [r1, r2]
        str     r3, [r6, r2]
        add     r2, r2, #4
        b       2b
3:      mov     r0, r6
        ldr     ip, =ENOS_DELIVER
        mov     lr, pc
        bx      ip
8:      str     r0, [r4, #0x44]
        mov     r0, #2
        str     r0, [r4, #0x30]
        b       1b
9:      mov     r0, #0
        ldmfd   sp!, {r4-r8, pc}
        .ltorg

@ ---- trap objects (as nos_standin.S): every virtual method -> the board's NOS device ----
        .macro  trapobj slot
        .align  2
obj\slot:
        .word   vtbl\slot
vtbl\slot:
        .set    k, 0
        .rept   NMETHODS
        .word   tramp\slot + 8 * k
        .set    k, k + 1
        .endr
tramp\slot:
        .set    k, 0
        .rept   NMETHODS
        mov     ip, #k
        b       slot\slot
        .set    k, k + 1
        .endr
slot\slot:
        .if     \slot == 3
        @ H-ngage-idle (as nos_standin.S): the null thread's idle hook (method 18, r1 = 13,
        @ r3 = 0) sleeps until the next interrupt instead of a device call per pass, unless the
        @ board has flagged a delivery (RX_FLAG).
        cmp     ip, #18
        bne     2f
        cmp     r1, #13
        cmpeq   r3, #0
        bne     2f
        stmfd   sp!, {r0}
        ldr     r0, =RTC_DONE
        ldr     r0, [r0]
        cmp     r0, #0
        beq     3f                              @ hand-over pending: take the device path
        ldr     r0, =RX_FLAG
        ldr     r0, [r0]
        cmp     r0, #0
        bne     3f
        mcr     p15, 0, r0, c15, c8, 0          @ TI925T wait-for-interrupt
        ldmfd   sp!, {r0}
        mov     r0, #0
        mov     pc, lr
        .ltorg
3:      ldmfd   sp!, {r0}
2:
        .endif
        orr     ip, ip, #(\slot << 8)
        b       common
        .endm

        .ltorg
        trapobj 3               @ variant slot 3: product data
        trapobj 6
        trapobj 7
        trapobj 64              @ power model
        trapobj 70              @ plain NOS exports (1, 2)
        trapobj 71              @ the NOS object enosdriver keeps at 0x80002184

@ ---- RTC hand-over DFC (H-uem-rtc; queued once from common) ------------------------------
rtc_dfc_fn:
        stmfd   sp!, {r4, lr}
        ldr     r0, =RTC_DONE
        ldr     r0, [r0]                        @ the RTC object
        @ H-uem-rtc: A, B, C = the date the NOS keeps for the RTC, from the board (device +48/+4C, a TTime;
        @ copied to the stack). 0 there = none: the RTC's own current time (+0x10), as before.
        add     r2, r0, #0x10
        sub     sp, sp, #16
        ldr     r4, =DEV
        ldr     r1, [r4, #0x48]
        ldr     r3, [r4, #0x4c]
        orrs    ip, r1, r3
        addne   r2, sp, #8
        strne   r1, [r2]
        strne   r3, [r2, #4]
        mov     r3, r2
        str     r2, [sp]
        mov     r1, #1
        ldr     ip, [r0]
        mov     lr, pc
        ldr     pc, [ip, #0x20]
        add     sp, sp, #16
        ldr     r4, =DEV
        str     r0, [r4, #4]
        mov     r0, #0xfb00
        str     r0, [r4]                        @ event: RTC handed over
        ldmfd   sp!, {r4, pc}
        .ltorg

common:
        @ RTC hand-over (as nos_standin.S, N-Gage Q49): once rtc.dll has registered (variant
        @ slot 1, [variant + 0x20] here; slots start at +0x1C), call its request 1 (vtable +0x20)
        @ with three pointers to its own current time (+0x10). Without it [rtc+0x120] stays NULL
        @ and DosServer's first alarm request faults in rtc.dll 0x50340960 [V run nhl10].
        stmfd   sp!, {r0-r4, ip, lr}
        ldr     r4, =RTC_DONE
        ldr     r0, [r4]
        cmp     r0, #0
        bne     2f
        ldr     ip, =KERN_VARIANT
        mov     lr, pc
        bx      ip
        ldr     r0, [r0, #0x20]                 @ the RTC object
        cmp     r0, #0
        beq     2f
        str     r0, [r4]                        @ once
        @ H-uem-rtc: the hand-over runs as a DFC, as in the NOS's own kernel context. Called here, at thread level,
        @ the tick DFC ran inside rtc.dll's SetUniversalTime (it waits for a tick between setting the time and moving
        @ the kernel's second counter, 0x5030AD10..AD1C on the 7650) and stepped that counter one second at a time
        @ from 2000 to the new date: minutes of boot for a date years away [V run nhl2]. As a DFC it runs to the end
        @ first; the tick ISR still sets the flag SetUniversalTime waits for (7650 0x50309530) [V].
        ldr     r0, =RTC_DFC
        ldr     r1, =rtc_dfc_fn
        str     r1, [r0, #8]
        mov     r1, #0
        str     r1, [r0, #12]
        ldr     ip, =KERN_ADD_DFC
        mov     lr, pc
        bx      ip                              @ Kern::Add(dfc)
2:      ldmfd   sp!, {r0-r4, ip, lr}

        @ atomic exchange with the device (IRQ/FIQ off), as nos_standin.S
        stmfd   sp!, {r4, r5, lr}
        mrs     r5, cpsr
        orr     r4, r5, #0xc0
        msr     cpsr_c, r4
        ldr     r4, =DEV
        str     r0, [r4, #0x04]
        str     r1, [r4, #0x08]
        str     r2, [r4, #0x0c]
        str     r3, [r4, #0x10]
        ldr     r0, [sp, #12]
        str     r0, [r4, #0x14]
        ldr     r0, [sp, #16]
        str     r0, [r4, #0x18]
        ldr     r0, [sp, #20]
        str     r0, [r4, #0x1c]
        ldr     r0, [sp, #24]
        str     r0, [r4, #0x20]
        str     ip, [r4]                        @ the call
        ldr     r0, [r4, #0x2c]                 @ a DFC to queue?
        ldr     r4, [r4, #0x28]                 @ the result
        msr     cpsr_c, r5
        cmp     r0, #0
        beq     1f
        ldr     ip, =KERN_ADD_DFC
        mov     lr, pc
        bx      ip
1:      mov     r0, r4
        ldmfd   sp!, {r4, r5, pc}
        .ltorg

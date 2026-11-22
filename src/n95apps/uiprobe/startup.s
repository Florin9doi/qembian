@ EKA2 DLL entry point (as kernel/eka/euser/epoc/arm/uc_dll.cia): the version marker, a jump to the
@ body, the word the loader fills with the code segment's unique ID (entry + 12). uiprobe has no
@ static constructors and no writable static data: the body returns KErrNone.
	.text
	.arm
	.align 2
	.global _E32Dll
	.type _E32Dll, %function
_E32Dll:
	tst	pc, #0			@ EKA2 entry point version identifier
	ldr	r12, 1f
	bx	r12
	.word	0			@ code seg unique ID (loader fills in)
1:	.word	uiprobe_dll_body
	.global uiprobe_dll_body
	.type uiprobe_dll_body, %function
uiprobe_dll_body:
	mov	r0, #0
	bx	lr

@ Fault recovery for the probe (uiprobe.cpp FaultHandler): a setjmp that keeps r4-r11, sp, lr, and the
@ place the exception handler sends a faulting thread to (it puts the buffer in the saved r0 and this
@ address in the saved pc of the kernel's exception frame; EUSER's exception entry restores r0-r15).
	.global uiprobe_setjmp
	.type uiprobe_setjmp, %function
uiprobe_setjmp:
	stmia	r0, {r4-r11, sp, lr}
	mov	r0, #0
	bx	lr
	.global uiprobe_fault_land
	.type uiprobe_fault_land, %function
uiprobe_fault_land:
	ldmia	r0, {r4-r11, sp, lr}
	mov	r0, #1
	bx	lr

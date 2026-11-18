@ EKA2 DLL entry point (as src/khr/startup.s): version marker, jump, the code seg ID word at entry + 12
	.text
	.arm
	.align 2
	.global _E32Dll
	.type _E32Dll, %function
_E32Dll:
	tst	pc, #0
	ldr	r12, 1f
	bx	r12
	.word	0
1:	.word	pvr2dif_dll_body

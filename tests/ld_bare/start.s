# Vstup obrazu bare metal pro tests/ld_bare.py: .text.start na začátek RAM,
# odkazy na symboly rozložení (relativně k PC i absolutní v datech), data
# a .bss se zarovnáním na stránku jako vrstva architektury runtime LSL.
	.section .text.start,"ax",@progbits
	.globl _bare_start
_bare_start:
	la sp, lsl_stack_top
	la t0, __bss_start
	la t1, __bss_end
	call _start
1:	j 1b

	.data
	.p2align 3
layout:
	.dword lsl_text_end, lsl_rodata_end, __bss_start, __bss_end
	.dword lsl_trap_stacks, lsl_stack_guard, lsl_stack_top, lsl_image_end

	.section .rodata.layout,"a",@progbits
	.p2align 4
	.ascii "luxir bare metal"

	.bss
	.p2align 12
pages:
	.zero 12345

	.section .eh_frame,"a",@progbits
	.word 0

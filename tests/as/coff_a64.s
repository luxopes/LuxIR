// COFF arm64 (vzor LLVM): relokace a addendy v instrukcích, zarovnání,
// pořadí a tabulka řetězců (sdílené přípony).
	.file "a_long_file_name_for_coff.s"
	.text
	adrp x0, ext+8
	add x0, x0, #:lo12:ext+8
	ldr x1, [x0, #:lo12:ext+16]
	ldr w1, [x0, #:lo12:ext+16]
	ldrb w1, [x0, #:lo12:ext+3]
	ldr q1, [x0, #:lo12:ext+32]
	adrp x0, loc+8
	add x0, x0, #:lo12:loc+8
	adrp x0, .Ltmp+8
	add x0, x0, #:lo12:.Ltmp+8
	ldr x2, [x0, #:lo12:.Ltmp+8]
	adrp x0, gl
	bl ext
	b ext
	b.eq ext
	cbz x0, ext
	tbz x0, #3, ext
	bl gl
	bl other
	.globl gl
gl:	ret
	.section ".text$abc","xr"
other: ret
.Lo2: ret
	.section .rodata
	.p2align 3
.Ltmp:	.quad 1
	.quad 2
	.xword ext
	.xword loc
	.xword gl
	.xword .Ltmp+8
	.word ext
	.data
loc:	.quad 0
	bl zeta_function_long
	bl alpha_function_long
	bl function_long
	bl beta_function_longer
	bl mid_function_long
	.section ".text$aaa_long_section","xr"
	ret
	.section ".text$zzz_long_section","xr"
	ret
	.section ".text$long_section","xr"
	ret

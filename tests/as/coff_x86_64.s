# COFF (GNU as pro pe-x86-64): příznaky a výchozí zarovnání sekcí podle
# jména, relokace přes symboly sekcí, místní jména L…, výplň sekcí.
	.file "coff_x86_64_with_a_long_name.s"
	.text
	cmpl $5, ext(%rip)
	movb $1, loc(%rip)
	movq $7, loc+8(%rip)
	call ext
	call gl
	jmp loc2
	leaq .Lstr(%rip), %rax
	movabsq $ext, %rax
	.globl gl
gl:	ret
loc2:	ret
	.section .rodata
.Lstr:	.ascii "abc"
	.p2align 3
rod:	.quad ext
	.quad gl
	.quad loc
	.long ext
	.section ".text$abc","xr"
	.p2align 5
f1:	ret
	.section ".text$averyveryverylongname","xr"
	.globl f2
f2:	call f1
	.data
	.p2align 2
loc:	.long 1
	.bss
bb:	.zero 3
	.text
	call h2
	leaq h2(%rip), %rax
	leaq h3(%rip), %rax
	jmp h2
	.globl h1
h1:	ret
	.section ".text$x","xr"
	.globl h2
h2:	call h1
	jmp h1
	ret
	.section .rdata,"dr"
	.byte 1
	.globl h3
h3:	.quad h1
	.quad h2
	.quad h3
	.section .foo
	.byte 2
	.section .bar,"x"
	.byte 3
	.section .xdata,"r"
	.byte 4
	.section .baz,"w"
	.byte 5
	.section .qq,"bw"
	.zero 3
	.section .textq,"xr"
	.byte 1
	.section .text$q,"xr"
	.byte 1
	.section .rdata$q,"dr"
	.byte 1
	.section .datax,"dw"
	.byte 1
	.section .data$q,"dw"
	.byte 1
	.section .rdataq,"dr"
	.byte 1
	.section .bss$q,"bw"
	.zero 1
	.section .rodata
	.byte 1
	.section .rodata$x
	.byte 1
	.section .text$q2
	.byte 1
	.section .pdata,"dr"
	.byte 1
	.section .tls$,"dw"
	.byte 1
	.text
Llocal:	jmp Llocal
	leaq Llocal(%rip), %rax

# Mach-O x86-64 (vzor LLVM): atomy, dočasná návěští, SIGNED_1/2/4,
# relokace na sekci literálů, xchg a nop podle LLVM.
	.text
	.globl _f
_f:
	call _f
	jmp _f
	jmp Lin
Lin:	call Lother
	leaq Lstr(%rip), %rax
	leaq Lin(%rip), %rax
	leaq _g(%rip), %rax
	cmpl $5, _ext(%rip)
	cmpl $500, _ext+8(%rip)
	movq _loc+8(%rip), %rax
	movq Ldat+16(%rip), %rax
	call _ext
	jmp _ext
	je _ext
	je Lother
	ret
_g:	ret
Lother:	ret
	.section __TEXT,__cstring,cstring_literals
Lstr:	.asciz "hello"
	.data
	.p2align 3
_loc:	.quad 1
Ldat:	.quad 2
	.quad _f
	.quad Lin
	.quad Ldat+8
	.quad _ext+4
	.long _ext
	.text
	.globl _f2
_f2:
	leaq _f2(%rip), %rax
	movq _f2+8(%rip), %rax
	call _f2
	jmp _f2
	je _f2
	.fill 200, 1, 0x90
	jmp _f2
	je _f2
	call Lx2
	leaq Lx2(%rip), %rax
Lx2:	ret
_h2:	call _f2
	call _h2
	jmp _h2
	ret
	xchgq %rsi, %rdi
	xchgq %rax, %rdi
	xchgl %ecx, %edx
	.p2align 5
	ret
	.p2align 4
	.subsections_via_symbols

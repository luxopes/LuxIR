# Relokace, symboly, číselná návěští, data s odkazy a výplně (x86-64)
.text
.globl f
f:
	callq g
	callq h
	jmp g
	jz g
	jmp .L1
	movq ".Lfp0"(%rip), %rax
	leaq data+8(%rip), %rcx
	movq data2(%rip), %rcx
	movl $5, %eax
	movq $g, %rax
	movq $f, %rax
	movl $f, %eax
	leaq f(%rip), %rax
	jmp 1f
1:
	nop
	jmp 1b
	callq f
	jmp f
.L1:
	ret
h:
	ret
.p2align 4
k:
	nop
.p2align 4
m:
	ret
.weak w
.type f, @function
.size f, .-f
.data
data:
	.quad h
	.quad f+4
	.quad .L1
	.quad w
	.int g
data2:
	.int 1
	.short 2, 3
	.byte 4
	.ascii "ab\n\0c"
	.asciz "xy"
	.zero 5
	.fill 3,2,1
.section .rodata
.balign 16
".Lfp0":
	.quad 1
.bss
.balign 8
b1:
	.zero 24
.comm cm, 16, 8
.section .note.GNU-stack,"",@progbits

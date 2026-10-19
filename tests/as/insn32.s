# i386: cdecl, moffs, cmpxchg8b, x87 přenos výsledku
.text
.globl f
f:
	pushl %ebp
	movl %esp, %ebp
	calll g
	calll f
	jmp g
	movl $g, %eax
	movl g, %eax
	movl %eax, g
	movl %gs:8, %eax
	movl %eax, %gs:4
	movl %gs:(%ecx), %eax
	movl 4(%esp), %eax
	movl 8(%ebp), %ecx
	movl (%eax, %esi, 1), %edi
	movl %edx, 4(%ecx, %edx, 1)
	lock cmpxchg8b (%esi)
	movw %ax, %gs
	mull %ecx
	imull $97, %eax, %ecx
	shll $3, %esi
	sarl $31, %eax
	addl $-2147483648, %edi
	setb %dl
	movzbl %dl, %edx
	movsd %xmm0, -8(%ebp)
	fldl -8(%ebp)
	fstpl -8(%ebp)
	flds -8(%ebp)
	fstps -8(%ebp)
	movd %xmm0, %eax
	pshufd $1, %xmm0, %xmm7
	psrlq $32, %xmm0
	punpckldq %xmm7, %xmm1
	cvtsi2sd %eax, %xmm0
	int $0x80
	leave
	ret
g:
	ret
.data
	.int f, g+4
	.int 0
# úzké immediate: 32bitová hodnota se znaménkově rozšíří, jinak zkrátí (varování)
	movw $4294967295, 112(%eax)
	addw $0xffffffff, %ax
	cmpb $4294967295, %al
	andw $0xffff8000, %ax
	movb $-129, %al
	imulw $0xffffffff, %ax, %ax
	subw $0x7fffffff, %ax

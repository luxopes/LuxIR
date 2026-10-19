# Volba kódování jako GNU as (x86-64)
.text
	xchgq %rax, %rdx
	xchgq %rdx, %rax
	xchgq %rbx, %rcx
	xchgl %eax, %ecx
	testq %rax, %rcx
	addq $5, %rax
	addq $500, %rax
	addq $500, %rcx
	cmpl $500, %eax
	cmpb $1, %al
	cmpb $1, %cl
	movq $0xffffffff, %rax
	movq $-1, %rax
	movq $0x7fffffff, %rax
	imulq $5, %rax
	imulq $500, %rax, %rcx
	shlq $1, %rax
	shlq $2, %rax
	shrq %cl, %rax
	pushq $5
	pushq $500
	pushq %r12
	popq %r12
	movsd %xmm1, %xmm2
	movq %xmm1, %xmm2
	cvtsi2sd %rax, %xmm0
	cvtsi2sd %eax, %xmm0
	cvtsi2sdq %rax, %xmm0
	cvttsd2siq %xmm0, %rax
	cvttsd2si %xmm0, %eax
	movq %xmm0, %rax
	movq %rax, %xmm0
	movd %eax, %xmm0
	testl $5, %eax
	testl $5, %ecx
	movb $1, %sil
	movzbl %sil, %eax
	movzbq (%rax, %rdx, 8), %rcx
	movzwq 2(%rax), %rcx
	movslq %eax, %rcx
	movq %fs:(%rdi), %rax
	movq %fs:0, %rax
	movw %ax, %fs:(%rdi)
	lock xadd %rax, (%rdi)
	lock cmpxchg %rdx, (%rdi)
	xchg %rax, (%rdi)
	sqrtsd 4(%rsp), %xmm0
	ucomisd %xmm1, %xmm0
	xorpd %xmm1, %xmm1
	movabsq $0x123456789, %rax
	subq $8, %rsp
	andq $-16, %rsp
	incq %rax
	notq %rax
	negq %rax
	idivq %rcx
	divq (%rax)
	cqto
	cltd
	setl %al
	setnp %cl
	cmovnz %rcx, %rax
	cmovge %rcx, %rax
	leaq 0x7fff0000(%rax,%rcx,4), %rdx
	movq 0x10(%rsp), %rax
	movq 0x10(%r12), %rax
	movq (%r13), %rax
	movq (%rsp), %rax
	movq 8(%rbp), %rax
	movl %eax, 0(%rbp)
	movq %rax, -8(%rbp)
	syscall
	endbr64
	pause
	ud2
	leave
	ret
# úzké immediate: 32bitová hodnota se znaménkově rozšíří, jinak zkrátí (varování)
	movw $4294967295, 112(%rax)
	addw $0xffffffff, %ax
	addw $0xffffffff, 2(%rax)
	cmpb $4294967295, %al
	andw $0xffff8000, %ax
	movb $-129, %al
	imulw $0xffffffff, %ax, %ax
	subw $0x7fffffff, %ax
	movw $65536, %ax

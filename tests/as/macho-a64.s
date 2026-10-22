// Mach-O arm64 (vzor LLVM): insn_a64.s v syntaxi Apple (@PAGE, @PAGEOFF).
// AArch64: aliasy, immediate, adresování, relokace jako GNU as
.text
.globl f
f:
	hint #34
	adrp x0, sym@PAGE
	add x0, x0, sym@PAGEOFF
	adrp x1, sym@PAGE+5904
	add x1, x1, sym@PAGEOFF+5904
	ldr x2, [x1, sym@PAGEOFF]
	bl g
	bl f
	b g
	b f
	b.eq g
	beq f
	cbnz x0, f
	ret
	mov x0, #0
	mov x0, #1
	mov x0, #-1
	mov x0, #0xffff
	mov x0, #0x10000
	mov w0, #-2
	mov x0, #0xffffffff
	mov x0, #0x3fffffffffff
	mov x1, x2
	mov w1, w2
	mov x1, sp
	mov sp, x1
	movk x0, #0x1234, lsl #16
	cmp x0, #5
	cmp x0, #4096
	cmp x0, x1
	cmn x0, #5
	cset x0, eq
	cset w0, lt
	lsr x0, x1, #3
	lsl x0, x1, #3
	asr w0, w1, #2
	lsl x0, x1, x2
	and x0, x1, #0xff
	and x0, x1, #0x7
	and w0, w1, #1
	orr x0, x1, #0x100
	eor x0, x1, x2
	bic x0, x1, x2
	add x0, x1, #0xfff, lsl #12
	add x0, x1, #2, lsl #12
	sub x0, x1, x2
	add x0, x1, x2, lsl #3
	mul x0, x1, x2
	msub x0, x1, x2, x3
	udiv x0, x1, x2
	sdiv x0, x1, x2
	sxtw x0, w1
	ubfx x0, x1, #3, #5
	neg x0, x1
	stp x29, x30, [sp, -16]!
	ldp x29, x30, [sp], 16
	stp x19, x20, [sp, 16]
	str x0, [x29, 24]
	str w0, [x29, 24]
	str d0, [x29, 24]
	str s0, [x29, 24]
	str x0, [x1]
	ldr x0, [x1, 8]
	ldr w0, [x1, 8]
	ldr d0, [x1]
	ldr s0, [x1]
	ldrb w0, [x1, 3]
	strb w0, [x1, 3]
	ldrh w0, [x1, 2]
	ldrsb x0, [x1, 3]
	ldrsh x0, [x1, 2]
	ldrsw x0, [x1, 4]
	ldr x0, [x1, x2]
	ldr x0, [x1, x2, lsl #3]
	ldr x0, [x1, 1]
	fmov d0, x0
	fmov x0, d0
	fmov s0, w0
	fmov w0, s0
	fmov d0, d1
	fadd d0, d1, d2
	fsub s0, s1, s2
	fmul d0, d1, d2
	fdiv d0, d1, d2
	fneg d0, d1
	fsqrt d0, d1
	fcmpe d0, d1
	fcmpe s0, s1
	fcvt d0, s1
	fcvt s0, d1
	fcvtzs x0, d1
	fcvtzs w0, s1
	fcvtzu x0, d1
	scvtf d0, x1
	scvtf d0, w1
	ucvtf s0, x1
	dmb ish
	dsb sy
	isb
	mrs x0, tpidr_el0
	msr tpidr_el0, x0
	svc #0
	brk #1000
	yield
	nop
	blr x1
	ldaxr x0, [x1]
	stlxr w2, x0, [x1]
	ldxr x0, [x1]
	stxr w2, x0, [x1]
	cbz w0, f
	tbnz x0, #3, f
.p2align 4
g:
	ret
.data
	.quad f
	.quad g+8
	.xword f

// atomy, dočasná návěští, ADDEND, literály, ltmpN
	.text
	.globl _f
_f:
	bl _f
	b _f
	b Lin
Lin:	bl Lother
	adrp x0, Lstr@PAGE
	add x0, x0, Lstr@PAGEOFF
	adrp x0, Lin@PAGE
	add x0, x0, Lin@PAGEOFF
	adrp x0, _g@PAGE
	adrp x0, _ext@PAGE+8
	ldr x1, [x0, _ext@PAGEOFF+8]
	ldr x1, [x0, _loc@PAGEOFF]
	adrp x0, Ldat@PAGE+16
	ldr x1, [x0, Ldat@PAGEOFF+16]
	bl _ext
	b.eq Lother
	cbz x0, Lin
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
	.subsections_via_symbols

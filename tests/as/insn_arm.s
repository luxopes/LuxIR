@ Instrukce ARM (A32, ARMv7-A, VFPv3-D16) a pseudoinstrukce jako GNU as -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard
.syntax unified
.arm
.text
.globl f
f:
	mov r0, #0xff000000
	mov r0, #-1
	mov r0, #-2
	and r0, r1, #0xfffffff0
	add r0, r1, #-4
	sub r0, r1, #-4
	cmp r0, #-1
	cmn r0, #-1
	adds r0, r1, #0x104
	movlo r0, #1
	mvnhs r0, #0
	cmpeq r0, r1
	lsl r0, r1, #3
	lsr r0, r1, r2
	asr r0, r1, #31
	mov r0, r1, lsl #2
	add r0, r1, r2, lsr r3
	ldr r0, [r1, #-4]
	ldr r0, [r1, r2]
	ldr r0, [r1, r2, lsl #2]
	ldr r0, [r1], #4
	ldr r0, [r1, #4]!
	strh r0, [r1, #6]
	ldrsb r0, [r1, #-1]
	ldrsh r0, [r1, r2]
	ldrd r0, r1, [r2, #8]
	strexd r0, r2, r3, [r4]
	ldrexd r2, r3, [r4]
	push {r4-r11, lr}
	pop {r4-r11, pc}
	stm r0, {r1-r3}
	ldm r0!, {r1, r2}
	stmdb sp!, {r4, lr}
	umull r0, r1, r2, r3
	smull r0, r1, r2, r3
	mla r0, r1, r2, r3
	clz r0, r1
	udf #0
	yield
	nop
	dmb ish
	dmb
	dsb sy
	isb
	mrc p15, 0, r0, c13, c0, 3
	mcr p15, 0, r0, c7, c10, 5
	vmov s0, r1
	vmov r1, s0
	vmov r0, r1, d0
	vmov d0, r0, r1
	vmov.f64 d0, #1.0
	vmov.f64 d0, #-0.5
	vmov.f32 s0, #2.0
	vmov.f64 d0, d1
	vmrs APSR_nzcv, fpscr
	vmrs r0, fpscr
	vmsr fpscr, r0
	vcmp.f64 d0, d1
	vcmp.f64 d0, #0
	vcmpe.f64 d0, d1
	vcvt.f64.s32 d0, s1
	vcvt.f64.u32 d0, s1
	vcvt.s32.f64 s0, d1
	vcvt.u32.f64 s0, d1
	vcvt.f32.f64 s0, d1
	vcvt.f64.f32 d0, s1
	vcvt.f32.s32 s0, s1
	vcvt.s32.f32 s0, s1
	vsqrt.f64 d0, d1
	vneg.f32 s0, s1
	vabs.f64 d0, d1
	vsub.f32 s0, s1, s2
	vmul.f64 d0, d1, d2
	vdiv.f32 s0, s1, s2
	vldr s1, [r0, #-8]
	vstr d7, [sp, #16]
	vpush {d8-d15}
	vpop {d8}
	vpush {d0}
	vldr d0, .Lp
	ldr r0, .Lp
	movw r0, #:lower16:".Lp"
	movt r0, #:upper16:".Lp"
	movw r0, #0xffff
	movt r0, #0x1234
	bxeq lr
	blx r3
	bl f
	bne f
	.p2align 3
.Lp:
	.quad 0
	mov r0, #0x1234
	movs r0, #1
	teq r0, r1
	tst r0, #4
	rsbs r0, r1, #0
	orrs r0, r1, r2
	movw r1, #:lower16:v-8
	movt r1, #:upper16:v-8
	bl v+8
	.word v+4
	ldr r0, [pc, #8]
	smlal r0, r1, r2, r3
	mls r0, r1, r2, r3
	uxtb r0, r1
	sxth r0, r1
	rev r0, r1
	bfi r0, r1, #4, #8
	ubfx r0, r1, #4, #8
	sbfx r0, r1, #4, #8
	pld [r0]
.globl g2
loc2:
	nop
.Lloc2:
	nop
g2:
	bl loc2
	bl .Lloc2
	bl g
	b loc2
	beq .Lloc2
	movw r0, #:lower16:loc2
	movt r0, #:upper16:loc2
	movw r1, #:lower16:g2
	.word loc2
	.word .Lloc2+4
	.word g2
	ldr r0, .Lloc2
	vldr d0, .Lloc2
	ldr r0, loc2
.macro kernel_result
	movw ip, #0xf001
	movt ip, #0xffff
	mov r1, #0
	cmp r0, ip
	mvnhs r1, #0
.endm
.macro addc dst, src, k
	add \dst, \src, #\k
.endm
	kernel_result
	addc r0, r1, 8
	clrex
	ldm r0!, {r1, r2}
	stmia r1!, {r2-r4}
	ldmfd sp!, {r4, pc}
	stmfd sp!, {r4, lr}
	mov r0, #0x1234
	ldr r0, [r1], r2
	str r0, [r1, -r2]
	ldrb r0, [r1, r2, lsr #3]
	movlo r0, #1
	mvnhs r1, #0
	cmpeq r0, r1
	bhs .Lloc2
	blo .Lloc2
	bls .Lloc2
	bxls lr
	vcmpe.f32 s0, #0
	vpush {s4-s7}
	vpop {s4}
	vmla.f64 d0, d1, d2
	vmls.f32 s0, s1, s2
	dsb ish
	mcr p15, 0, r0, c7, c10, 5
	pld [r0, #32]
	.p2align 3
	.word 1, 2
	.short 3

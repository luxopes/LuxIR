# Instrukce RISC-V (RV64IMAFD, Zicsr, Zifencei) a pseudoinstrukce jako GNU as -march=rv64g -mno-relax
.text
.globl f
f:
	la a1, v
	lla a2, v
	ld a3, v
	sd a3, v, t0
	fld fa0, .Lfp1, t1
	sb a0, v, t2
	jalr a0
	jalr ra, a0
	jalr a0, 8(a1)
	call f
	li a0, 0x1000
	li a0, 0x1800
	li a0, 0x123456789abcdef
	li a0, -0x80000000
	li a0, 0x80000000
	li a0, 0xffffffff
	zext.w a0, a1
	fge.d a0, fa0, fa1
	fgt.d a0, fa0, fa1
	fneg.d fa0, fa1
	fcvt.l.d a0, fa0, rtz
	fcvt.d.l fa0, a0
	bgt a0, a1, f
	ble a0, a1, f
	bgtu a0, a1, f
	bleu a0, a1, f
	blez a0, f
	bgez a0, f
	bgtz a0, f
	nop
	fence
	fence rw, rw
	fence.i
	csrr a0, mstatus
	csrw mtvec, a0
	csrs 0x300, a0
	csrsi mstatus, 8
	csrrci a0, mstatus, 8
	csrc mie, a0
	rdtime a0
	frcsr a0
	fscsr a0
	amoadd.d.aqrl a0, a1, (a2)
	amoswap.d.aqrl a0, a1, (a2)
	lr.d.aqrl a0, (a1)
	sc.d.aqrl a0, a1, (a2)
	sfence.vma
	sfence.vma a0, a1
	mret
	sret
	wfi
	ebreak
	ecall
	sub a0, a1, a2
	sraw a0, a1, a2
	negw a0, a1
	neg a0, a1
	not a0, a1
	mulw a0, a1, a2
	divw a0, a1, a2
	fmv.d fa0, fa1
	fabs.d fa0, fa1
	fmin.d fa0, fa1, fa2
	fcvt.s.d fa0, fa1
	fcvt.d.s fa0, fa1
	flw fa0, 0(a0)
	fsw fa0, 8(a0)
	fcvt.w.s a0, fa0, rtz
	fcvt.wu.d a0, fa0, rtz
	fcvt.lu.d a0, fa0, rtz
	fcvt.d.lu fa0, a0
	fcvt.d.w fa0, a0
	fcvt.d.wu fa0, a0
	fmv.w.x fa0, a0
	fmv.x.w a0, fa0
	lui a0, 0x12345
	auipc a0, 0x12345
	sext.b a0, a1
.data
v: .quad 1
.Lfp1: .quad 2


f2:
	bne a0, a1, ext
	beq a0, a1, .Lx
	j ext
	beqz a0, g
	nop
.Lx:
	nop
	call ext
	la a0, .Lx
.globl g
g:
	nop
	jal ext
	jal ra, .Lx
	bgt a0, a1, ext
.data
.Ly: .quad .Lx+8
	.quad g
	.quad ext+4
	.word .Lx
.section .rodata
.Lz: .quad 0
	.equ KONST, 0x1234
	.equ DALSI, (KONST << 4) | 7
	li a0, KONST
	li a1, DALSI
	li t2, (70 << 16) | 0x3333
	lui a0, %hi(v)
	addi a0, a0, %lo(v)
	ld a1, %lo(v)(a0)
	sd a1, %lo(v)(a0)
	lui a0, %hi(.Lz)
	lw a1, %lo(.Lz)(a0)
	auipc a0, %pcrel_hi(ext)
	zext.h a0, a1
	sext.h a0, a1
	sltz a0, a1
	sgtz a0, a1
	fmadd.d fa0, fa1, fa2, fa3
	fnmsub.s fa0, fa1, fa2, fa3
	fclass.d a0, fa0
	fmax.s fa0, fa1, fa2
	fsgnjx.d fa0, fa1, fa2
	fadd.d fa0, fa1, fa2, rne
	fcvt.s.w fa0, a0
	fcvt.s.l fa0, a0, rtz
	fcvt.l.s a0, fa0
	fle.s a0, fa0, fa1
	rdcycle a0
	frrm a0
	fsflags a0, a1
	amoor.w.aq a0, a1, (a2)
	amominu.d.rl a0, a1, (a2)
	lr.w a0, (a1)
	sc.w.rl a0, a1, (a2)
	mulhu a0, a1, a2
	remuw a0, a1, a2
	sraiw a0, a1, 3
	srlw a0, a1, 2
	and a0, a1, -1
	sltu a0, a1, 5
	or a0, a1, 0x7ff
	sra a0, a1, 63
	addw a0, a1, -5
	jalr ra, 8(a1)
	jalr a0, a1, 4
	fence.i
	fence iorw, rw
	csrrwi a0, mepc, 31
	csrwi satp, 0
	csrrs a0, 0x7c0, a1
	.option push
	.option norvc
	nop
	.option pop
	.data
	.half 1, 2
	.dword 5
	.word v

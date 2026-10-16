#include "all.h"

int i386_rsave[] = {
	EAX, ECX, EDX,
	XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6,
	-1
};
int i386_rclob[] = {EBX, ESI, EDI, -1};

/* vlastnosti operací jako u amd64 (ops.h, sloupec X) */
I386Op i386_op[NOp] = {
#define O(op, t, x) [O##op] =
#define X(nm, zf, lf) { nm, zf, lf, },
	#include "../ops.h"
};

static int
i386_memargs(int op)
{
	return i386_op[op].nmem;
}

Target T_i386 = {
	.name = "i386",
	.w32 = 1,
	.w32ops = W32Ucvt,
	.gpr0 = EAX,
	.ngpr = NGPR,
	.fpr0 = XMM0,
	.nfpr = NFPR,
	.rglob = BIT(EBP) | BIT(ESP),
	.nrglob = 2,
	.rsave = i386_rsave,
	.nrsave = {NGPS, NFPS},
	.retregs = i386_retregs,
	.argregs = i386_argregs,
	.memargs = i386_memargs,
	.abi0 = elimsb,
	.abi1 = i386_abi,
	.isel = i386_isel,
	.emitfn = i386_emitfn,
	.emitfin = elf_emitfin,
	.asloc = ".L",
	.cansel = 0,
};

MAKESURE(i386_arrays_ok,
	sizeof i386_rsave == (NGPS+NFPS+1) * sizeof(int) &&
	sizeof i386_rclob == (NCLR+1) * sizeof(int)
);

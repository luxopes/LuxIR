#include "all.h"

int arm_rsave[] = {
	R0, R1, R2, R3, LR,
	D0, D1, D2, D3, D4, D5, D6,
	-1
};
int arm_rclob[] = {
	R4, R5, R6, R7, R8, R9, R10,
	D8, D9, D10, D11, D12, D13, D14,
	-1
};

#define RGLOB (BIT(FP) | BIT(IP) | BIT(SP) | BIT(D7))

static int
arm_memargs(int op)
{
	(void)op;
	return 0;
}

Target T_arm = {
	.name = "arm",
	.w32 = 1,
	.w32ops = W32Div,
	.armasm = 1,
	.gpr0 = R0,
	.ngpr = NGPR,
	.fpr0 = D0,
	.nfpr = NFPR,
	.rglob = RGLOB,
	.nrglob = 4,
	.rsave = arm_rsave,
	.nrsave = {NGPS, NFPS},
	.retregs = arm_retregs,
	.argregs = arm_argregs,
	.memargs = arm_memargs,
	.abi0 = elimsb,
	.abi1 = arm_abi,
	.isel = arm_isel,
	.emitfn = arm_emitfn,
	.emitfin = elf_emitfin,
	.asloc = ".L",
	.cansel = 0,
};

MAKESURE(arm_arrays_ok,
	sizeof arm_rsave == (NGPS+NFPS+1) * sizeof(int) &&
	sizeof arm_rclob == (NCLR+1) * sizeof(int)
);

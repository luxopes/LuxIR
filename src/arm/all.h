#include "../all.h"

/* arm (ARMv7-A, režim ARM, VFPv3-D16, AAPCS s hard-float): po w32split()
 * jsou v IR jen slova a desetinná čísla. r12 (ip) a d7 jsou pomocné
 * registry emitoru, r11 (fp) ukazatel rámce. */

enum ArmReg {
	R0 = RXX + 1,
	     R1,  R2,  R3,  R4,  R5,  R6,  R7,
	R8,  R9,  R10, FP,  IP,  SP,  LR,

	D0,  D1,  D2,  D3,  D4,  D5,  D6,  D7,
	D8,  D9,  D10, D11, D12, D13, D14, D15,

	/* d0-d14; d7 je v rglob (pomocný), d15 se nepoužívá */
	NFPR = D14 - D0 + 1,
	/* r0-lr; fp, ip a sp jsou v rglob */
	NGPR = LR - R0 + 1,
	NGPS = R3 - R0 + 1 + 1,      /* r0-r3, lr */
	NFPS = D6 - D0 + 1,          /* d0-d6 */
	NCLR = (R10 - R4 + 1) + (D14 - D8 + 1),
};
MAKESURE(arm_reg_not_tmp, D15 < (int)Tmp0);

/* targ.c */
extern int arm_rsave[];
extern int arm_rclob[];

/* abi.c */
extern uint arm_argsz;
bits arm_retregs(Ref, int[2]);
bits arm_argregs(Ref, int[2]);
void arm_abi(Fn *);

/* isel.c */
int arm_imm(uint32_t);
void arm_isel(Fn *);

/* emit.c */
void arm_emitfn(Fn *, FILE *);

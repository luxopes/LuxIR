#include "../all.h"

/* i386 (i686 se SSE2), System V ABI (cdecl): všechny argumenty na
 * zásobníku, výsledek v eax (edx:eax pro dvojici) a desetinný v st(0).
 * Kl tu v IR po w32split() není. */

enum I386Reg {
	EAX = RXX+1, /* caller-save */
	ECX,
	EDX,

	EBX, /* callee-save */
	ESI,
	EDI,

	EBP, /* globally live */
	ESP,

	XMM0, /* sse, caller-save */
	XMM1,
	XMM2,
	XMM3,
	XMM4,
	XMM5,
	XMM6,
	XMM7, /* pomocný registr emitoru */

	NFPR = XMM6 - XMM0 + 1,
	NGPR = ESP - EAX + 1,
	NGPS = EDX - EAX + 1,
	NFPS = NFPR,
	NCLR = EDI - EBX + 1,
};
MAKESURE(i386_reg_not_tmp, XMM7 < (int)Tmp0);

typedef struct I386Op I386Op;

/* sloupec X v ops.h: paměťové operandy, nastavuje ZF, nemění příznaky */
struct I386Op {
	char nmem;
	char zflag;
	char lflag;
};

/* targ.c */
extern int i386_rsave[];
extern int i386_rclob[];
extern I386Op i386_op[];

/* abi.c */
extern uint i386_argsz;
extern int i386_fret;
bits i386_retregs(Ref, int[2]);
bits i386_argregs(Ref, int[2]);
void i386_abi(Fn *);

/* isel.c */
void i386_isel(Fn *);

/* emit.c */
void i386_emitfn(Fn *, FILE *);

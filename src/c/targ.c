#include "../all.h"

/* C pro 32bitové cíle (c32): kód vzniká přímo z IR v SSA (c_emitfn), bez
 * výběru instrukcí a přidělování registrů; výsledné C přeloží clang nebo gcc
 * pro cílovou architekturu (riscv32, arm, i686 …). Viz c/emit.c. */

static void
noop(Fn *fn)
{
	(void)fn;
}

static bits
noregs(Ref r, int p[2])
{
	(void)r;
	p[0] = 0;
	p[1] = 0;
	return 0;
}

static int
nomemargs(int op)
{
	(void)op;
	return 0;
}

static int c_rsave[] = {-1};

Target T_c32 = {
	.name = "c32",
	.wasm = 0,
	.cgen = 1,
	.gpr0 = 0,
	.ngpr = 0,
	.fpr0 = 0,
	.nfpr = 0,
	.rglob = 0,
	.nrglob = 0,
	.rsave = c_rsave,
	.nrsave = {0, 0},
	.retregs = noregs,
	.argregs = noregs,
	.memargs = nomemargs,
	.abi0 = elimsb,
	.abi1 = noop,
	.isel = noop,
	.emitfn = c_emitfn,
	.emitfin = c_emitfin,
	.asloc = "",
	.assym = "",
	.cansel = 0,
};

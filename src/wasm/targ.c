#include "../all.h"

/* WebAssembly (wasm32): kód vzniká přímo z IR v SSA (wasm_emitfn), bez
 * výběru instrukcí a přidělování registrů; viz wasm/emit.c. */

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

static int wasm_rsave[] = {-1};

Target T_wasm32 = {
	.name = "wasm32",
	.wasm = 1,
	.gpr0 = 0,
	.ngpr = 0,
	.fpr0 = 0,
	.nfpr = 0,
	.rglob = 0,
	.nrglob = 0,
	.rsave = wasm_rsave,
	.nrsave = {0, 0},
	.retregs = noregs,
	.argregs = noregs,
	.memargs = nomemargs,
	.abi0 = elimsb,
	.abi1 = noop,
	.isel = noop,
	.emitfn = wasm_emitfn,
	.emitfin = wasm_emitfin,
	.asloc = "",
	.assym = "",
	.cansel = 0,
};

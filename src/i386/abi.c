#include "all.h"

/* System V i386 ABI (cdecl) po w32split(): parametry a argumenty jsou slova
 * (dvojice Kl jako dvě po sobě jdoucí slova), s a d na zásobníku po 4
 * a 8 bajtech bez dalšího zarovnání. Argumenty se zapisují do oblasti na
 * dně rámce ([esp+off]); její největší velikost ve funkci je i386_argsz
 * (rezervuje ji emitor). Výsledek: eax, dvojice edx:eax, desetinný v st(0)
 * (emitor ho převede z/do xmm0).
 *
 * Druhý argument call (RCall) a jmp.arg u ret0:
 *
 *  3   2   0
 *  |x|x|xx|   gp registry výsledku (0..2), bit 2 výsledek d, bit 3 výsledek s
 */

uint i386_argsz;
/* desetinný výsledek funkce (4 d, 8 s, jinak 0): simpljmp sloučí návraty
 * do jednoho bloku bez jmp.arg, emitor ho bere odtud */
int i386_fret;

bits
i386_retregs(Ref r, int p[2])
{
	bits b;
	int ni, nf;

	assert(rtype(r) == RCall);
	ni = r.val & 3;
	nf = (r.val >> 2) & 3 ? 1 : 0;
	b = 0;
	if (ni >= 1)
		b |= BIT(EAX);
	if (ni >= 2)
		b |= BIT(EDX);
	if (nf)
		b |= BIT(XMM0);
	if (p) {
		p[0] = ni;
		p[1] = nf;
	}
	return b;
}

bits
i386_argregs(Ref r, int p[2])
{
	(void)r;
	if (p) {
		p[0] = 0;
		p[1] = 0;
	}
	return 0;
}

static int
retbits(int k, int pair)
{
	if (pair)
		return 2;
	if (KBASE(k) == 0)
		return 1;
	return k == Kd ? 4 : 8;
}

static void
selret(Blk *b, Fn *fn)
{
	int j, ca;
	Ref r, h;
	Ins *i;

	(void)fn;
	j = b->jmp.type;
	if (!isret(j) || j == Jret0)
		return;
	r = b->jmp.arg;
	b->jmp.type = Jret0;
	switch (j) {
	case Jretl:
		/* pairhi s horním slovem (gcm za něj může přesunout výpočty) */
		for (i=&b->ins[b->nins]; i>b->ins; i--)
			if ((i-1)->op == Opairhi && req((i-1)->to, R))
				break;
		if (i == b->ins)
			die("i386: retl without the high word");
		i--;
		h = i->arg[0];
		*i = (Ins){.op = Onop};
		emit(Ocopy, Kw, TMP(EDX), h, R);
		emit(Ocopy, Kw, TMP(EAX), r, R);
		ca = 2;
		break;
	case Jretw:
		emit(Ocopy, Kw, TMP(EAX), r, R);
		ca = 1;
		break;
	case Jrets:
		emit(Ocopy, Ks, TMP(XMM0), r, R);
		ca = 8;
		i386_fret = 8;
		break;
	case Jretd:
		emit(Ocopy, Kd, TMP(XMM0), r, R);
		ca = 4;
		i386_fret = 4;
		break;
	default:
		die("i386: unsupported return");
	}
	b->jmp.arg = CALL(ca);
}

static int
argsize(Ins *i)
{
	return i->cls == Kd ? 8 : 4;
}

/* i0..i1: argumenty, i1: call; hi: horní slovo výsledku Kl nebo R */
static void
selcall(Fn *fn, Ins *i0, Ins *i1, Ref hi)
{
	Ins *i;
	uint off, sz;
	int ca, st;
	Ref r;

	sz = 0;
	for (i=i0; i<i1; i++)
		if (i->op != Oargv)
			sz += argsize(i);
	ca = retbits(i1->cls, i1->cls == Kl);
	if (KBASE(i1->cls) == 1 && sz < 8)
		sz = 8;
	if (sz > i386_argsz)
		i386_argsz = sz;

	/* spill a rega čekají za call kopii z každého výsledkového registru,
	 * i nepoužitého (cíl R) */
	if (i1->cls == Kl) {
		emit(Ocopy, Kw, hi, TMP(EDX), R);
		emit(Ocopy, Kw, i1->to, TMP(EAX), R);
	} else if (KBASE(i1->cls) == 0)
		emit(Ocopy, Kw, i1->to, TMP(EAX), R);
	else
		emit(Ocopy, i1->cls, i1->to, TMP(XMM0), R);
	emit(Ocall, i1->cls == Kl ? Kw : i1->cls, R, i1->arg[0], CALL(ca));

	for (i=i0, off=0; i<i1; i++) {
		if (i->op == Oargv)
			continue;
		switch (i->cls) {
		case Kw: st = Ostorew; break;
		case Ks: st = Ostores; break;
		case Kd: st = Ostored; break;
		default: die("i386: unsupported argument class");
		}
		r = newtmp("abi", Kw, fn);
		emit(st, 0, R, i->arg[0], r);
		emit(Oadd, Kw, r, TMP(ESP), getcon(off, fn));
		off += argsize(i);
	}
}

static void
selpar(Fn *fn, Ins *i0, Ins *i1)
{
	Ins *i;
	int s;

	curi = &insb[NIns];
	fn->reg = 0;
	/* první parametr na ebp+8: SLOT(-2) */
	for (i=i0, s=2; i<i1; i++) {
		if (i->op != Opar)
			die("i386: unsupported parameter");
		emit(Oload, i->cls, i->to, SLOT(-s), R);
		s += argsize(i) / 4;
	}
	if (fn->retty >= 0)
		die("i386: aggregate results are not supported");
}

void
i386_abi(Fn *fn)
{
	Blk *b;
	Ins *i, *i0;
	Ref hi;
	int n0, n1, ioff;

	w32split(fn);
	i386_argsz = 0;
	i386_fret = 0;
	for (b=fn->start; b; b=b->link)
		b->visit = 0;

	/* parametry */
	for (b=fn->start, i=b->ins; i<&b->ins[b->nins]; i++)
		if (!ispar(i->op))
			break;
	selpar(fn, b->ins, i);
	n0 = &insb[NIns] - curi;
	ioff = i - b->ins;
	n1 = b->nins - ioff;
	vgrow(&b->ins, n0+n1);
	icpy(b->ins+n0, b->ins+ioff, n1);
	icpy(b->ins, curi, n0);
	b->nins = n0+n1;

	/* volání a návraty */
	for (b=fn->start; b; b=b->link) {
		curi = &insb[NIns];
		selret(b, fn);
		hi = R;
		for (i=&b->ins[b->nins]; i!=b->ins;)
			switch ((--i)->op) {
			default:
				emiti(*i);
				break;
			case Opairhi:
				/* horní slovo výsledku předchozího call */
				hi = i->to;
				break;
			case Ocall:
				for (i0=i; i0>b->ins; i0--)
					if (!isarg((i0-1)->op))
						break;
				selcall(fn, i0, i, hi);
				hi = R;
				i = i0;
				break;
			case Oarg:
			case Oargv:
				die("unreachable");
			}
		idup(b, curi, &insb[NIns]-curi);
	}

	if (debug['A']) {
		fprintf(stderr, "\n> After ABI lowering:\n");
		printfn(fn, stderr);
	}
}

#include "all.h"

/* AAPCS (hard-float) po w32split(): slova v r0-r3, dvojice Kl (par/arg
 * s arg[1] = INT(1) a následující horní slovo) v sudé dvojici r0:r1 nebo
 * r2:r3, d v d0-d6 a s v dolní polovině dalšího volného d (bez
 * dosazování do děr v s0-s15; d7 je pomocný registr emitoru). Co se
 * nevejde, jde na zásobník po 4 bajtech, dvojice a d zarovnané na 8; když
 * se dvojice nevejde, zbylé r se už nepoužijí. Volání s ... předává podle
 * základního AAPCS i desetinná čísla v r0-r3 a na zásobníku.
 *
 * Argumenty na zásobníku jdou do oblasti na dně rámce ([sp+off]),
 * největší velikost je arm_argsz. Výsledek: r0, dvojice r0:r1, d0 (s0).
 *
 * Druhý argument call (RCall) a jmp.arg u ret0:
 *
 *  14   12  11     4  3  0
 *  |x|xx|0|xxxxxxx|xxxx|
 *   |  |    |       ` r0-r3 s argumenty (maska)
 *   |  |    ` d0-d6 s argumenty (maska)
 *   |  ` gp registry výsledku (0..2)
 *   ` výsledek v d0
 */

uint arm_argsz;

typedef struct Class Class;

struct Class {
	char stk;   /* na zásobníku */
	char pair;  /* dvojice slov (horní slovo je další argument) */
	char gpd;   /* desetinné číslo v r (varargs): d ve dvojici, s ve slově */
	int reg;    /* registr (u dvojice první) */
	uint off;   /* posun na zásobníku */
	int size;
};

bits
arm_retregs(Ref r, int p[2])
{
	bits b;
	int ni, nf;

	assert(rtype(r) == RCall);
	ni = (r.val >> 12) & 3;
	nf = (r.val >> 14) & 1;
	b = 0;
	if (ni >= 1)
		b |= BIT(R0);
	if (ni >= 2)
		b |= BIT(R1);
	if (nf)
		b |= BIT(D0);
	if (p) {
		p[0] = ni;
		p[1] = nf;
	}
	return b;
}

bits
arm_argregs(Ref r, int p[2])
{
	bits b;
	int n, ni, nf;

	assert(rtype(r) == RCall);
	b = 0;
	ni = nf = 0;
	for (n=0; n<4; n++)
		if (r.val & (1 << n)) {
			b |= BIT(R0+n);
			ni++;
		}
	for (n=0; n<7; n++)
		if (r.val & (1 << (4+n))) {
			b |= BIT(D0+n);
			nf++;
		}
	if (p) {
		p[0] = ni;
		p[1] = nf;
	}
	return b;
}

static uint
align(uint x, uint al)
{
	return (x + al-1) & -al;
}

/* Rozdělení argumentů nebo parametrů i0..i1 (pevné před argv, va: volání
 * s ...); vrací masku registrů pro RCall a velikost na zásobníku. */
static int
argsclass(Ins *i0, Ins *i1, Class *ca, uint *stk)
{
	Ins *i;
	Class *c;
	int ngp, nfp, va, mask, need;
	uint off;

	va = 0;
	for (i=i0; i<i1; i++)
		if (i->op == Oargv)
			va = 1;
	ngp = 0;
	nfp = 0;
	off = 0;
	mask = 0;
	for (i=i0, c=ca; i<i1; i++, c++) {
		memset(c, 0, sizeof *c);
		if (i->op == Oargv)
			continue;
		c->pair = req(i->arg[1], INT(1));
		if (KBASE(i->cls) == 1 && !va) {
			if (nfp < 7) {
				c->reg = D0 + nfp;
				mask |= 1 << (4 + nfp);
				nfp++;
				continue;
			}
			/* d i s na zásobník, další desetinná čísla také */
			c->stk = 1;
			c->size = i->cls == Kd ? 8 : 4;
			off = align(off, c->size);
			c->off = off;
			off += c->size;
			continue;
		}
		if (KBASE(i->cls) == 1) {
			c->gpd = 1;
			c->pair = i->cls == Kd;
		}
		need = c->pair ? 2 : 1;
		if (c->pair && (ngp & 1))
			ngp++;
		if (ngp + need <= 4) {
			c->reg = R0 + ngp;
			mask |= ((1 << need) - 1) << ngp;
			ngp += need;
			if (c->pair && !c->gpd) {
				/* horní slovo do dalšího registru */
				i++, c++;
				memset(c, 0, sizeof *c);
				c->reg = R0 + ngp - 1;
			}
			continue;
		}
		ngp = 4;
		c->stk = 1;
		c->size = 4 * need;
		off = align(off, c->size);
		c->off = off;
		off += c->size;
		if (c->pair && !c->gpd) {
			i++, c++;
			memset(c, 0, sizeof *c);
			c->stk = 1;
			c->size = 4;
			c->off = off - 4;
		}
	}
	*stk = off;
	return mask;
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
			die("arm: retl without the high word");
		i--;
		h = i->arg[0];
		*i = (Ins){.op = Onop};
		emit(Ocopy, Kw, TMP(R1), h, R);
		emit(Ocopy, Kw, TMP(R0), r, R);
		ca = 2 << 12;
		break;
	case Jretw:
		emit(Ocopy, Kw, TMP(R0), r, R);
		ca = 1 << 12;
		break;
	case Jrets:
	case Jretd:
		emit(Ocopy, j == Jrets ? Ks : Kd, TMP(D0), r, R);
		ca = 1 << 14;
		break;
	default:
		die("arm: unsupported return");
	}
	b->jmp.arg = CALL(ca);
}

static void
selcall(Fn *fn, Ins *i0, Ins *i1, Ref hi)
{
	Ins *i;
	Class *ca, *c;
	uint stk;
	int cty, st;
	Ref r, w;

	ca = alloc((i1-i0+1) * sizeof ca[0]);
	cty = argsclass(i0, i1, ca, &stk);
	stk = align(stk, 8);
	if (stk > arm_argsz)
		arm_argsz = stk;

	/* spill a rega čekají za call kopii z každého výsledkového registru */
	if (i1->cls == Kl) {
		cty |= 2 << 12;
		emit(Ocopy, Kw, hi, TMP(R1), R);
		emit(Ocopy, Kw, i1->to, TMP(R0), R);
	} else if (KBASE(i1->cls) == 0) {
		cty |= 1 << 12;
		emit(Ocopy, Kw, i1->to, TMP(R0), R);
	} else {
		cty |= 1 << 14;
		emit(Ocopy, i1->cls, i1->to, TMP(D0), R);
	}
	emit(Ocall, Kw, R, i1->arg[0], CALL(cty));

	/* registry: kopie těsně před call */
	for (i=i0, c=ca; i<i1; i++, c++) {
		if (i->op == Oargv || c->stk)
			continue;
		if (c->gpd) {
			if (i->cls == Ks) {
				w = newtmp("abi", Kw, fn);
				emit(Ocopy, Kw, TMP(c->reg), w, R);
				emit(Ocast, Kw, w, i->arg[0], R);
			} else {
				w = newtmp("abi", Kw, fn);
				emit(Ocopy, Kw, TMP(c->reg+1), w, R);
				emit(Odhi, Kw, w, i->arg[0], R);
				w = newtmp("abi", Kw, fn);
				emit(Ocopy, Kw, TMP(c->reg), w, R);
				emit(Odlo, Kw, w, i->arg[0], R);
			}
			continue;
		}
		emit(Ocopy, i->cls, TMP(c->reg), i->arg[0], R);
	}

	/* zásobník */
	for (i=i0, c=ca; i<i1; i++, c++) {
		if (i->op == Oargv || !c->stk)
			continue;
		if (c->gpd && i->cls == Kd) {
			/* d podle základního AAPCS: osm bajtů jako double */
			st = Ostored;
		} else
			switch (i->cls) {
			case Kw: st = Ostorew; break;
			case Ks: st = Ostores; break;
			case Kd: st = Ostored; break;
			default: die("arm: unsupported argument class");
			}
		r = newtmp("abi", Kw, fn);
		emit(st, 0, R, i->arg[0], r);
		emit(Oadd, Kw, r, TMP(SP), getcon(c->off, fn));
	}
}

static void
selpar(Fn *fn, Ins *i0, Ins *i1)
{
	Class *ca, *c;
	Ins *i;
	uint stk;
	int cty;

	ca = alloc((i1-i0+1) * sizeof ca[0]);
	curi = &insb[NIns];
	for (i=i0; i<i1; i++)
		if (i->op != Opar)
			die("arm: unsupported parameter");
	cty = argsclass(i0, i1, ca, &stk);
	fn->reg = arm_argregs(CALL(cty), 0);
	if (fn->retty >= 0)
		die("arm: aggregate results are not supported");
	for (i=i0, c=ca; i<i1; i++, c++) {
		if (c->stk) {
			/* první parametr na zásobníku na fp+8: SLOT(-2) */
			emit(Oload, i->cls, i->to, SLOT(-(int)(2 + c->off / 4)), R);
			continue;
		}
		emit(Ocopy, i->cls, i->to, TMP(c->reg), R);
	}
}

void
arm_abi(Fn *fn)
{
	Blk *b;
	Ins *i, *i0;
	Ref hi;
	int n0, n1, ioff;

	w32split(fn);
	arm_argsz = 0;
	if (fn->vararg)
		die("arm: variadic functions are not supported");
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

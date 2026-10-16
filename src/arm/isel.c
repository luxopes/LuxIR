#include "all.h"
#include <limits.h>

/* Výběr instrukcí arm:
 *
 * - okamžité hodnoty jen kódovatelné (8 bitů s rotací o sudý počet) ve
 *   druhém operandu add/sub/and/or/xor/cmp; jinak do registru, add a sub
 *   se při záporné konstantě prohodí, cmp na cmn
 * - adresy [reg, #off]: add s konstantou ze stejného bloku se sloučí do
 *   paměťového operandu (RMem), velké posuny legalizuje emitor přes ip
 * - desetinné konstanty z paměti, adresy symbolů přes movw/movt
 * - rychlé lokální proměnné jako u i386 (alloc s konstantou kdekoli)
 * - porovnání přes acmp/acmn/afcmp a příznaky jako u arm64 */

int
arm_imm(uint32_t x)
{
	int r;

	for (r=0; r<32; r+=2)
		if ((((x << r) | (x >> ((32 - r) & 31))) & ~0xffu) == 0)
			return 1;
	return 0;
}

static int
conbits(Ref r, Fn *fn, int64_t *v)
{
	Con *c;

	if (rtype(r) != RCon)
		return 0;
	c = &fn->con[r.val];
	if (c->type != CBits)
		return 0;
	*v = c->bits.i;
	return 1;
}

static void
fixarg(Ref *pr, int k, int phi, Fn *fn)
{
	char buf[32];
	Con *c, cc;
	Ref r0, r1, r2;
	int s, n;

	/* třída m (adresa) je tu slovo */
	if (k == Kl)
		k = Kw;
	r0 = *pr;
	switch (rtype(r0)) {
	case RCon:
		c = &fn->con[r0.val];
		if (c->type == CAddr && c->sym.type != SGlo)
			die("arm: thread-local and external symbols are not supported");
		if (KBASE(k) == 0 && phi)
			return;
		r1 = newtmp("isel", k, fn);
		if (KBASE(k) == 0) {
			emit(Ocopy, k, r1, r0, R);
		} else {
			n = stashbits(c->bits.i, KWIDE(k) ? 8 : 4);
			cc = (Con){.type = CAddr};
			sprintf(buf, "\"%sfp%d\"", T.asloc, n);
			cc.sym.id = intern(buf);
			r2 = newtmp("isel", Kw, fn);
			emit(Oload, k, r1, r2, R);
			emit(Ocopy, Kw, r2, newcon(&cc, fn), R);
		}
		*pr = r1;
		break;
	case RTmp:
		s = fn->tmp[r0.val].slot;
		if (s == -1)
			break;
		r1 = newtmp("isel", Kw, fn);
		emit(Oaddr, Kw, r1, SLOT(s), R);
		*pr = r1;
		break;
	}
}

/* Druhý operand: okamžitá hodnota, když se dá zakódovat. */
static void
fiximm(Ref *pr, Fn *fn)
{
	int64_t v;

	if (conbits(*pr, fn, &v) && arm_imm((uint32_t)v))
		return;
	fixarg(pr, Kw, 0, fn);
}

/* Adresa: add base, c (base ze stejného bloku) -> RMem [base, #c]. */
static void
seladdr(Ref *pr, Blk *b, Fn *fn)
{
	Ref r, base;
	Tmp *t;
	Ins *d;
	Addr a;
	int64_t v;
	int s;

	r = *pr;
	if (rtype(r) != RTmp || isreg(r))
		goto Plain;
	t = &fn->tmp[r.val];
	s = t->slot;
	if (s != -1) {
		/* rychlá lokální proměnná: [fp, #slot] */
		memset(&a, 0, sizeof a);
		a.offset.type = CBits;
		a.base = SLOT(s);
		chuse(r, -1, fn);
		goto Mem;
	}
	d = t->def;
	if (!d || t->bid != b->id || t->ndef != 1)
		goto Plain;
	if (d->op != Oadd || d->cls != Kw || !conbits(d->arg[1], fn, &v))
		goto Plain;
	base = d->arg[0];
	if (rtype(base) != RTmp)
		goto Plain;
	if (v < -(1 << 20) || v > (1 << 20))
		goto Plain;
	memset(&a, 0, sizeof a);
	a.offset.type = CBits;
	a.offset.bits.i = v;
	if (!isreg(base) && fn->tmp[base.val].slot != -1) {
		a.offset.bits.i = v;
		a.base = SLOT(fn->tmp[base.val].slot);
	} else {
		a.base = base;
		chuse(base, +1, fn);
	}
	chuse(r, -1, fn);
Mem:
	vgrow(&fn->mem, ++fn->nmem);
	fn->mem[fn->nmem-1] = a;
	*pr = MEM(fn->nmem-1);
	return;
Plain:
	fixarg(pr, Kw, 0, fn);
}

static int
selcmp(Ref arg[2], int k, Fn *fn)
{
	Ref r, *iarg;
	int swap, cmp;
	int64_t v;

	if (KBASE(k) == 1) {
		emit(Oafcmp, k, R, arg[0], arg[1]);
		iarg = curi->arg;
		fixarg(&iarg[0], k, 0, fn);
		fixarg(&iarg[1], k, 0, fn);
		return 0;
	}
	swap = rtype(arg[0]) == RCon;
	if (swap) {
		r = arg[1];
		arg[1] = arg[0];
		arg[0] = r;
	}
	cmp = Oacmp;
	r = arg[1];
	if (conbits(r, fn, &v) && !arm_imm((uint32_t)v) && arm_imm(-(uint32_t)v)) {
		cmp = Oacmn;
		r = getcon((int32_t)-(uint32_t)v, fn);
	}
	emit(cmp, Kw, R, arg[0], r);
	iarg = curi->arg;
	fixarg(&iarg[0], Kw, 0, fn);
	fiximm(&iarg[1], fn);
	return swap;
}

static void
sel(Ins i, Blk *b, Fn *fn)
{
	Ref *iarg, r;
	Ins *i0;
	int ck, cc;
	int64_t v;

	if (rtype(i.to) == RTmp)
	if (!isreg(i.to) && (i.op == Oadd || (!isreg(i.arg[0]) && !isreg(i.arg[1]))))
	if (i.op != Oaddc && i.op != Osubc) /* přenos pro adc/sbc */
	if (fn->tmp[i.to.val].nuse == 0) {
		chuse(i.arg[0], -1, fn);
		chuse(i.arg[1], -1, fn);
		return;
	}
	if (iscmp(i.op, &ck, &cc)) {
		emit(Oflag, Kw, i.to, R, R);
		i0 = curi;
		if (selcmp(i.arg, ck, fn))
			i0->op += cmpop(cc);
		else
			i0->op += cc;
		return;
	}
	switch (i.op) {
	case Onop:
		return;
	case Oadd:
	case Osub:
		if (KBASE(i.cls) == 1)
			break;
		if (rtype(i.arg[0]) == RCon && i.op == Oadd) {
			r = i.arg[0];
			i.arg[0] = i.arg[1];
			i.arg[1] = r;
		}
		if (conbits(i.arg[1], fn, &v) && !arm_imm((uint32_t)v)
		&& arm_imm(-(uint32_t)v)) {
			i.op = i.op == Oadd ? Osub : Oadd;
			i.arg[1] = getcon((int32_t)-(uint32_t)v, fn);
		}
		emiti(i);
		iarg = curi->arg;
		fixarg(&iarg[0], Kw, 0, fn);
		fiximm(&iarg[1], fn);
		return;
	case Oaddc:
	case Oadc:
	case Osubc:
	case Osbc:
		/* příznaky: konstanty nelze prohodit s opačnou operací */
		if ((i.op == Oaddc || i.op == Oadc) && rtype(i.arg[0]) == RCon) {
			r = i.arg[0];
			i.arg[0] = i.arg[1];
			i.arg[1] = r;
		}
		emiti(i);
		iarg = curi->arg;
		fixarg(&iarg[0], Kw, 0, fn);
		fiximm(&iarg[1], fn);
		return;
	case Oand:
	case Oor:
	case Oxor:
		if (rtype(i.arg[0]) == RCon) {
			r = i.arg[0];
			i.arg[0] = i.arg[1];
			i.arg[1] = r;
		}
		emiti(i);
		iarg = curi->arg;
		fixarg(&iarg[0], Kw, 0, fn);
		fiximm(&iarg[1], fn);
		return;
	case Oshl:
	case Oshr:
	case Osar:
		emiti(i);
		iarg = curi->arg;
		fixarg(&iarg[0], Kw, 0, fn);
		if (rtype(iarg[1]) != RCon)
			fixarg(&iarg[1], Kw, 0, fn);
		return;
	case Ocall:
		if (rtype(i.arg[0]) == RCon && fn->con[i.arg[0].val].type == CAddr
		&& !fn->con[i.arg[0].val].bits.i) {
			emiti(i);
			return;
		}
		break;
	case Ocopy:
		/* konstanty načte emitor (movw/movt, adresy) */
		emiti(i);
		iarg = curi->arg;
		if (rtype(iarg[0]) == RCon && KBASE(i.cls) == 0)
			return;
		fixarg(&iarg[0], i.cls, 0, fn);
		return;
	case Ostoreb:
	case Ostoreh:
	case Ostorew:
	case Ostores:
	case Ostored:
		emiti(i);
		iarg = curi->arg;
		seladdr(&iarg[1], b, fn);
		fixarg(&iarg[0], argcls(&i, 0), 0, fn);
		return;
	case Oalloc4:
	case Oalloc8:
	case Oalloc16:
		die("arm: dynamic stack allocation is not supported");
	default:
		if (isload(i.op)) {
			emiti(i);
			iarg = curi->arg;
			seladdr(&iarg[0], b, fn);
			return;
		}
		break;
	}
	emiti(i);
	iarg = curi->arg; /* fixarg() může změnit curi */
	fixarg(&iarg[0], argcls(&i, 0), 0, fn);
	fixarg(&iarg[1], argcls(&i, 1), 0, fn);
}

static void
seljmp(Blk *b, Fn *fn)
{
	Ref r;
	Ins *i, *ir;
	int ck, cc, use;

	if (b->jmp.type == Jret0
	|| b->jmp.type == Jjmp
	|| b->jmp.type == Jhlt)
		return;
	assert(b->jmp.type == Jjnz);
	r = b->jmp.arg;
	b->jmp.arg = R;
	if (rtype(r) == RCon) {
		b->jmp.type = Jjmp;
		if (fn->con[r.val].bits.i == 0)
			b->s1 = b->s2;
		b->s2 = 0;
		return;
	}
	if (b->s1 == b->s2) {
		chuse(r, -1, fn);
		b->jmp.type = Jjmp;
		b->s2 = 0;
		return;
	}
	use = -1;
	ir = 0;
	i = &b->ins[b->nins];
	while (i > b->ins)
		if (req((--i)->to, r)) {
			use = fn->tmp[r.val].nuse;
			ir = i;
			break;
		}
	if (ir && use == 1
	&& iscmp(ir->op, &ck, &cc)) {
		if (selcmp(ir->arg, ck, fn))
			cc = cmpop(cc);
		b->jmp.type = Jjf + cc;
		*ir = (Ins){.op = Onop};
	}
	else {
		selcmp((Ref[]){r, CON_Z}, Kw, fn);
		b->jmp.type = Jjfine;
	}
}

void
arm_isel(Fn *fn)
{
	Blk *b, **sb;
	Ins *i;
	Phi *p;
	uint n, al;
	int64_t sz;

	/* místo v rámci pro alloc s konstantní velikostí (v každém bloku) */
	for (b=fn->start; b; b=b->link)
		for (al=Oalloc, n=4; al<=Oalloc1; al++, n*=2)
			for (i=b->ins; i<&b->ins[b->nins]; i++)
				if (i->op == al) {
					if (rtype(i->arg[0]) != RCon)
						die("arm: dynamic stack allocation is not supported");
					sz = fn->con[i->arg[0].val].bits.i;
					if (sz < 0 || sz >= INT_MAX-15)
						err("invalid alloc size %"PRId64, sz);
					sz = (sz + n-1) & -n;
					sz /= 4;
					fn->slot = (fn->slot + n/4 - 1) & -(n/4);
					fn->tmp[i->to.val].slot = fn->slot;
					fn->slot += sz;
					*i = (Ins){.op = Onop};
				}

	for (b=fn->start; b; b=b->link) {
		curi = &insb[NIns];
		for (sb=(Blk*[3]){b->s1, b->s2, 0}; *sb; sb++)
			for (p=(*sb)->phi; p; p=p->link) {
				for (n=0; p->blk[n] != b; n++)
					assert(n+1 < p->narg);
				fixarg(&p->arg[n], p->cls, 1, fn);
			}
		seljmp(b, fn);
		for (i=&b->ins[b->nins]; i!=b->ins;)
			sel(*--i, b, fn);
		idup(b, curi, &insb[NIns]-curi);
	}

	if (debug['I']) {
		fprintf(stderr, "\n> After instruction selection:\n");
		printfn(fn, stderr);
	}
}

#include "all.h"

static void
blit(Ref sd[2], int sz, Fn *fn)
{
	struct { int st, ld, cls, size; } *p, tbl[] = {
		{ Ostorel, Oload,   Kl, 8 },
		{ Ostorew, Oload,   Kw, 4 },
		{ Ostoreh, Oloaduh, Kw, 2 },
		{ Ostoreb, Oloadub, Kw, 1 }
	};
	Ref r, r1, ro;
	int off, fwd, n;

	fwd = sz >= 0;
	sz = abs(sz);
	off = fwd ? sz : 0;
	for (p=tbl; sz; p++)
		for (n=p->size; sz>=n; sz-=n) {
			off -= fwd ? n : 0;
			r = newtmp("blt", Kl, fn);
			r1 = newtmp("blt", Kl, fn);
			ro = getcon(off, fn);
			emit(p->st, 0, R, r, r1);
			emit(Oadd, Kl, r1, sd[1], ro);
			r1 = newtmp("blt", Kl, fn);
			emit(p->ld, p->cls, r, r1, R);
			emit(Oadd, Kl, r1, sd[0], ro);
			off += fwd ? 0 : n;
		}
}

static int
ulog2_tab64[64] = {
	63,  0,  1, 41, 37,  2, 16, 42,
	38, 29, 32,  3, 12, 17, 43, 55,
	39, 35, 30, 53, 33, 21,  4, 23,
	13,  9, 18,  6, 25, 44, 48, 56,
	62, 40, 36, 15, 28, 31, 11, 54,
	34, 52, 20, 22,  8,  5, 24, 47,
	61, 14, 27, 10, 51, 19,  7, 46,
	60, 26, 50, 45, 59, 49, 58, 57,
};

static int
ulog2(uint64_t pow2)
{
	return ulog2_tab64[(pow2 * 0x5b31ab928877a7e) >> 58];
}

static int
ispow2(uint64_t v)
{
	return v && (v & (v - 1)) == 0;
}

/* RV64GC has no mandatory Zbb; x86-64 baseline has no POPCNT.
 * Fixed-size SWAR lowering uses no runtime call and handles zero explicitly.
 * ARM64 keeps all counts native; amd64 keeps baseline BSR/BSF. */
static Ref
bitstep(Ins *seq, int *n, int op, int k, Ref a, Ref b, Fn *fn)
{
	Ref r = newtmp("bit", k, fn);
	seq[(*n)++] = (Ins){op, k, r, {a,b}};
	return r;
}

static void
bitlower(Ins *i, Fn *fn)
{
	Ins seq[64];
	Ref x, y, z;
	int n = 0, k = i->cls, width = k == Kl ? 64 : 32, shift;
	uint64_t mask;

	x = i->arg[0];
	if (i->op == Oclz) {
		for (shift=1; shift<width; shift*=2) {
			y = bitstep(seq, &n, Oshr, k, x, getcon(shift, fn), fn);
			x = bitstep(seq, &n, Oor, k, x, y, fn);
		}
	} else if (i->op == Octz) {
		y = bitstep(seq, &n, Osub, k, x, getcon(1, fn), fn);
		x = bitstep(seq, &n, Oxor, k, x, getcon(-1, fn), fn);
		x = bitstep(seq, &n, Oand, k, x, y, fn);
	}
	mask = k == Kl ? 0x5555555555555555ull : 0x55555555;
	y = bitstep(seq, &n, Oshr, k, x, getcon(1, fn), fn);
	y = bitstep(seq, &n, Oand, k, y, getcon(mask, fn), fn);
	x = bitstep(seq, &n, Osub, k, x, y, fn);
	mask = k == Kl ? 0x3333333333333333ull : 0x33333333;
	y = bitstep(seq, &n, Oshr, k, x, getcon(2, fn), fn);
	y = bitstep(seq, &n, Oand, k, y, getcon(mask, fn), fn);
	x = bitstep(seq, &n, Oand, k, x, getcon(mask, fn), fn);
	x = bitstep(seq, &n, Oadd, k, x, y, fn);
	y = bitstep(seq, &n, Oshr, k, x, getcon(4, fn), fn);
	x = bitstep(seq, &n, Oadd, k, x, y, fn);
	mask = k == Kl ? 0x0f0f0f0f0f0f0f0full : 0x0f0f0f0f;
	x = bitstep(seq, &n, Oand, k, x, getcon(mask, fn), fn);
	for (shift=8; shift<width; shift*=2) {
		y = bitstep(seq, &n, Oshr, k, x, getcon(shift, fn), fn);
		x = bitstep(seq, &n, Oadd, k, x, y, fn);
	}
	z = bitstep(seq, &n, Oand, k, x, getcon(127, fn), fn);
	if (i->op == Oclz)
		z = bitstep(seq, &n, Osub, k, getcon(width, fn), z, fn);
	emit(Ocopy, k, i->to, z, R);
	while (n)
		emiti(seq[--n]);
}

static void
ins(Ins **pi, int *new, Blk *b, Fn *fn)
{
	ulong ni;
	Con *c;
	Ins *i;
	Ref r;
	int n;

	i = *pi;
	/* simplify more instructions here;
	 * copy 0 into xor, bit rotations,
	 * etc. */
	switch (i->op) {
	case Oclz:
	case Octz:
	case Opopcount:
		if (strncmp(T.name, "arm64", 5) == 0)
			break;
		if (strncmp(T.name, "amd64", 5) == 0 && i->op != Opopcount)
			break;
		if (!*new) {
			curi = &insb[NIns];
			ni = &b->ins[b->nins] - (i+1);
			curi -= ni;
			icpy(curi, i+1, ni);
			*new = 1;
		}
		bitlower(i, fn);
		return;
	case Oblit1:
		assert(i > b->ins);
		assert((i-1)->op == Oblit0);
		if (!*new) {
			curi = &insb[NIns];
			ni = &b->ins[b->nins] - (i+1);
			curi -= ni;
			icpy(curi, i+1, ni);
			*new = 1;
		}
		blit((i-1)->arg, rsval(i->arg[0]), fn);
		*pi = i-1;
		return;
	case Oudiv:
	case Ourem:
		r = i->arg[1];
		if (KBASE(i->cls) == 0)
		if (rtype(r) == RCon) {
			c = &fn->con[r.val];
			if (c->type == CBits)
			if (ispow2(c->bits.i)) {
				n = ulog2(c->bits.i);
				if (i->op == Ourem) {
					i->op = Oand;
					i->arg[1] = getcon((1ull<<n) - 1, fn);
				} else {
					i->op = Oshr;
					i->arg[1] = getcon(n, fn);
				}
			}
		}
		break;
	}
	if (*new)
		emiti(*i);
}

void
simpl(Fn *fn)
{
	Blk *b;
	Ins *i;
	int new;

	for (b=fn->start; b; b=b->link) {
		new = 0;
		for (i=&b->ins[b->nins]; i!=b->ins;) {
			--i;
			ins(&i, &new, b, fn);
		}
		if (new)
			idup(b, curi, &insb[NIns]-curi);
	}
}

/* Registers: result, expected scratch, desired, address, status, packet.
 * Copies protect virtual values and track implicit machine clobbers.
 * CAS expected input is explicit; output uses physical-register markers. */
void
atomicisel(Ins i, Fn *fn, int regs[6])
{
	int k = i.cls, n, cas = i.op == Oacas;
	int rmw = i.op == Oaadd || i.op == Oaxchg;
	int arm = strncmp(T.name, "arm64", 5) == 0;
	Ref arg = i.arg[1], slot;
	Ins native = i;

	if (i.op == Oafence) {
		emiti(i);
		return;
	}
	if (!req(i.to, R) && fn->tmp[i.to.val].nuse)
		emit(Ocopy, k, i.to, TMP(regs[0]), R);
	for (n=1; n<6; n++) {
		if (n == 1 && !(cas || (arm && i.op == Oaadd)))
			continue;
		if (n == 2 && i.op == Oaload)
			continue;
		if (n == 4 && !(cas || (arm && rmw)))
			continue;
		if (n == 5 && !cas)
			continue;
		emit(Ocopy, n == 1 || n == 2 ? k : Kl, R, TMP(regs[n]), R);
	}
	native.to = i.op == Oastore ? R : TMP(regs[0]);
	native.arg[0] = TMP(regs[3]);
	native.arg[1] = i.op == Oaload ? R : TMP(regs[2]);
	if (cas) {
		native.op = Oncas;
		native.to = R;
		native.arg[1] = TMP(regs[0]);
	}
	if (i.op != Oastore && !fn->tmp[i.to.val].nuse)
		emit(Ocopy, k, R, TMP(regs[0]), R);
	emiti(native);
	if (cas) {
		emit(Oadd, k, TMP(regs[1]), TMP(regs[0]), CON_Z);
		slot = newtmp("casptr", Kl, fn);
		emit(Oload, k, TMP(regs[2]), slot, R);
		emit(Oadd, Kl, slot, TMP(regs[5]), getcon(k == Kl ? 8 : 4, fn));
		emit(Oload, k, TMP(regs[0]), TMP(regs[5]), R);
		emit(Ocopy, Kl, TMP(regs[5]), arg, R);
	} else if (i.op != Oaload)
		emit(Ocopy, k, TMP(regs[2]), arg, R);
	if (cas || (arm && rmw))
		emit(Ocopy, Kl, TMP(regs[4]), CON_Z, R);
	if (arm && i.op == Oaadd)
		emit(Ocopy, k, TMP(regs[1]), CON_Z, R);
	emit(Ocopy, Kl, TMP(regs[3]), i.arg[0], R);
}

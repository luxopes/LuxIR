#include "all.h"

/* 32bitové cíle s assemblerem (arm, i386): před snížením ABI se každá
 * hodnota Kl rozloží na dvojici Kw (dolní a horní slovo) a operace Kl na
 * operace Kw. Za průchodem už v IR žádná Kl není, další fáze (ABI, výběr
 * instrukcí, přidělení registrů) pracují jen se slovy a desetinnými čísly.
 *
 * Konvence jako u c32: adresy jsou 64bitová slova s nulovou horní
 * polovinou, do paměti jde dolní slovo; Kw použité jako Kl se rozšíří
 * nulami. Dělení 64bitových čísel a převody mezi Kl a desetinnými čísly
 * volají pomocné funkce __luxir_* (runtime/w32/helpers.lir), stejně dělení
 * Kw a převody Kw bez znaménka, pokud je cíl neumí (T.w32ops).
 *
 * Dvojice v ABI: par a arg dolního slova mají arg[1] = INT(1) a za nimi
 * následuje horní slovo; call s třídou Kl vrací dolní slovo do to a horní
 * do pairhi hned za ním; před retl je pairhi s horním slovem (arg[0]).
 *
 * Nakonec se odstraní nepoužité výpočty (typicky horní slova adres). */

static Fn *fn;
static int *wlo, *whi;
static char *ishi;  /* temp je horní slovo (phi horních slov) */
static Ins *out;
static uint nout;
/* náhrady kopií: temp definovaný jako copy hodnoty (SSA) -> ta hodnota;
 * známá nulová horní slova se tak uplatní hned (mul, porovnání, add) */
static Ref *subst;
static uint nsubst;
/* horní slova adres s posunem spočtená v aktuálním bloku */
static struct { int con; Ref r; } *hcache;
static uint nhcache;

static int iscon(Ref, int64_t *);
static Ref resolve(Ref);

/* Identity slovních operací s konstantou, které GVN nezjednoduší (skládá
 * jen dvě konstanty): x+0, x|0, x^0, x-0, posun o 0 -> x; x&0, x*0 -> 0;
 * x&-1, x*1 -> x. Vzniká kopie, kterou GVN propaguje. */
static int
simplify(int op, Ref a0, Ref a1, Ref *r)
{
	int64_t v0, v1;
	int c0, c1;

	c0 = iscon(a0, &v0);
	c1 = iscon(a1, &v1);
	v0 = (int32_t)v0;
	v1 = (int32_t)v1;
	switch (op) {
	case Oadd:
	case Oor:
	case Oxor:
		if (c1 && v1 == 0) {
			*r = a0;
			return 1;
		}
		if (c0 && v0 == 0) {
			*r = a1;
			return 1;
		}
		break;
	case Osub:
	case Oshl:
	case Oshr:
	case Osar:
		if (c1 && v1 == 0) {
			*r = a0;
			return 1;
		}
		break;
	case Oand:
		if ((c1 && v1 == 0) || (c0 && v0 == 0)) {
			*r = CON_Z;
			return 1;
		}
		if (c1 && v1 == -1) {
			*r = a0;
			return 1;
		}
		if (c0 && v0 == -1) {
			*r = a1;
			return 1;
		}
		break;
	case Omul:
		if ((c1 && v1 == 0) || (c0 && v0 == 0)) {
			*r = CON_Z;
			return 1;
		}
		if (c1 && v1 == 1) {
			*r = a0;
			return 1;
		}
		if (c0 && v0 == 1) {
			*r = a1;
			return 1;
		}
		break;
	case Omulhu:
		if ((c1 && v1 == 0) || (c0 && v0 == 0)) {
			*r = CON_Z;
			return 1;
		}
		break;
	case Ocmpw+Ciult:
		/* x <u 0 */
		if (c1 && v1 == 0) {
			*r = CON_Z;
			return 1;
		}
		break;
	}
	return 0;
}

static void
put(int op, int k, Ref to, Ref a0, Ref a1)
{
	Ins *i;
	Ref r;

	a0 = resolve(a0);
	a1 = resolve(a1);
	if (k == Kw && rtype(to) == RTmp && simplify(op, a0, a1, &r)) {
		op = Ocopy;
		a0 = r;
		a1 = R;
	}
	if (op == Ocopy && k == Kw && rtype(to) == RTmp
	&& (rtype(a0) == RCon || (rtype(a0) == RTmp && fn->tmp[a0.val].cls == Kw))) {
		if (to.val >= nsubst) {
			vgrow(&subst, fn->ntmp);
			while (nsubst < (uint)fn->ntmp)
				subst[nsubst++] = R;
		}
		subst[to.val] = resolve(a0);
	}
	vgrow(&out, ++nout);
	i = &out[nout-1];
	i->op = op;
	i->cls = k;
	i->to = to;
	i->arg[0] = a0;
	i->arg[1] = a1;
}

static int
wide(Ref r)
{
	int k;

	if (rtype(r) != RTmp || r.val < Tmp0)
		return 0;
	k = fn->tmp[r.val].cls;
	return k == Kl || k < 0;
}

static Ref
nw(void)
{
	return newtmp("w32", Kw, fn);
}

static Ref
wcon(int64_t v)
{
	return getcon((int32_t)v, fn);
}

static void add64(Ref, Ref, Ref, Ref);

static int
isaddroff(Ref r)
{
	return rtype(r) == RCon && fn->con[r.val].type == CAddr && fn->con[r.val].bits.i != 0;
}

/* Horní slovo adresy symbolu s posunem jako 64bitového součtu (adresa bez
 * znaménka + posun se znaménkem, jako c32): přenos nebo výpůjčka se
 * dopočítá za běhu. */
static Ref
addrhi(Ref r)
{
	Con c;
	Ref base, dl, dh;
	int64_t off;
	uint n;

	for (n=0; n<nhcache; n++)
		if (hcache[n].con == (int)r.val)
			return hcache[n].r;
	c = fn->con[r.val];
	off = c.bits.i;
	c.bits.i = 0;
	base = newcon(&c, fn);
	dl = newtmp("w32", Kw, fn);
	dh = newtmp("w32", Kw, fn);
	add64(dl, dh, base, getcon(off, fn));
	vgrow(&hcache, ++nhcache);
	hcache[nhcache-1].con = r.val;
	hcache[nhcache-1].r = dh;
	return dh;
}

/* Slovo h (0 dolní, 1 horní) hodnoty r použité jako Kl. */
static Ref
half(Ref r, int h)
{
	Con *c;
	int64_t v;

	switch (rtype(r)) {
	case RTmp:
		if (wide(r))
			return TMP(h ? whi[r.val] : wlo[r.val]);
		return h ? CON_Z : r;
	case RCon:
		c = &fn->con[r.val];
		if (c->type == CBits) {
			v = c->bits.i;
			return wcon(h ? v >> 32 : v);
		}
		if (c->type == CAddr) {
			if (!h)
				return r;
			if (!c->bits.i)
				return CON_Z;
			return addrhi(r);
		}
		return r;
	default:
		return r;
	}
}

static Ref
resolve(Ref r)
{
	while (rtype(r) == RTmp && r.val < nsubst && !req(subst[r.val], R))
		r = subst[r.val];
	return r;
}

static Ref
lo(Ref r)
{
	return resolve(half(r, 0));
}

static Ref
hi(Ref r)
{
	return resolve(half(r, 1));
}

static int
iscon(Ref r, int64_t *v)
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

static int
iszero(Ref r)
{
	int64_t v;

	return iscon(r, &v) && v == 0;
}

static Ref
symcon(char *name)
{
	Con c;

	memset(&c, 0, sizeof c);
	c.type = CAddr;
	c.sym.type = SGlo;
	c.sym.id = intern(name);
	return newcon(&c, fn);
}

/* Výsledek Kl do dvojice (dlo, dhi); bez cíle se zahodí. */
static void
dest(Ref to, Ref *dlo, Ref *dhi)
{
	if (wide(to)) {
		*dlo = TMP(wlo[to.val]);
		*dhi = TMP(whi[to.val]);
	} else {
		*dlo = nw();
		*dhi = nw();
	}
}

/* Hodnota r jako argument Kl volání: dvojice slov. */
static void
argpair(Ref r)
{
	Ref l, h;

	/* hi() může vložit výpočet; argumenty musí jít po sobě */
	l = lo(r);
	h = hi(r);
	put(Oarg, Kw, R, l, INT(1));
	put(Oarg, Kw, R, h, R);
}

/* Volání pomocné funkce s výsledkem třídy k (Kl do dvojice). */
static void
callhelp(char *name, int k, Ref to)
{
	Ref dl, dh;

	if (k == Kl) {
		dest(to, &dl, &dh);
		put(Ocall, Kl, dl, symcon(name), R);
		put(Opairhi, Kw, dh, R, R);
	} else
		put(Ocall, k, to, symcon(name), R);
}

static void
add64(Ref dl, Ref dh, Ref a, Ref b)
{
	Ref al, ah, bl, bh, c, t;

	al = lo(a), ah = hi(a), bl = lo(b), bh = hi(b);
	if (iszero(bl) || iszero(al)) {
		put(Ocopy, Kw, dl, iszero(bl) ? al : bl, R);
		put(Oadd, Kw, dh, ah, bh);
		return;
	}
	c = nw();
	put(Oadd, Kw, dl, al, bl);
	put(Ocmpw+Ciult, Kw, c, dl, al);
	if (iszero(ah) && iszero(bh)) {
		put(Ocopy, Kw, dh, c, R);
		return;
	}
	t = nw();
	put(Oadd, Kw, t, ah, bh);
	put(Oadd, Kw, dh, t, c);
}

static void
sub64(Ref dl, Ref dh, Ref a, Ref b)
{
	Ref al, ah, bl, bh, c, t;

	al = lo(a), ah = hi(a), bl = lo(b), bh = hi(b);
	if (iszero(bl)) {
		put(Ocopy, Kw, dl, al, R);
		put(Osub, Kw, dh, ah, bh);
		return;
	}
	c = nw();
	t = nw();
	put(Ocmpw+Ciult, Kw, c, al, bl);
	put(Osub, Kw, dl, al, bl);
	put(Osub, Kw, t, ah, bh);
	put(Osub, Kw, dh, t, c);
}

static void
mul64(Ref dl, Ref dh, Ref a, Ref b)
{
	Ref al, ah, bl, bh, h1, h2, h3, t;

	al = lo(a), ah = hi(a), bl = lo(b), bh = hi(b);
	h1 = nw();
	put(Omul, Kw, dl, al, bl);
	put(Omulhu, Kw, h1, al, bl);
	t = h1;
	if (!iszero(bh)) {
		h2 = nw();
		h3 = nw();
		put(Omul, Kw, h2, al, bh);
		put(Oadd, Kw, h3, t, h2);
		t = h3;
	}
	if (!iszero(ah)) {
		h2 = nw();
		h3 = nw();
		put(Omul, Kw, h2, ah, bl);
		put(Oadd, Kw, h3, t, h2);
		t = h3;
	}
	put(Ocopy, Kw, dh, t, R);
}

/* Posun o konstantu n (0..63). */
static void
shiftcon(int op, Ref dl, Ref dh, Ref a, int n)
{
	Ref al, ah, t1, t2;

	al = lo(a), ah = hi(a);
	if (n == 0) {
		put(Ocopy, Kw, dl, al, R);
		put(Ocopy, Kw, dh, ah, R);
		return;
	}
	if (n >= 32) {
		switch (op) {
		case Oshl:
			put(Ocopy, Kw, dl, CON_Z, R);
			put(Oshl, Kw, dh, al, wcon(n - 32));
			break;
		case Oshr:
			put(Oshr, Kw, dl, ah, wcon(n - 32));
			put(Ocopy, Kw, dh, CON_Z, R);
			break;
		case Osar:
			put(Osar, Kw, dl, ah, wcon(n - 32));
			put(Osar, Kw, dh, ah, wcon(31));
			break;
		}
		return;
	}
	t1 = nw();
	t2 = nw();
	if (op == Oshl) {
		put(Oshl, Kw, dl, al, wcon(n));
		put(Oshl, Kw, t1, ah, wcon(n));
		put(Oshr, Kw, t2, al, wcon(32 - n));
		put(Oor, Kw, dh, t1, t2);
	} else {
		put(Oshr, Kw, t1, al, wcon(n));
		put(Oshl, Kw, t2, ah, wcon(32 - n));
		put(Oor, Kw, dl, t1, t2);
		put(op, Kw, dh, ah, wcon(n));
	}
}

/* Posun o proměnnou s (bere se s mod 64) bez větvení: posuny Kw berou
 * velikost mod 32, m je samé jedničky pro s >= 32. */
static void
shiftvar(int op, Ref dl, Ref dh, Ref a, Ref s)
{
	Ref al, ah, n, b5, big, m, nm, x, y, z, h1, l1, w2, sg, t, u;

	al = lo(a), ah = hi(a);
	n = nw(), b5 = nw(), big = nw(), m = nw(), nm = nw();
	x = nw(), y = nw(), z = nw(), h1 = nw(), l1 = nw(), w2 = nw();
	t = nw(), u = nw();
	put(Oand, Kw, n, s, wcon(31));
	put(Oshr, Kw, b5, s, wcon(5));
	put(Oand, Kw, big, b5, wcon(1));
	put(Oneg, Kw, m, big, R);
	put(Oxor, Kw, nm, m, wcon(-1));
	put(Oxor, Kw, y, n, wcon(31));
	if (op == Oshl) {
		/* l1 = al << n, w2 = ah << n | al >> 1 >> (31 - n) */
		put(Oshl, Kw, l1, al, n);
		put(Oshl, Kw, h1, ah, n);
		put(Oshr, Kw, x, al, wcon(1));
		put(Oshr, Kw, z, x, y);
		put(Oor, Kw, w2, h1, z);
		put(Oand, Kw, dl, l1, nm);
		put(Oand, Kw, t, l1, m);
		put(Oand, Kw, u, w2, nm);
		put(Oor, Kw, dh, t, u);
		return;
	}
	/* h1 = ah >> n, w2 = al >> n | ah << 1 << (31 - n) */
	put(op, Kw, h1, ah, n);
	put(Oshr, Kw, l1, al, n);
	put(Oshl, Kw, x, ah, wcon(1));
	put(Oshl, Kw, z, x, y);
	put(Oor, Kw, w2, l1, z);
	put(Oand, Kw, t, h1, m);
	put(Oand, Kw, u, w2, nm);
	put(Oor, Kw, dl, t, u);
	if (op == Oshr) {
		put(Oand, Kw, dh, h1, nm);
		return;
	}
	sg = nw();
	put(Osar, Kw, sg, ah, wcon(31));
	t = nw(), u = nw();
	put(Oand, Kw, t, sg, m);
	put(Oand, Kw, u, h1, nm);
	put(Oor, Kw, dh, t, u);
}

/* Porovnání Kl s podmínkou c (Cieq..Ciult) do slova r. */
static void
cmp64(int c, Ref r, Ref a, Ref b)
{
	Ref al, ah, bl, bh, x, y, o, s, e, l;
	int sgn, base, strict, lop;

	al = lo(a), ah = hi(a), bl = lo(b), bh = hi(b);
	if (c == Cieq || c == Cine) {
		o = nw();
		if (iszero(bl) && iszero(bh))
			put(Oor, Kw, o, al, ah);
		else {
			x = nw(), y = nw();
			put(Oxor, Kw, x, al, bl);
			put(Oxor, Kw, y, ah, bh);
			put(Oor, Kw, o, x, y);
		}
		put(Ocmpw+c, Kw, r, o, CON_Z);
		return;
	}
	sgn = c <= Cislt;
	base = c - (sgn ? Cisge : Ciuge);
	/* ge, gt -> gt; le, lt -> lt na horních slovech */
	if (base <= 1)
		strict = sgn ? Cisgt : Ciugt;
	else
		strict = sgn ? Cislt : Ciult;
	lop = Ciuge + base;
	s = nw(), e = nw(), l = nw(), x = nw();
	put(Ocmpw+strict, Kw, s, ah, bh);
	put(Ocmpw+Cieq, Kw, e, ah, bh);
	put(Ocmpw+lop, Kw, l, al, bl);
	put(Oand, Kw, x, e, l);
	put(Oor, Kw, r, s, x);
}

/* blit0 (zdroj, cíl) a blit1 s velikostí: kopie po slovech, se záporným
 * počtem odzadu (překrývající se oblasti). Úseky: slova od začátku, pak
 * případně půlslovo a bajt. */
static void
blitpart(Ref src, Ref dst, int off, int size)
{
	static int ld[] = {[1] = Oloadub, [2] = Oloaduh, [4] = Oload};
	static int st[] = {[1] = Ostoreb, [2] = Ostoreh, [4] = Ostorew};
	Ref v, s, d;

	v = nw(), s = nw(), d = nw();
	put(Oadd, Kw, s, lo(src), wcon(off));
	put(ld[size], Kw, v, s, R);
	put(Oadd, Kw, d, lo(dst), wcon(off));
	put(st[size], 0, R, v, d);
}

static void
blit(Ref src, Ref dst, int sz)
{
	int total, words, o;

	total = sz < 0 ? -sz : sz;
	words = total / 4;
	if (sz >= 0) {
		for (o=0; o<words*4; o+=4)
			blitpart(src, dst, o, 4);
		if (total & 2)
			blitpart(src, dst, words*4, 2);
		if (total & 1)
			blitpart(src, dst, total-1, 1);
	} else {
		if (total & 1)
			blitpart(src, dst, total-1, 1);
		if (total & 2)
			blitpart(src, dst, words*4, 2);
		for (o=words*4; o>0; o-=4)
			blitpart(src, dst, o-4, 4);
	}
}

/* log2 mocniny dvou, jinak -1 */
static int
ulog2(uint64_t v)
{
	int n;

	if (!v || (v & (v - 1)))
		return -1;
	for (n=0; !(v & 1); n++)
		v >>= 1;
	return n;
}

static void
lowerins(Ins *i, Ins *start, Ins *end)
{
	Ins *j;
	Ref dl, dh, r;
	int op, k, c, n;
	int64_t v;
	char *h;

	op = i->op;
	k = i->cls;
	switch (op) {
	case Onop:
		return;
	case Odbgloc:
		put(op, k, i->to, i->arg[0], i->arg[1]);
		return;
	case Ocopy:
		if (k == Kl) {
			dest(i->to, &dl, &dh);
			put(Ocopy, Kw, dl, lo(i->arg[0]), R);
			put(Ocopy, Kw, dh, hi(i->arg[0]), R);
		} else if (k == Kw)
			put(Ocopy, Kw, i->to, lo(i->arg[0]), R);
		else
			put(op, k, i->to, i->arg[0], R);
		return;
	case Oadd:
	case Osub:
	case Omul:
	case Oand:
	case Oor:
	case Oxor:
	case Oneg:
		if (KBASE(k) == 1) {
			put(op, k, i->to, i->arg[0], i->arg[1]);
			return;
		}
		if (k == Kw) {
			put(op, Kw, i->to, lo(i->arg[0]), op == Oneg ? R : lo(i->arg[1]));
			return;
		}
		dest(i->to, &dl, &dh);
		switch (op) {
		case Oadd: add64(dl, dh, i->arg[0], i->arg[1]); break;
		case Osub: sub64(dl, dh, i->arg[0], i->arg[1]); break;
		case Omul:
			if (iscon(i->arg[1], &v) && (n = ulog2(v)) >= 0)
				shiftcon(Oshl, dl, dh, i->arg[0], n);
			else if (iscon(i->arg[0], &v) && (n = ulog2(v)) >= 0)
				shiftcon(Oshl, dl, dh, i->arg[1], n);
			else
				mul64(dl, dh, i->arg[0], i->arg[1]);
			break;
		case Oneg:
			sub64(dl, dh, CON_Z, i->arg[0]);
			break;
		default:
			put(op, Kw, dl, lo(i->arg[0]), lo(i->arg[1]));
			put(op, Kw, dh, hi(i->arg[0]), hi(i->arg[1]));
			break;
		}
		return;
	case Odiv:
	case Orem:
	case Oudiv:
	case Ourem:
		if (KBASE(k) == 1) {
			put(op, k, i->to, i->arg[0], i->arg[1]);
			return;
		}
		if ((op == Oudiv || op == Ourem) && iscon(i->arg[1], &v)
		&& (n = ulog2(k == Kl ? (uint64_t)v : (uint32_t)v)) >= 0) {
			if (k == Kw) {
				if (op == Oudiv)
					put(Oshr, Kw, i->to, lo(i->arg[0]), wcon(n));
				else
					put(Oand, Kw, i->to, lo(i->arg[0]), wcon(((int64_t)1 << n) - 1));
				return;
			}
			dest(i->to, &dl, &dh);
			if (op == Oudiv)
				shiftcon(Oshr, dl, dh, i->arg[0], n);
			else if (n < 32) {
				put(Oand, Kw, dl, lo(i->arg[0]), wcon(((int64_t)1 << n) - 1));
				put(Ocopy, Kw, dh, CON_Z, R);
			} else {
				put(Ocopy, Kw, dl, lo(i->arg[0]), R);
				put(Oand, Kw, dh, hi(i->arg[0]), wcon(((int64_t)1 << (n - 32)) - 1));
			}
			return;
		}
		if (k == Kw && !(T.w32ops & W32Div)) {
			put(op, Kw, i->to, lo(i->arg[0]), lo(i->arg[1]));
			return;
		}
		switch (op) {
		case Odiv: h = k == Kl ? "__luxir_divl" : "__luxir_divw"; break;
		case Orem: h = k == Kl ? "__luxir_reml" : "__luxir_remw"; break;
		case Oudiv: h = k == Kl ? "__luxir_udivl" : "__luxir_udivw"; break;
		default: h = k == Kl ? "__luxir_ureml" : "__luxir_uremw"; break;
		}
		if (k == Kl) {
			hi(i->arg[0]);
			hi(i->arg[1]);
			argpair(i->arg[0]);
			argpair(i->arg[1]);
		} else {
			put(Oarg, Kw, R, lo(i->arg[0]), R);
			put(Oarg, Kw, R, lo(i->arg[1]), R);
		}
		callhelp(h, k, i->to);
		return;
	case Osar:
	case Oshr:
	case Oshl:
		if (k == Kw) {
			/* konstantní posun mod 32 (assembler nepřijme větší) */
			if (iscon(i->arg[1], &v))
				put(op, Kw, i->to, lo(i->arg[0]), wcon(v & 31));
			else
				put(op, Kw, i->to, lo(i->arg[0]), lo(i->arg[1]));
			return;
		}
		dest(i->to, &dl, &dh);
		if (iscon(i->arg[1], &v))
			shiftcon(op, dl, dh, i->arg[0], v & 63);
		else
			shiftvar(op, dl, dh, i->arg[0], lo(i->arg[1]));
		return;
	case Ostoreb:
	case Ostoreh:
	case Ostorew:
		put(op, 0, R, lo(i->arg[0]), lo(i->arg[1]));
		return;
	case Ostorel:
		r = nw();
		put(Ostorew, 0, R, lo(i->arg[0]), lo(i->arg[1]));
		put(Oadd, Kw, r, lo(i->arg[1]), wcon(4));
		put(Ostorew, 0, R, hi(i->arg[0]), r);
		return;
	case Ostores:
	case Ostored:
		put(op, 0, R, i->arg[0], lo(i->arg[1]));
		return;
	case Oload:
	case Oloadsw:
	case Oloaduw:
		if (k != Kl) {
			put(Oload, k, i->to, lo(i->arg[0]), R);
			return;
		}
		dest(i->to, &dl, &dh);
		put(Oload, Kw, dl, lo(i->arg[0]), R);
		if (op == Oload) {
			r = nw();
			put(Oadd, Kw, r, lo(i->arg[0]), wcon(4));
			put(Oload, Kw, dh, r, R);
		} else if (op == Oloadsw)
			put(Osar, Kw, dh, dl, wcon(31));
		else
			put(Ocopy, Kw, dh, CON_Z, R);
		return;
	case Oloadsb:
	case Oloadub:
	case Oloadsh:
	case Oloaduh:
		if (k != Kl) {
			put(op, Kw, i->to, lo(i->arg[0]), R);
			return;
		}
		dest(i->to, &dl, &dh);
		put(op, Kw, dl, lo(i->arg[0]), R);
		if (op == Oloadsb || op == Oloadsh)
			put(Osar, Kw, dh, dl, wcon(31));
		else
			put(Ocopy, Kw, dh, CON_Z, R);
		return;
	case Oextsb:
	case Oextub:
	case Oextsh:
	case Oextuh:
		if (k != Kl) {
			put(op, Kw, i->to, lo(i->arg[0]), R);
			return;
		}
		dest(i->to, &dl, &dh);
		put(op, Kw, dl, lo(i->arg[0]), R);
		if (op == Oextsb || op == Oextsh)
			put(Osar, Kw, dh, dl, wcon(31));
		else
			put(Ocopy, Kw, dh, CON_Z, R);
		return;
	case Oextsw:
	case Oextuw:
		dest(i->to, &dl, &dh);
		put(Ocopy, Kw, dl, lo(i->arg[0]), R);
		if (op == Oextsw)
			put(Osar, Kw, dh, dl, wcon(31));
		else
			put(Ocopy, Kw, dh, CON_Z, R);
		return;
	case Oexts:
	case Otruncd:
		put(op, k, i->to, i->arg[0], R);
		return;
	case Ostosi:
	case Odtosi:
		if (k == Kw) {
			put(op, Kw, i->to, i->arg[0], R);
			return;
		}
		put(Oarg, op == Ostosi ? Ks : Kd, R, i->arg[0], R);
		callhelp(op == Ostosi ? "__luxir_stosl" : "__luxir_dtosl", Kl, i->to);
		return;
	case Ostoui:
	case Odtoui:
		if (k == Kw && !(T.w32ops & W32Ucvt)) {
			put(op, Kw, i->to, i->arg[0], R);
			return;
		}
		put(Oarg, op == Ostoui ? Ks : Kd, R, i->arg[0], R);
		if (k == Kw)
			h = op == Ostoui ? "__luxir_stouw" : "__luxir_dtouw";
		else
			h = op == Ostoui ? "__luxir_stoul" : "__luxir_dtoul";
		callhelp(h, k, i->to);
		return;
	case Oswtof:
		put(op, k, i->to, lo(i->arg[0]), R);
		return;
	case Ouwtof:
		if (!(T.w32ops & W32Ucvt)) {
			put(op, k, i->to, lo(i->arg[0]), R);
			return;
		}
		put(Oarg, Kw, R, lo(i->arg[0]), R);
		callhelp(k == Ks ? "__luxir_uwtos" : "__luxir_uwtod", k, i->to);
		return;
	case Osltof:
	case Oultof:
		argpair(i->arg[0]);
		if (op == Osltof)
			h = k == Ks ? "__luxir_sltos" : "__luxir_sltod";
		else
			h = k == Ks ? "__luxir_ultos" : "__luxir_ultod";
		callhelp(h, k, i->to);
		return;
	case Ocast:
		switch (k) {
		case Kw:
			put(op, Kw, i->to, i->arg[0], R);
			break;
		case Ks:
			put(op, Ks, i->to, lo(i->arg[0]), R);
			break;
		case Kl:
			dest(i->to, &dl, &dh);
			put(Odlo, Kw, dl, i->arg[0], R);
			put(Odhi, Kw, dh, i->arg[0], R);
			break;
		default:
			put(Omkd, Kd, i->to, lo(i->arg[0]), hi(i->arg[0]));
			break;
		}
		return;
	case Oalloc4:
	case Oalloc8:
	case Oalloc16:
		if (wide(i->to)) {
			dest(i->to, &dl, &dh);
			put(op, Kw, dl, lo(i->arg[0]), R);
			put(Ocopy, Kw, dh, CON_Z, R);
		} else
			put(op, Kw, i->to, lo(i->arg[0]), R);
		return;
	case Oblit0:
		if (i + 1 == end || (i+1)->op != Oblit1)
			die("w32: blit0 without blit1");
		blit(i->arg[0], i->arg[1], rsval((i+1)->arg[0]));
		return;
	case Oblit1:
		return;
	case Opar:
		if (wide(i->to)) {
			put(Opar, Kw, TMP(wlo[i->to.val]), R, INT(1));
			put(Opar, Kw, TMP(whi[i->to.val]), R, R);
		} else
			put(op, k, i->to, R, R);
		return;
	case Oarg:
		/* na začátku skupiny argumentů horní slova adres předem */
		if (i == start || !isarg((i-1)->op))
			for (j=i; j<end && isarg(j->op); j++)
				if (j->op == Oarg && j->cls == Kl)
					hi(j->arg[0]);
		if (k == Kl)
			argpair(i->arg[0]);
		else if (k == Kw)
			put(Oarg, Kw, R, lo(i->arg[0]), R);
		else
			put(Oarg, k, R, i->arg[0], R);
		return;
	case Oargv:
		put(op, k, R, R, R);
		return;
	case Ocall:
		if (!req(i->arg[1], R))
			die("w32: aggregate results are not supported");
		if (k == Kl) {
			if (wide(i->to)) {
				put(Ocall, Kl, TMP(wlo[i->to.val]), lo(i->arg[0]), R);
				put(Opairhi, Kw, TMP(whi[i->to.val]), R, R);
			} else
				put(Ocall, Kl, i->to, lo(i->arg[0]), R);
		} else
			put(Ocall, k, i->to, lo(i->arg[0]), R);
		return;
	default:
		break;
	}
	if (INRANGE(op, Ocmpw, Ocmpd1)) {
		/* výsledek 0/1 do slova, u třídy Kl s nulovou horní polovinou */
		if (k == Kl) {
			dest(i->to, &dl, &dh);
			r = dl;
			put(Ocopy, Kw, dh, CON_Z, R);
		} else
			r = req(i->to, R) ? nw() : i->to;
		if (INRANGE(op, Ocmpw, Ocmpw1))
			put(op, Kw, r, lo(i->arg[0]), lo(i->arg[1]));
		else if (INRANGE(op, Ocmpl, Ocmpl1)) {
			c = op - Ocmpl;
			cmp64(c, r, i->arg[0], i->arg[1]);
		} else
			put(op, Kw, r, i->arg[0], i->arg[1]);
		return;
	}
	if (isparbh(op) || isargbh(op))
		die("w32: sub-word arguments must be eliminated first");
	if (op == Ovastart || op == Ovaarg)
		die("w32: variadic functions are not supported");
	die("w32: unsupported operation %s", optab[op].name);
}

/* Porovnání Kl, jehož jediné použití je jnz na konci bloku: místo
 * aritmetiky větvení podle horních slov a pak dolních (bez znaménka).
 *
 *   b:  jnz (ah != bh), hb, lb      (eq/ne: jnz, F/T, lb)
 *   hb: jnz (ah <cond ostře> bh), T, F
 *   lb: jnz (al <cond bez znaménka> bl), T, F
 */
static Ins *fusedcmp;

static Blk *
addblk(Blk *after, char *tag)
{
	Blk *nb;

	nb = newblk();
	nb->visit = 1;
	nb->loop = after->loop;
	nb->name = strf(PFn, "%s.%s", after->name, tag);
	nb->link = after->link;
	after->link = nb;
	fn->nblk++;
	return nb;
}

/* phi v s: hrana z b se nahradí hranami ze všech nových předchůdců */
static void
repred(Blk *s, Blk *b, Blk **np, int nnp)
{
	Phi *p;
	uint n, m;
	int k, first;

	for (p=s->phi; p; p=p->link) {
		for (n=0; n<p->narg; n++)
			if (p->blk[n] == b)
				break;
		if (n == p->narg)
			continue;
		first = 1;
		for (k=0; k<nnp; k++) {
			if (np[k]->s1 != s && np[k]->s2 != s)
				continue;
			if (first) {
				p->blk[n] = np[k];
				first = 0;
				continue;
			}
			m = p->narg++;
			vgrow(&p->arg, p->narg);
			vgrow(&p->blk, p->narg);
			p->arg[m] = p->arg[n];
			p->blk[m] = np[k];
		}
	}
}

static void
cmpbranch(Blk *b, Ins *ic)
{
	Blk *T, *F, *hb, *lb, *np[3];
	Ref al, ah, bl, bh, hc, r;
	int c, sgn, base, strict, lop, eq;
	Ins ins;

	T = b->s1;
	F = b->s2;
	c = ic->op - Ocmpl;
	al = lo(ic->arg[0]), ah = hi(ic->arg[0]);
	bl = lo(ic->arg[1]), bh = hi(ic->arg[1]);
	eq = c == Cieq || c == Cine;
	if (eq)
		lop = c;
	else {
		sgn = c <= Cislt;
		base = c - (sgn ? Cisge : Ciuge);
		if (base <= 1)
			strict = sgn ? Cisgt : Ciugt;
		else
			strict = sgn ? Cislt : Ciult;
		lop = Ciuge + base;
	}
	if (iszero(ah) && iszero(bh)) {
		/* obě horní slova nulová: jen dolní */
		r = nw();
		put(Ocmpw+lop, Kw, r, al, bl);
		b->jmp.arg = r;
		return;
	}
	hc = nw();
	put(Ocmpw+Cine, Kw, hc, ah, bh);
	b->jmp.arg = hc;
	lb = addblk(b, "lo");
	r = nw();
	ins = (Ins){.op = Ocmpw+lop, .cls = Kw, .to = r, .arg = {al, bl}};
	idup(lb, &ins, 1);
	lb->jmp.type = Jjnz;
	lb->jmp.arg = r;
	lb->s1 = T;
	lb->s2 = F;
	if (eq) {
		/* horní slova se liší: eq nepravda, ne pravda */
		b->s1 = c == Cieq ? F : T;
		b->s2 = lb;
		np[0] = b;
		np[1] = lb;
		repred(T, b, np, 2);
		if (F != T)
			repred(F, b, np, 2);
		return;
	}
	hb = addblk(b, "hi");
	r = nw();
	ins = (Ins){.op = Ocmpw+strict, .cls = Kw, .to = r, .arg = {ah, bh}};
	idup(hb, &ins, 1);
	hb->jmp.type = Jjnz;
	hb->jmp.arg = r;
	hb->s1 = T;
	hb->s2 = F;
	b->s1 = hb;
	b->s2 = lb;
	np[0] = hb;
	np[1] = lb;
	repred(T, b, np, 2);
	if (F != T)
		repred(F, b, np, 2);
}

/* Kandidát: jnz na výsledek porovnání Kl ze stejného bloku bez dalšího
 * použití. */
static Ins *
findcmp(Blk *b)
{
	Ref r;
	Ins *i;
	Tmp *t;

	if (b->jmp.type != Jjnz || b->s1 == b->s2)
		return 0;
	r = b->jmp.arg;
	if (rtype(r) != RTmp || r.val < Tmp0)
		return 0;
	t = &fn->tmp[r.val];
	if (t->nuse != 1 || t->ndef != 1 || t->cls != Kw)
		return 0;
	for (i=&b->ins[b->nins]; i>b->ins;) {
		i--;
		if (req(i->to, r)) {
			if (INRANGE(i->op, Ocmpl, Ocmpl1) && i->cls == Kw)
				return i;
			return 0;
		}
	}
	return 0;
}

/* Horní slova adres s posunem v phi následníků se spočtou na konci b. */
static void
succphis(Blk *b, Blk *s)
{
	Phi *p;
	uint n;

	if (!s)
		return;
	for (p=s->phi; p; p=p->link)
		if (ishi[p->to.val])
			for (n=0; n<p->narg; n++)
				if (p->blk[n] == b && isaddroff(p->arg[n]))
					p->arg[n] = hi(p->arg[n]);
}

static void
lowerjmp(Blk *b)
{
	Ref r, o;

	succphis(b, b->s1);
	if (b->s2 != b->s1)
		succphis(b, b->s2);
	if (fusedcmp) {
		cmpbranch(b, fusedcmp);
		fusedcmp = 0;
		return;
	}

	r = b->jmp.arg;
	switch (b->jmp.type) {
	case Jjnz:
		if (wide(r) || rtype(r) == RCon) {
			o = nw();
			put(Oor, Kw, o, lo(r), hi(r));
			b->jmp.arg = o;
		}
		break;
	case Jretl:
		put(Opairhi, Kw, R, hi(r), R);
		b->jmp.arg = lo(r);
		break;
	case Jretw:
		b->jmp.arg = lo(r);
		break;
	case Jretc:
		die("w32: aggregate results are not supported");
	default:
		break;
	}
}

static void
lowerphis(Blk *b)
{
	Phi *p, *ph;
	uint n;

	for (p=b->phi; p; p=p->link) {
		if (!wide(p->to)) {
			if (p->cls == Kw)
				for (n=0; n<p->narg; n++)
					p->arg[n] = lo(p->arg[n]);
			continue;
		}
		ph = alloc(sizeof *ph);
		*ph = *p;
		ph->to = TMP(whi[p->to.val]);
		ph->cls = Kw;
		ph->arg = vnew(p->narg, sizeof ph->arg[0], PFn);
		ph->blk = vnew(p->narg, sizeof ph->blk[0], PFn);
		for (n=0; n<p->narg; n++) {
			/* horní slovo adresy s posunem dopočítá předchůdce
			 * (lowerjmp), zatím zůstává konstanta */
			if (isaddroff(p->arg[n]))
				ph->arg[n] = p->arg[n];
			else
				ph->arg[n] = hi(p->arg[n]);
			ph->blk[n] = p->blk[n];
			p->arg[n] = lo(p->arg[n]);
		}
		p->to = TMP(wlo[p->to.val]);
		p->cls = Kw;
		ph->link = p->link;
		p->link = ph;
		p = ph;
	}
}

/* Odstranění nepoužitých výpočtů: označí se hodnoty potřebné pro instrukce
 * s vedlejším účinkem a skoky, pak zmizí neoznačené čisté definice. */
static int
pure(int op)
{
	if (op == Ocall || isstore(op) || isarg(op) || ispar(op))
		return 0;
	if (op == Oblit0 || op == Oblit1 || op == Odbgloc || op == Osalloc)
		return 0;
	if (op == Ovastart || op == Ovaarg)
		return 0;
	return 1;
}

static void
dce(void)
{
	Blk *b;
	Ins *i;
	Phi *p;
	Ins **defi;
	Phi **defp;
	int *live, *stk, nstk, t, n;
	uint a;
	Ref *r;

	defi = emalloc(fn->ntmp * sizeof defi[0]);
	defp = emalloc(fn->ntmp * sizeof defp[0]);
	live = emalloc(fn->ntmp * sizeof live[0]);
	stk = emalloc((fn->ntmp + 1) * sizeof stk[0]);
	nstk = 0;
	for (b=fn->start; b; b=b->link) {
		for (p=b->phi; p; p=p->link)
			if (rtype(p->to) == RTmp)
				defp[p->to.val] = p;
		for (i=b->ins; i<&b->ins[b->nins]; i++)
			if (rtype(i->to) == RTmp)
				defi[i->to.val] = i;
	}
#define MARK(ref) \
	if (rtype(ref) == RTmp && (ref).val >= Tmp0 && !live[(ref).val]) { \
		live[(ref).val] = 1; \
		stk[nstk++] = (ref).val; \
	}
	for (b=fn->start; b; b=b->link) {
		MARK(b->jmp.arg);
		for (i=b->ins; i<&b->ins[b->nins]; i++)
			if (!pure(i->op) || req(i->to, R)) {
				MARK(i->to);
				MARK(i->arg[0]);
				MARK(i->arg[1]);
			}
	}
	while (nstk) {
		t = stk[--nstk];
		if (defi[t]) {
			MARK(defi[t]->arg[0]);
			MARK(defi[t]->arg[1]);
		}
		if (defp[t])
			for (a=0; a<defp[t]->narg; a++) {
				r = &defp[t]->arg[a];
				MARK(*r);
			}
	}
#undef MARK
	for (b=fn->start; b; b=b->link) {
		for (p=b->phi; p && !live[p->to.val];)
			p = p->link;
		b->phi = p;
		for (; p; p=p->link)
			while (p->link && !live[p->link->to.val])
				p->link = p->link->link;
		n = 0;
		for (i=b->ins; i<&b->ins[b->nins]; i++) {
			if (pure(i->op) && rtype(i->to) == RTmp
			&& i->to.val >= Tmp0 && !live[i->to.val])
				continue;
			b->ins[n++] = *i;
		}
		b->nins = n;
	}
	free(defi);
	free(defp);
	free(live);
	free(stk);
}

/* Identity po GVN (konstanty se objeví až propagací): instrukce se
 * změní na kopii, další gvn ji propaguje. */
static int
wsimpl(void)
{
	Blk *b;
	Ins *i;
	Ref r;
	int n;

	n = 0;
	for (b=fn->start; b; b=b->link)
		for (i=b->ins; i<&b->ins[b->nins]; i++)
			if (i->cls == Kw && rtype(i->to) == RTmp
			&& simplify(i->op, i->arg[0], i->arg[1], &r)) {
				i->op = Ocopy;
				i->arg[0] = r;
				i->arg[1] = R;
				n++;
			}
	return n;
}

/* gvn a gcm jako v hlavním průchodu (gvn ruší duplicity bez ohledu na
 * dominanci, správná místa dopočítá gcm) */
static void
wopt(void)
{
	fillcfg(fn);
	filluse(fn);
	filldom(fn);
	ssacheck(fn);
	gvn(fn);
	fillcfg(fn);
	simplcfg(fn);
	filluse(fn);
	filldom(fn);
	gcm(fn);
	filluse(fn);
	ssacheck(fn);
}

/* Sloučení přenosu: po add64/sub64 a optimalizacích
 *
 *   lo = add a, b;  c = cultw lo, a;  t = add ah, bh;  hi = add t, c
 *   lo = sub a, b;  c = cultw a, b;   t = sub ah, bh;  hi = sub t, c
 *
 * se nahradí dvojicí lo = addc a, b; hi = adc ah, bh (subc, sbc) na místě
 * hi. Přenos jde příznakem, proto musí být těsně za sebou; mezi ně pak
 * přidají další fáze jen přesuny (mov, ldr, str), které příznaky nemění.
 * Podmínky: vše v jednom bloku, c a t jen pro hi, lo nemá použití mezi
 * svou definicí a hi (přesune se dolů) a má nějaké použití (jinak by ho
 * výběr instrukcí zahodil). */
static Ins *
defin(Blk *b, Ref r, Ins **pos)
{
	Ins *i;

	if (rtype(r) != RTmp || r.val < Tmp0)
		return 0;
	for (i=b->ins; i<*pos; i++)
		if (req(i->to, r))
			return i;
	return 0;
}

static int
usedin(Ref r, Ins *i0, Ins *i1, Ins *skip)
{
	Ins *i;

	for (i=i0; i<i1; i++)
		if (i != skip && (req(i->arg[0], r) || req(i->arg[1], r)))
			return 1;
	return 0;
}

static void
wfuse(void)
{
	Blk *b;
	Ins *h, *ic, *il, *it, lo;
	Ref c, t, a0, a1;
	int add;

	for (b=fn->start; b; b=b->link)
		for (h=b->ins; h<&b->ins[b->nins]; h++) {
			if ((h->op != Oadd && h->op != Osub) || h->cls != Kw)
				continue;
			add = h->op == Oadd;
			/* c je druhý argument (u add i první) */
			c = h->arg[1];
			t = h->arg[0];
			ic = defin(b, c, &h);
			if ((!ic || ic->op != Ocmpw+Ciult) && add) {
				c = h->arg[0];
				t = h->arg[1];
				ic = defin(b, c, &h);
			}
			if (!ic || ic->op != Ocmpw+Ciult || fn->tmp[c.val].nuse != 1)
				continue;
			if (add) {
				il = defin(b, ic->arg[0], &h);
				if (!il || il->op != Oadd || il->cls != Kw)
					continue;
				if (!req(ic->arg[1], il->arg[0]) && !req(ic->arg[1], il->arg[1]))
					continue;
				if (fn->tmp[il->to.val].nuse < 2)
					continue;
			} else {
				for (il=b->ins; il<h; il++)
					if (il->op == Osub && il->cls == Kw
					&& req(il->arg[0], ic->arg[0]) && req(il->arg[1], ic->arg[1]))
						break;
				if (il == h || rtype(il->to) != RTmp || fn->tmp[il->to.val].nuse < 1)
					continue;
			}
			if (usedin(il->to, il+1, h, ic))
				continue;
			/* horní slovo: t = add/sub ah, bh se složí do adc/sbc */
			it = defin(b, t, &h);
			if (it && it->op == h->op && it->cls == Kw
			&& fn->tmp[t.val].nuse == 1) {
				a0 = it->arg[0];
				a1 = it->arg[1];
				*it = (Ins){.op = Onop};
			} else {
				a0 = t;
				a1 = CON_Z;
			}
			/* lo dolů těsně před hi (il < h) */
			*ic = (Ins){.op = Onop};
			/* instrukce mezi il a h o jednu nahoru, lo na h-1 */
			lo = *il;
			memmove(il, il+1, (h - il - 1) * sizeof *il);
			lo.op = add ? Oaddc : Osubc;
			h[-1] = lo;
			h->op = add ? Oadc : Osbc;
			h->arg[0] = a0;
			h->arg[1] = a1;
		}
}

void
w32split(Fn *f)
{
	Blk *b;
	Ins *i;
	Ref r;
	int t, n;

	fn = f;
	n = fn->ntmp;
	wlo = emalloc(n * sizeof wlo[0]);
	whi = emalloc(n * sizeof whi[0]);
	for (t=Tmp0; t<n; t++)
		if (wide(TMP(t))) {
			r = newtmp("lo", Kw, fn);
			wlo[t] = r.val;
			r = newtmp("hi", Kw, fn);
			whi[t] = r.val;
		}
	if (!out)
		out = vnew(0, sizeof out[0], PHeap);
	ishi = emalloc(fn->ntmp);
	for (t=Tmp0; t<n; t++)
		if (wide(TMP(t)))
			ishi[whi[t]] = 1;
	if (!hcache)
		hcache = vnew(0, sizeof hcache[0], PHeap);
	if (!subst)
		subst = vnew(0, sizeof subst[0], PHeap);
	nsubst = 0;
	for (b=fn->start; b; b=b->link)
		lowerphis(b);
	for (b=fn->start; b; b=b->link)
		b->visit = 0;
	for (b=fn->start; b; b=b->link) {
		if (b->visit)
			continue; /* nový blok z cmpbranch, už je nízký */
		nout = 0;
		nhcache = 0;
		fusedcmp = findcmp(b);
		for (i=b->ins; i<&b->ins[b->nins]; i++)
			if (i != fusedcmp)
				lowerins(i, b->ins, &b->ins[b->nins]);
		lowerjmp(b);
		idup(b, out, nout);
	}
	for (t=Tmp0; t<n; t++)
		if (wide(TMP(t)))
			fn->tmp[t].cls = Kw;
	free(wlo);
	free(whi);
	free(ishi);
	dce();
	if (debug['A']) {
		fprintf(stderr, "\n> After 64-bit splitting (before word optimization):\n");
		printfn(fn, stderr);
	}
	/* znovu optimalizace nad slovy: skládání konstant (add x, 0, horní
	 * slova konstant), kopie, společné podvýrazy (sar x, 31 u obou slov)
	 * a vytažení invariantů ze smyček */
	wopt();
	if (wsimpl())
		wopt();
	wfuse();
	filluse(fn);
	if (debug['A']) {
		fprintf(stderr, "\n> After 64-bit splitting:\n");
		printfn(fn, stderr);
	}
}

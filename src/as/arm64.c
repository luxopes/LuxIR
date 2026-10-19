#include "as.h"

/* Kodér AArch64 (A64) pro vestavěný assembler: instrukce, které vypisuje
 * emitor arm64 LuxIR a vrstvy architektury runtime LSL, kódování a výběr
 * aliasů (mov, cmp, lsl/lsr/asr, ldur) jako GNU as. Každá instrukce má
 * 4 bajty; skoky mají pevnou šířku, relaxace není třeba. Skoky na místní
 * návěští se vyřeší hned, na globální symboly zůstává relokace (jako
 * GNU as); adrp a :lo12: jsou vždy relokace. */

enum {
	ENone,
	EBr26,    /* b: imm26 */
	EBl26,    /* bl: imm26 */
	EBr19,    /* b.cond, cbz, cbnz: imm19 */
	EBr14,    /* tbz, tbnz: imm14 */
	EAdrp,    /* adrp: stránka */
	EAdd12,   /* add :lo12: */
	ELd8, ELd16, ELd32, ELd64, ELd128, /* ldr/str :lo12: */
};

enum {
	RX, RW, RSP, RWSP, RD, RS, RQ, RH, RB,
};

typedef struct AOp AOp;
struct AOp {
	int kind;      /* 0 nic, 1 registr, 2 immediate, 3 paměť, 4 symbol/výraz, 5 slovo (cond, bariéra) */
	int rk;        /* třída registru */
	int reg;
	int64_t imm;
	int shift;     /* lsl #n u immediate / registru (typ shift v stype) */
	int stype;     /* 0 lsl, 1 lsr, 2 asr */
	int hasshift;
	/* paměť */
	int base, index, idxshift, idxext;
	int64_t off;
	int pre, post;  /* [..]! / [..], #imm */
	int hasoff;
	AExpr e;        /* symbol: adrp, :lo12:, skoky */
	int lo12;       /* :lo12: modifikátor */
	char word[16];
};

static char *curmnem;

static void
aerr2(char *msg)
{
	aerr("%s: %s", curmnem, msg);
}

/* ---- rozbor ---- */

static int
parsereg(char **ps, AOp *o)
{
	char *p, *q;
	int n;

	p = *ps;
	o->kind = 1;
	if (strncmp(p, "sp", 2) == 0 && !isalnum((uchar)p[2])) {
		o->rk = RSP;
		o->reg = 31;
		*ps = p + 2;
		return 1;
	}
	if (strncmp(p, "wsp", 3) == 0 && !isalnum((uchar)p[3])) {
		o->rk = RWSP;
		o->reg = 31;
		*ps = p + 3;
		return 1;
	}
	if (strncmp(p, "xzr", 3) == 0 && !isalnum((uchar)p[3])) {
		o->rk = RX;
		o->reg = 31;
		*ps = p + 3;
		return 1;
	}
	if (strncmp(p, "wzr", 3) == 0 && !isalnum((uchar)p[3])) {
		o->rk = RW;
		o->reg = 31;
		*ps = p + 3;
		return 1;
	}
	if (strncmp(p, "lr", 2) == 0 && !isalnum((uchar)p[2])) {
		o->rk = RX;
		o->reg = 30;
		*ps = p + 2;
		return 1;
	}
	if (strncmp(p, "fp", 2) == 0 && !isalnum((uchar)p[2])) {
		o->rk = RX;
		o->reg = 29;
		*ps = p + 2;
		return 1;
	}
	switch (*p) {
	case 'x': o->rk = RX; break;
	case 'w': o->rk = RW; break;
	case 'd': o->rk = RD; break;
	case 's': o->rk = RS; break;
	case 'q': o->rk = RQ; break;
	case 'h': o->rk = RH; break;
	case 'b': o->rk = RB; break;
	default: return 0;
	}
	q = p + 1;
	if (!isdigit((uchar)*q))
		return 0;
	n = strtol(q, &q, 10);
	if (n < 0 || n > 31 || isalnum((uchar)*q) || *q == '_')
		return 0;
	if (n == 31 && (o->rk == RX || o->rk == RW))
		return 0;
	o->reg = n;
	*ps = q;
	return 1;
}

static int
isxw(AOp *o)
{
	return o->kind == 1 && (o->rk == RX || o->rk == RW);
}

static int
is64(AOp *o)
{
	return o->rk == RX || o->rk == RSP || o->rk == RD || o->rk == RQ;
}

/* číslo nebo výraz za '#' (nebo bez něj) */
static void
parseimm(char **ps, AOp *o)
{
	char *p;

	p = *ps;
	if (*p == '#')
		p++;
	aspace(&p);
	if (*p == ':') {
		/* :lo12:sym */
		if (strncmp(p, ":lo12:", 6) != 0)
			aerr2("unsupported relocation modifier");
		p += 6;
		o->kind = 4;
		o->lo12 = 1;
		if (!aexpr(&p, &o->e))
			aerr2("symbol expected");
		*ps = p;
		return;
	}
	o->kind = 2;
	if (!aexpr(&p, &o->e))
		aerr2("immediate expected");
	if (o->e.sym >= 0) {
		o->kind = 4;
	} else
		o->imm = o->e.add;
	*ps = p;
}

static int
shiftname(char *w)
{
	if (strcmp(w, "lsl") == 0) return 0;
	if (strcmp(w, "lsr") == 0) return 1;
	if (strcmp(w, "asr") == 0) return 2;
	if (strcmp(w, "uxtw") == 0) return 10;
	if (strcmp(w, "sxtw") == 0) return 11;
	if (strcmp(w, "sxtx") == 0) return 12;
	if (strcmp(w, "uxtx") == 0) return 13;
	return -1;
}

static void
parsemem(char **ps, AOp *o)
{
	char *p, *w;
	AOp t;
	int sh;

	p = *ps;
	o->kind = 3;
	o->index = -1;
	o->base = -1;
	if (*p != '[')
		aerr2("'[' expected");
	p++;
	aspace(&p);
	memset(&t, 0, sizeof t);
	if (!parsereg(&p, &t) || (t.rk != RX && t.rk != RSP))
		aerr2("base register expected");
	o->base = t.reg;
	aspace(&p);
	if (*p == ',') {
		p++;
		aspace(&p);
		if (*p == '#' || isdigit((uchar)*p) || *p == '-' || *p == ':') {
			parseimm(&p, &t);
			if (t.kind == 4) {
				o->e = t.e;
				o->lo12 = t.lo12;
				o->kind = 3;
				o->hasoff = 2; /* symbol */
			} else {
				o->off = t.imm;
				o->hasoff = 1;
			}
		} else {
			memset(&t, 0, sizeof t);
			if (!parsereg(&p, &t) || !isxw(&t))
				aerr2("index register expected");
			o->index = t.reg;
			o->idxext = t.rk == RW ? 10 : 13; /* uxtw : lsl */
			aspace(&p);
			if (*p == ',') {
				p++;
				w = aword(&p);
				sh = shiftname(w);
				if (sh < 0)
					aerr2("shift expected");
				if (sh == 0)
					sh = t.rk == RW ? 10 : 13;
				o->idxext = sh;
				aspace(&p);
				if (*p == '#') {
					p++;
					o->idxshift = strtol(p, &p, 10);
					o->hasshift = 1;
				}
			}
		}
		aspace(&p);
	}
	if (*p != ']')
		aerr2("']' expected");
	p++;
	if (*p == '!') {
		o->pre = 1;
		p++;
	}
	*ps = p;
}

static int
parseop(char **ps, AOp *o)
{
	char *p, *w;

	memset(o, 0, sizeof *o);
	o->e.sym = -1;
	o->index = -1;
	p = *ps;
	aspace(&p);
	if (!*p)
		return 0;
	if (*p == '[') {
		parsemem(&p, o);
	} else if (*p == '#' || isdigit((uchar)*p) || *p == '-' || *p == ':') {
		parseimm(&p, o);
	} else {
		w = p;
		if (parsereg(&p, o)) {
			/* registr s případným posunem ", lsl #n" řeší volající */
		} else {
			p = w;
			w = aword(&p);
			if (!*w)
				aerr2("operand expected");
			if (strlen(w) < sizeof o->word && (strcmp(w, "eq") == 0 || strcmp(w, "ne") == 0 || strcmp(w, "cs") == 0 || strcmp(w, "hs") == 0
			|| strcmp(w, "cc") == 0 || strcmp(w, "lo") == 0 || strcmp(w, "mi") == 0 || strcmp(w, "pl") == 0 || strcmp(w, "vs") == 0
			|| strcmp(w, "vc") == 0 || strcmp(w, "hi") == 0 || strcmp(w, "ls") == 0 || strcmp(w, "ge") == 0 || strcmp(w, "lt") == 0
			|| strcmp(w, "gt") == 0 || strcmp(w, "le") == 0 || strcmp(w, "al") == 0 || strcmp(w, "nv") == 0
			|| strcmp(w, "sy") == 0 || strcmp(w, "ish") == 0 || strcmp(w, "ishld") == 0 || strcmp(w, "ishst") == 0
			|| strcmp(w, "osh") == 0 || strcmp(w, "nsh") == 0 || strcmp(w, "ld") == 0 || strcmp(w, "st") == 0
			|| strcmp(w, "tpidr_el0") == 0 || strcmp(w, "tpidrro_el0") == 0 || strcmp(w, "fpcr") == 0 || strcmp(w, "fpsr") == 0 || strcmp(w, "nzcv") == 0
			|| strcmp(w, "ctr_el0") == 0 || strcmp(w, "dczid_el0") == 0 || strcmp(w, "cntvct_el0") == 0 || strcmp(w, "cntfrq_el0") == 0
			|| strcmp(w, "cvau") == 0 || strcmp(w, "civac") == 0 || strcmp(w, "cvac") == 0 || strcmp(w, "ivau") == 0)) {
				o->kind = 5;
				strcpy(o->word, w);
			} else {
				/* symbol (skok, adrp) */
				p = *ps;
				aspace(&p);
				o->kind = 4;
				if (!aexpr(&p, &o->e))
					aerr2("operand expected");
			}
		}
	}
	aspace(&p);
	/* ", lsl #n" za registrem nebo immediate */
	if (*p == ',' && o->kind != 3) {
		char *q = p + 1;
		aspace(&q);
		w = q;
		{
			char *ww = aword(&q);
			int sh = shiftname(ww);
			if (sh >= 0 && sh <= 2) {
				aspace(&q);
				if (*q != '#')
					aerr2("shift amount expected");
				q++;
				o->shift = strtol(q, &q, 10);
				o->stype = sh;
				o->hasshift = 1;
				p = q;
				aspace(&p);
			}
		}
		(void)w;
	}
	*ps = p;
	return 1;
}

static int
parseops(char *s, AOp *o, int max)
{
	int n;
	char *p;

	p = s;
	n = 0;
	for (;;) {
		aspace(&p);
		if (!*p)
			break;
		if (n == max)
			aerr2("too many operands");
		if (!parseop(&p, &o[n]))
			break;
		n++;
		aspace(&p);
		if (*p == ',')
			p++;
		else if (*p)
			aerr2("junk after operand");
	}
	return n;
}

/* ---- výstup ---- */

static void
put32(uint32_t w)
{
	uchar *b;

	b = aemit(4);
	if (!b)
		aerr2("instruction in a nobits section");
	b[0] = w;
	b[1] = w >> 8;
	b[2] = w >> 16;
	b[3] = w >> 24;
}

/* instrukce s opravou symbolu */
static void
put32fix(uint32_t w, int enc, AExpr *e, int branch)
{
	AItem *it;
	uint off;

	put32(w);
	it = &asecs[acursec].item[asecs[acursec].nitem - 1];
	off = it->size - 4;
	/* skoky se k místním návěštím vyřeší hned; adrp a :lo12: jsou vždy
	 * relokace (FAbs: symbol + addend bez P) */
	afixenc(off, 4, branch ? FBranch : FAbs, 0, enc, e);
}

static int
condcode(char *s)
{
	static char *names[] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv", 0};
	int n;

	if (strcmp(s, "hs") == 0)
		return 2;
	if (strcmp(s, "lo") == 0)
		return 3;
	for (n=0; names[n]; n++)
		if (strcmp(s, names[n]) == 0)
			return n;
	return -1;
}

/* Kódování logické immediate (N:immr:imms); 0 když nelze. */
static int
logimm(uint64_t v, int sf, uint32_t *out)
{
	int size, rot, ones, n, immr, imms;
	uint64_t mask, e;

	if (!sf) {
		if (v >> 32)
			return 0;
		v |= v << 32;
	}
	if (v == 0 || ~v == 0)
		return 0;
	/* nejmenší perioda */
	size = 64;
	for (n=32; n>=2; n/=2) {
		mask = ((uint64_t)1 << n) - 1;
		if ((v & mask) != ((v >> n) & mask))
			break;
		size = n;
	}
	mask = size == 64 ? ~(uint64_t)0 : (((uint64_t)1 << size) - 1);
	e = v & mask;
	/* rotace tak, aby jedničky byly souvislé od nuly */
	ones = 0;
	for (n=0; n<size; n++)
		ones += (e >> n) & 1;
	for (rot=0; rot<size; rot++) {
		uint64_t r = ((e >> rot) | (e << (size - rot))) & mask;
		uint64_t want = ones == 64 ? ~(uint64_t)0 : (((uint64_t)1 << ones) - 1);
		if (r == want)
			break;
	}
	if (rot == size)
		return 0;
	/* immr je rotace doprava základního vzoru: e = ROR(vzor, immr) */
	immr = (size - rot) % size;
	imms = ones - 1;
	/* imms: N=1 a 0xxxxx (64), 0xxxxx (32), 10xxxx (16), 110xxx (8),
	 * 1110xx (4), 11110x (2) */
	switch (size) {
	case 64: *out = (1 << 12) | (immr << 6) | imms; break;
	case 32: *out = (immr << 6) | imms; break;
	case 16: *out = (immr << 6) | (0x20 | imms); break;
	case 8:  *out = (immr << 6) | (0x30 | imms); break;
	case 4:  *out = (immr << 6) | (0x38 | imms); break;
	case 2:  *out = (immr << 6) | (0x3c | imms); break;
	default: return 0;
	}
	return 1;
}

static int
sfof(AOp *o)
{
	return is64(o) ? 1 : 0;
}

/* add/sub [s]: imm12 (lsl #12), shifted register; alias cmp/cmn/neg */
static void
addsub(int sub, int setflags, AOp *o, int no)
{
	int sf;
	uint32_t w;
	AOp *d, *n, *m;

	if (no == 3) {
		d = &o[0];
		n = &o[1];
		m = &o[2];
	} else
		aerr2("three operands expected");
	sf = sfof(d);
	if (m->kind == 2 || (m->kind == 4 && m->lo12)) {
		int64_t v = m->kind == 2 ? m->imm : 0;
		int sh = 0;
		if (m->hasshift) {
			if (m->shift == 12)
				sh = 1;
			else if (m->shift != 0)
				aerr2("invalid immediate shift");
		}
		if (m->kind == 2) {
			if (v < 0) {
				/* add -n = sub n */
				sub = !sub;
				v = -v;
			}
			if (!sh && v > 4095 && (v & 0xfff) == 0 && (v >> 12) <= 4095) {
				sh = 1;
				v >>= 12;
			}
			if (v < 0 || v > 4095)
				aerr2("immediate out of range");
		}
		w = (sub ? 0x51000000 : 0x11000000) | ((uint32_t)sf << 31) | ((uint32_t)setflags << 29) | ((uint32_t)sh << 22) | ((uint32_t)v << 10) | (n->reg << 5) | d->reg;
		if (m->kind == 4)
			put32fix(w, EAdd12, &m->e, 0);
		else
			put32(w);
		return;
	}
	if (m->kind != 1)
		aerr2("register or immediate expected");
	if (n->rk == RSP || n->rk == RWSP || d->rk == RSP || d->rk == RWSP) {
		/* rozšířený registr (uxtx / uxtw, lsl #0) kvůli sp */
		if (m->hasshift && m->stype != 0)
			aerr2("only lsl is allowed with sp");
		w = (sub ? 0x4b200000 : 0x0b200000) | ((uint32_t)sf << 31) | ((uint32_t)setflags << 29) | (m->reg << 16) | ((uint32_t)(sf ? 3 : 2) << 13) | ((uint32_t)(m->shift & 7) << 10) | (n->reg << 5) | d->reg;
		put32(w);
		return;
	}
	w = (sub ? 0x4b000000 : 0x0b000000) | ((uint32_t)sf << 31) | ((uint32_t)setflags << 29) | ((uint32_t)m->stype << 22) | (m->reg << 16) | ((uint32_t)(m->shift & 63) << 10) | (n->reg << 5) | d->reg;
	put32(w);
}

/* logické: and/orr/eor/bic s immediate (bitmask) nebo registrem */
static void
logic(int opc, int invert, AOp *o, int no)
{
	int sf;
	uint32_t w, enc;
	static uint32_t immop[] = {0x12000000, 0x32000000, 0x52000000, 0x72000000};
	static uint32_t regop[] = {0x0a000000, 0x2a000000, 0x4a000000, 0x6a000000};

	if (no != 3)
		aerr2("three operands expected");
	sf = sfof(&o[0]);
	if (o[2].kind == 2) {
		if (invert)
			aerr2("bic needs a register");
		if (!logimm((uint64_t)o[2].imm, sf, &enc))
			aerr2("immediate is not a valid bitmask");
		w = immop[opc] | ((uint32_t)sf << 31) | (enc << 10) | (o[1].reg << 5) | o[0].reg;
		put32(w);
		return;
	}
	if (o[2].kind != 1)
		aerr2("register or immediate expected");
	w = regop[opc] | ((uint32_t)sf << 31) | ((uint32_t)o[2].stype << 22) | ((uint32_t)invert << 21) | (o[2].reg << 16) | ((uint32_t)(o[2].shift & 63) << 10) | (o[1].reg << 5) | o[0].reg;
	put32(w);
}

/* mov Rd, #imm: movz, movn nebo orr s bitmask (jako GNU as) */
static void
movimm(AOp *d, int64_t v)
{
	int sf, hw, n;
	uint64_t u, inv;
	uint32_t enc;

	sf = sfof(d);
	u = sf ? (uint64_t)v : (uint32_t)v;
	inv = sf ? ~u : (uint32_t)~u;
	for (hw=0; hw<(sf ? 4 : 2); hw++)
		if ((u & ~((uint64_t)0xffff << (16 * hw))) == 0) {
			put32(0x52800000 | ((uint32_t)sf << 31) | (hw << 21) | ((uint32_t)((u >> (16 * hw)) & 0xffff) << 5) | d->reg);
			return;
		}
	for (hw=0; hw<(sf ? 4 : 2); hw++)
		if ((inv & ~((uint64_t)0xffff << (16 * hw))) == 0) {
			put32(0x12800000 | ((uint32_t)sf << 31) | (hw << 21) | ((uint32_t)((inv >> (16 * hw)) & 0xffff) << 5) | d->reg);
			return;
		}
	if (logimm(u, sf, &enc)) {
		put32(0x32000000 | ((uint32_t)sf << 31) | (enc << 10) | (31 << 5) | d->reg);
		return;
	}
	n = 0;
	(void)n;
	aerr2("immediate cannot be moved by a single instruction");
}

/* posuny o konstantu jako bitfield, o registr jako lslv/lsrv/asrv */
static void
shiftop(int kind, AOp *o, int no)
{
	int sf, bits, n;
	uint32_t w;

	if (no != 3)
		aerr2("three operands expected");
	sf = sfof(&o[0]);
	bits = sf ? 64 : 32;
	if (o[2].kind == 2) {
		n = o[2].imm;
		if (n < 0 || n >= bits)
			aerr2("shift out of range");
		switch (kind) {
		case 0: /* lsl: ubfm immr=(bits-n)%bits imms=bits-1-n */
			w = 0x53000000 | ((uint32_t)sf << 31) | ((uint32_t)sf << 22) | ((uint32_t)((bits - n) % bits) << 16) | ((uint32_t)(bits - 1 - n) << 10);
			break;
		case 1: /* lsr: ubfm immr=n imms=bits-1 */
			w = 0x53000000 | ((uint32_t)sf << 31) | ((uint32_t)sf << 22) | ((uint32_t)n << 16) | ((uint32_t)(bits - 1) << 10);
			break;
		default: /* asr: sbfm */
			w = 0x13000000 | ((uint32_t)sf << 31) | ((uint32_t)sf << 22) | ((uint32_t)n << 16) | ((uint32_t)(bits - 1) << 10);
			break;
		}
		put32(w | (o[1].reg << 5) | o[0].reg);
		return;
	}
	if (o[2].kind != 1)
		aerr2("register or immediate expected");
	w = (kind == 0 ? 0x1ac02000 : kind == 1 ? 0x1ac02400 : 0x1ac02800) | ((uint32_t)sf << 31) | (o[2].reg << 16) | (o[1].reg << 5) | o[0].reg;
	put32(w);
}

/* ldr/str a varianty: unsigned offset (scaled), ldur/stur, registr,
 * :lo12:, pre/post index */
static void
loadstore(int load, int size, int sign, AOp *o, int no)
{
	/* size: 0 b, 1 h, 2 w, 3 x, 4 q (log2 bajtů); sign: 1 ldrs* (cíl x nebo w) */
	AOp *t, *m;
	uint32_t w, opc, base, vbit, sz;
	int64_t off;
	int scale, enc;

	if (no != 2 && no != 3)
		aerr2("two operands expected");
	t = &o[0];
	m = &o[1];
	if (t->kind != 1 || m->kind != 3)
		aerr2("register and memory operand expected");
	vbit = (t->rk == RD || t->rk == RS || t->rk == RQ || t->rk == RH || t->rk == RB) ? 1 : 0;
	if (vbit) {
		switch (t->rk) {
		case RB: size = 0; break;
		case RH: size = 1; break;
		case RS: size = 2; break;
		case RD: size = 3; break;
		default: size = 4; break;
		}
	} else if (t->rk == RW && size == 3)
		size = 2;   /* ldr/str w: 32 bitů */
	scale = size;
	if (size == 4) {
		sz = 0;
		opc = load ? 3 : 2;
	} else {
		sz = size;
		opc = load ? 1 : 0;
	}
	if (sign) {
		/* ldrsb/ldrsh/ldrsw: opc 10 pro x, 11 pro w */
		opc = t->rk == RX ? 2 : 3;
	}
	base = (sz << 30) | (vbit << 26) | (opc << 22);
	/* post-index: [base], #imm jako třetí operand */
	if (no == 3) {
		if (o[2].kind != 2 || m->hasoff)
			aerr2("invalid post-index");
		off = o[2].imm;
		if (off < -256 || off > 255)
			aerr2("post-index out of range");
		w = base | 0x38000400 | ((uint32_t)(off & 0x1ff) << 12) | (m->base << 5) | t->reg;
		put32(w);
		return;
	}
	if (m->pre) {
		off = m->off;
		if (off < -256 || off > 255)
			aerr2("pre-index out of range");
		w = base | 0x38000c00 | ((uint32_t)(off & 0x1ff) << 12) | (m->base << 5) | t->reg;
		put32(w);
		return;
	}
	if (m->index >= 0) {
		int s = 0;
		if (m->hasshift) {
			if (m->idxshift != scale && m->idxshift != 0)
				aerr2("invalid index shift");
			s = m->idxshift == scale && scale > 0;
		}
		w = base | 0x38200800 | (m->index << 16) | ((uint32_t)(m->idxext == 13 ? 3 : m->idxext == 10 ? 2 : m->idxext == 11 ? 6 : 7) << 13) | ((uint32_t)s << 12) | (m->base << 5) | t->reg;
		put32(w);
		return;
	}
	if (m->hasoff == 2) {
		/* :lo12: */
		static int encs[] = {ELd8, ELd16, ELd32, ELd64, ELd128};
		enc = encs[size];
		w = base | 0x39000000 | (m->base << 5) | t->reg;
		put32fix(w, enc, &m->e, 0);
		return;
	}
	off = m->hasoff ? m->off : 0;
	if (off >= 0 && (off % (1 << scale)) == 0 && (off >> scale) <= 4095) {
		w = base | 0x39000000 | ((uint32_t)(off >> scale) << 10) | (m->base << 5) | t->reg;
		put32(w);
		return;
	}
	if (off < -256 || off > 255)
		aerr2("offset out of range");
	/* ldur/stur */
	w = base | 0x38000000 | ((uint32_t)(off & 0x1ff) << 12) | (m->base << 5) | t->reg;
	put32(w);
}

/* stp/ldp: signed offset, pre-index, post-index */
static void
pair(int load, AOp *o, int no)
{
	uint32_t w, opc, vbit;
	int scale;
	int64_t off;
	AOp *m;

	if (no != 3 && no != 4)
		aerr2("three operands expected");
	if (o[0].kind != 1 || o[1].kind != 1 || o[2].kind != 3)
		aerr2("two registers and memory expected");
	m = &o[2];
	vbit = 0;
	switch (o[0].rk) {
	case RX: opc = 2; scale = 3; break;
	case RW: opc = 0; scale = 2; break;
	case RD: opc = 1; scale = 3; vbit = 1; break;
	case RS: opc = 0; scale = 2; vbit = 1; break;
	case RQ: opc = 2; scale = 4; vbit = 1; break;
	default: aerr2("invalid pair register"); return;
	}
	if (no == 4) {
		if (o[3].kind != 2)
			aerr2("post-index expected");
		off = o[3].imm;
		w = 0x28800000;
	} else if (m->pre) {
		off = m->off;
		w = 0x29800000;
	} else {
		off = m->hasoff ? m->off : 0;
		w = 0x29000000;
	}
	if (off % (1 << scale) != 0)
		aerr2("pair offset is not a multiple of the register size");
	off >>= scale;
	if (off < -64 || off > 63)
		aerr2("pair offset out of range");
	w |= (opc << 30) | (vbit << 26) | ((uint32_t)load << 22) | ((uint32_t)(off & 0x7f) << 15) | (o[1].reg << 10) | (m->base << 5) | o[0].reg;
	put32(w);
}

static void
branch(int enc, uint32_t w, AOp *target)
{
	if (target->kind != 4)
		aerr2("branch target expected");
	put32fix(w, enc, &target->e, 1);
}

/* FP: typ podle registru (s 00, d 01) na bitech 22-23 */
static uint32_t
ftype(AOp *o)
{
	if (o->rk == RS)
		return 0;
	if (o->rk == RD)
		return 1u << 22;
	aerr2("floating-point register expected");
	return 0;
}

static void
fop3(uint32_t op, AOp *o, int no)
{
	if (no != 3)
		aerr2("three operands expected");
	put32(op | ftype(&o[0]) | (o[2].reg << 16) | (o[1].reg << 5) | o[0].reg);
}

static void
fop2(uint32_t op, AOp *o, int no)
{
	if (no != 2)
		aerr2("two operands expected");
	put32(op | ftype(&o[0]) | (o[1].reg << 5) | o[0].reg);
}

static int
barrier(char *w)
{
	if (strcmp(w, "sy") == 0) return 15;
	if (strcmp(w, "st") == 0) return 14;
	if (strcmp(w, "ld") == 0) return 13;
	if (strcmp(w, "ish") == 0) return 11;
	if (strcmp(w, "ishst") == 0) return 10;
	if (strcmp(w, "ishld") == 0) return 9;
	if (strcmp(w, "nsh") == 0) return 7;
	if (strcmp(w, "nshst") == 0) return 6;
	if (strcmp(w, "nshld") == 0) return 5;
	if (strcmp(w, "osh") == 0) return 3;
	if (strcmp(w, "oshst") == 0) return 2;
	if (strcmp(w, "oshld") == 0) return 1;
	return -1;
}

static uint32_t
sysreg(char *w)
{
	/* op0 op1 CRn CRm op2 -> bity 5-19 jako v mrs */
	if (strcmp(w, "tpidr_el0") == 0) return 0x5e82;   /* 3 3 13 0 2 */
	if (strcmp(w, "tpidrro_el0") == 0) return 0x5e83;
	if (strcmp(w, "fpcr") == 0) return 0x5a20;
	if (strcmp(w, "fpsr") == 0) return 0x5a21;
	if (strcmp(w, "nzcv") == 0) return 0x5a10;
	if (strcmp(w, "ctr_el0") == 0) return 0x5801;   /* 3 3 0 0 1 */
	if (strcmp(w, "dczid_el0") == 0) return 0x5807;
	if (strcmp(w, "cntvct_el0") == 0) return 0x5f02;
	if (strcmp(w, "cntfrq_el0") == 0) return 0x5f00;
	aerr2("unknown system register");
	return 0;
}

static int
arm64insn(char *mnem, char *ops)
{
	AOp o[5];
	int no, cc, sf;
	uint32_t w;

	curmnem = mnem;
	no = parseops(ops, o, 5);
	sf = no > 0 && o[0].kind == 1 ? sfof(&o[0]) : 0;

	if (strcmp(mnem, "add") == 0) addsub(0, 0, o, no);
	else if (strcmp(mnem, "adds") == 0) addsub(0, 1, o, no);
	else if (strcmp(mnem, "sub") == 0) addsub(1, 0, o, no);
	else if (strcmp(mnem, "subs") == 0) addsub(1, 1, o, no);
	else if (strcmp(mnem, "cmp") == 0 || strcmp(mnem, "cmn") == 0) {
		AOp t[3];
		if (no != 2)
			aerr2("two operands expected");
		t[0] = o[0];
		t[0].reg = 31;
		t[0].rk = o[0].rk == RW || o[0].rk == RWSP ? RW : RX;
		t[1] = o[0];
		t[2] = o[1];
		addsub(mnem[2] == 'p', 1, t, 3);
	}
	else if (strcmp(mnem, "neg") == 0) {
		AOp t[3];
		if (no != 2)
			aerr2("two operands expected");
		t[0] = o[0];
		t[1] = o[0];
		t[1].reg = 31;
		t[2] = o[1];
		addsub(1, 0, t, 3);
	}
	else if (strcmp(mnem, "and") == 0) logic(0, 0, o, no);
	else if (strcmp(mnem, "orr") == 0) logic(1, 0, o, no);
	else if (strcmp(mnem, "eor") == 0) logic(2, 0, o, no);
	else if (strcmp(mnem, "ands") == 0) logic(3, 0, o, no);
	else if (strcmp(mnem, "bic") == 0) logic(0, 1, o, no);
	else if (strcmp(mnem, "orn") == 0) logic(1, 1, o, no);
	else if (strcmp(mnem, "tst") == 0) {
		AOp t[3];
		if (no != 2)
			aerr2("two operands expected");
		t[0] = o[0];
		t[0].reg = 31;
		t[1] = o[0];
		t[2] = o[1];
		logic(3, 0, t, 3);
	}
	else if (strcmp(mnem, "mov") == 0) {
		if (no != 2)
			aerr2("two operands expected");
		if (o[1].kind == 2)
			movimm(&o[0], o[1].imm);
		else if (o[1].kind == 1) {
			if (o[0].rk == RSP || o[1].rk == RSP || o[0].rk == RWSP || o[1].rk == RWSP)
				put32(0x11000000 | ((uint32_t)sf << 31) | (o[1].reg << 5) | o[0].reg);
			else if (isxw(&o[0]) && isxw(&o[1]))
				put32(0x2a0003e0 | ((uint32_t)sf << 31) | (o[1].reg << 16) | o[0].reg);
			else
				aerr2("invalid mov registers");
		} else
			aerr2("invalid mov operand");
	}
	else if (strcmp(mnem, "movz") == 0 || strcmp(mnem, "movk") == 0 || strcmp(mnem, "movn") == 0) {
		int hw;
		if (no != 2 || o[1].kind != 2)
			aerr2("register and immediate expected");
		hw = o[1].hasshift ? o[1].shift / 16 : 0;
		if (o[1].hasshift && (o[1].shift % 16 != 0 || hw >= (sf ? 4 : 2)))
			aerr2("invalid shift");
		if (o[1].imm < 0 || o[1].imm > 0xffff)
			aerr2("immediate out of range");
		w = mnem[3] == 'z' ? 0x52800000 : mnem[3] == 'k' ? 0x72800000 : 0x12800000;
		put32(w | ((uint32_t)sf << 31) | ((uint32_t)hw << 21) | ((uint32_t)o[1].imm << 5) | o[0].reg);
	}
	else if (strcmp(mnem, "lsl") == 0) shiftop(0, o, no);
	else if (strcmp(mnem, "lsr") == 0) shiftop(1, o, no);
	else if (strcmp(mnem, "asr") == 0) shiftop(2, o, no);
	else if (strcmp(mnem, "mul") == 0) {
		if (no != 3) aerr2("three operands expected");
		put32(0x1b007c00 | ((uint32_t)sf << 31) | (o[2].reg << 16) | (o[1].reg << 5) | o[0].reg);
	}
	else if (strcmp(mnem, "madd") == 0 || strcmp(mnem, "msub") == 0) {
		if (no != 4) aerr2("four operands expected");
		put32((mnem[1] == 'a' ? 0x1b000000 : 0x1b008000) | ((uint32_t)sf << 31) | (o[2].reg << 16) | (o[3].reg << 10) | (o[1].reg << 5) | o[0].reg);
	}
	else if (strcmp(mnem, "udiv") == 0 || strcmp(mnem, "sdiv") == 0) {
		if (no != 3) aerr2("three operands expected");
		put32((mnem[0] == 'u' ? 0x1ac00800 : 0x1ac00c00) | ((uint32_t)sf << 31) | (o[2].reg << 16) | (o[1].reg << 5) | o[0].reg);
	}
	else if (strcmp(mnem, "sxtw") == 0) {
		if (no != 2) aerr2("two operands expected");
		put32(0x93407c00 | (o[1].reg << 5) | o[0].reg);
	}
	else if (strcmp(mnem, "sxtb") == 0 || strcmp(mnem, "sxth") == 0 || strcmp(mnem, "uxtb") == 0 || strcmp(mnem, "uxth") == 0) {
		int imms = mnem[3] == 'b' ? 7 : 15;
		if (no != 2) aerr2("two operands expected");
		if (mnem[0] == 's')
			put32(0x13001c00 | ((uint32_t)sf << 31) | ((uint32_t)sf << 22) | ((uint32_t)imms << 10) | (o[1].reg << 5) | o[0].reg);
		else
			put32(0x53001c00 | ((uint32_t)imms << 10) | (o[1].reg << 5) | o[0].reg);
	}
	else if (strcmp(mnem, "ubfx") == 0 || strcmp(mnem, "sbfx") == 0) {
		int lsb, width;
		if (no != 4 || o[2].kind != 2 || o[3].kind != 2) aerr2("invalid operands");
		lsb = o[2].imm;
		width = o[3].imm;
		put32((mnem[0] == 'u' ? 0x53000000 : 0x13000000) | ((uint32_t)sf << 31) | ((uint32_t)sf << 22) | ((uint32_t)lsb << 16) | ((uint32_t)(lsb + width - 1) << 10) | (o[1].reg << 5) | o[0].reg);
	}
	else if (strcmp(mnem, "cset") == 0) {
		if (no != 2 || o[1].kind != 5) aerr2("register and condition expected");
		cc = condcode(o[1].word);
		if (cc < 0) aerr2("unknown condition");
		put32(0x1a9f07e0 | ((uint32_t)sf << 31) | ((uint32_t)(cc ^ 1) << 12) | o[0].reg);
	}
	else if (strcmp(mnem, "csel") == 0 || strcmp(mnem, "csinc") == 0 || strcmp(mnem, "csinv") == 0 || strcmp(mnem, "csneg") == 0) {
		uint32_t op = strcmp(mnem, "csel") == 0 ? 0x1a800000 : strcmp(mnem, "csinc") == 0 ? 0x1a800400 : strcmp(mnem, "csinv") == 0 ? 0x5a800000 : 0x5a800400;
		if (no != 4 || o[3].kind != 5) aerr2("invalid operands");
		cc = condcode(o[3].word);
		put32(op | ((uint32_t)sf << 31) | (o[2].reg << 16) | ((uint32_t)cc << 12) | (o[1].reg << 5) | o[0].reg);
	}
	else if (strcmp(mnem, "b") == 0) branch(EBr26, 0x14000000, &o[0]);
	else if (strcmp(mnem, "bl") == 0) branch(EBl26, 0x94000000, &o[0]);
	else if ((mnem[0] == 'b' && mnem[1] == '.' && (cc = condcode(mnem + 2)) >= 0)
	|| (mnem[0] == 'b' && (cc = condcode(mnem + 1)) >= 0 && strcmp(mnem, "bl") != 0 && strcmp(mnem, "blr") != 0 && strcmp(mnem, "br") != 0)) {
		if (no != 1) aerr2("one operand expected");
		branch(EBr19, 0x54000000 | cc, &o[0]);
	}
	else if (strcmp(mnem, "cbz") == 0 || strcmp(mnem, "cbnz") == 0) {
		if (no != 2) aerr2("two operands expected");
		branch(EBr19, (mnem[2] == 'n' ? 0x35000000 : 0x34000000) | ((uint32_t)sf << 31) | o[0].reg, &o[1]);
	}
	else if (strcmp(mnem, "tbz") == 0 || strcmp(mnem, "tbnz") == 0) {
		int bit;
		if (no != 3 || o[1].kind != 2) aerr2("invalid operands");
		bit = o[1].imm;
		branch(EBr14, (mnem[2] == 'n' ? 0x37000000 : 0x36000000) | ((uint32_t)(bit >> 5) << 31) | ((uint32_t)(bit & 31) << 19) | o[0].reg, &o[2]);
	}
	else if (strcmp(mnem, "ret") == 0) put32(0xd65f0000 | ((no ? o[0].reg : 30) << 5));
	else if (strcmp(mnem, "br") == 0) put32(0xd61f0000 | (o[0].reg << 5));
	else if (strcmp(mnem, "blr") == 0) put32(0xd63f0000 | (o[0].reg << 5));
	else if (strcmp(mnem, "adrp") == 0) {
		if (no != 2 || o[1].kind != 4) aerr2("register and symbol expected");
		put32fix(0x90000000 | o[0].reg, EAdrp, &o[1].e, 0);
	}
	else if (strcmp(mnem, "adr") == 0) aerr2("adr is not supported");
	else if (strcmp(mnem, "ldr") == 0) loadstore(1, 3, 0, o, no);
	else if (strcmp(mnem, "str") == 0) loadstore(0, 3, 0, o, no);
	else if (strcmp(mnem, "ldrb") == 0) loadstore(1, 0, 0, o, no);
	else if (strcmp(mnem, "strb") == 0) loadstore(0, 0, 0, o, no);
	else if (strcmp(mnem, "ldrh") == 0) loadstore(1, 1, 0, o, no);
	else if (strcmp(mnem, "strh") == 0) loadstore(0, 1, 0, o, no);
	else if (strcmp(mnem, "ldrsb") == 0) loadstore(1, 0, 1, o, no);
	else if (strcmp(mnem, "ldrsh") == 0) loadstore(1, 1, 1, o, no);
	else if (strcmp(mnem, "ldrsw") == 0) {
		/* opc 10, size 10 */
		if (no != 2 || o[1].kind != 3) aerr2("invalid operands");
		o[0].rk = RX;
		{
			AOp m = o[1];
			int64_t off = m.hasoff == 1 ? m.off : 0;
			(void)m;
			if (m.hasoff == 2)
				put32fix(0xb9800000 | (m.base << 5) | o[0].reg, ELd32, &m.e, 0);
			else if (m.index >= 0)
				put32(0xb8a00800 | (m.index << 16) | ((uint32_t)(m.idxext == 13 ? 3 : m.idxext == 10 ? 2 : m.idxext == 11 ? 6 : 7) << 13) | ((uint32_t)(m.hasshift && m.idxshift == 2) << 12) | (m.base << 5) | o[0].reg);
			else if (off >= 0 && off % 4 == 0 && off / 4 <= 4095)
				put32(0xb9800000 | ((uint32_t)(off / 4) << 10) | (m.base << 5) | o[0].reg);
			else
				put32(0xb8800000 | ((uint32_t)(off & 0x1ff) << 12) | (m.base << 5) | o[0].reg);
		}
	}
	else if (strcmp(mnem, "ldp") == 0) pair(1, o, no);
	else if (strcmp(mnem, "stp") == 0) pair(0, o, no);
	else if (strcmp(mnem, "ldaxr") == 0 || strcmp(mnem, "ldxr") == 0 || strcmp(mnem, "ldar") == 0) {
		uint32_t op = strcmp(mnem, "ldaxr") == 0 ? 0x085ffc00 : strcmp(mnem, "ldxr") == 0 ? 0x085f7c00 : 0x08dffc00;
		if (no != 2 || o[1].kind != 3) aerr2("invalid operands");
		put32(op | ((uint32_t)(sf ? 3 : 2) << 30) | (o[1].base << 5) | o[0].reg);
	}
	else if (strcmp(mnem, "stlxr") == 0 || strcmp(mnem, "stxr") == 0) {
		uint32_t op = mnem[2] == 'l' ? 0x0800fc00 : 0x08007c00;
		if (no != 3 || o[2].kind != 3) aerr2("invalid operands");
		put32(op | ((uint32_t)(sfof(&o[1]) ? 3 : 2) << 30) | (o[0].reg << 16) | (o[2].base << 5) | o[1].reg);
	}
	else if (strcmp(mnem, "stlr") == 0) {
		if (no != 2 || o[1].kind != 3) aerr2("invalid operands");
		put32(0x089ffc00 | ((uint32_t)(sf ? 3 : 2) << 30) | (o[1].base << 5) | o[0].reg);
	}
	else if (strcmp(mnem, "fmov") == 0) {
		if (no != 2) aerr2("two operands expected");
		if (o[0].rk == RD && o[1].rk == RX) put32(0x9e670000 | (o[1].reg << 5) | o[0].reg);
		else if (o[0].rk == RX && o[1].rk == RD) put32(0x9e660000 | (o[1].reg << 5) | o[0].reg);
		else if (o[0].rk == RS && o[1].rk == RW) put32(0x1e270000 | (o[1].reg << 5) | o[0].reg);
		else if (o[0].rk == RW && o[1].rk == RS) put32(0x1e260000 | (o[1].reg << 5) | o[0].reg);
		else if (o[0].rk == RD && o[1].rk == RD) put32(0x1e604000 | (o[1].reg << 5) | o[0].reg);
		else if (o[0].rk == RS && o[1].rk == RS) put32(0x1e204000 | (o[1].reg << 5) | o[0].reg);
		else aerr2("unsupported fmov");
	}
	else if (strcmp(mnem, "fadd") == 0) fop3(0x1e202800, o, no);
	else if (strcmp(mnem, "fsub") == 0) fop3(0x1e203800, o, no);
	else if (strcmp(mnem, "fmul") == 0) fop3(0x1e200800, o, no);
	else if (strcmp(mnem, "fdiv") == 0) fop3(0x1e201800, o, no);
	else if (strcmp(mnem, "fneg") == 0) fop2(0x1e214000, o, no);
	else if (strcmp(mnem, "fabs") == 0) fop2(0x1e20c000, o, no);
	else if (strcmp(mnem, "fsqrt") == 0) fop2(0x1e21c000, o, no);
	else if (strcmp(mnem, "fcmpe") == 0 || strcmp(mnem, "fcmp") == 0) {
		if (no != 2) aerr2("two operands expected");
		put32((mnem[4] == 'e' ? 0x1e202010 : 0x1e202000) | ftype(&o[0]) | (o[1].reg << 16) | (o[0].reg << 5));
	}
	else if (strcmp(mnem, "fcvt") == 0) {
		if (no != 2) aerr2("two operands expected");
		if (o[0].rk == RD && o[1].rk == RS) put32(0x1e22c000 | (o[1].reg << 5) | o[0].reg);
		else if (o[0].rk == RS && o[1].rk == RD) put32(0x1e624000 | (o[1].reg << 5) | o[0].reg);
		else aerr2("unsupported fcvt");
	}
	else if (strcmp(mnem, "fcvtzs") == 0 || strcmp(mnem, "fcvtzu") == 0) {
		if (no != 2) aerr2("two operands expected");
		put32((mnem[5] == 's' ? 0x1e380000 : 0x1e390000) | ((uint32_t)sf << 31) | ftype(&o[1]) | (o[1].reg << 5) | o[0].reg);
	}
	else if (strcmp(mnem, "scvtf") == 0 || strcmp(mnem, "ucvtf") == 0) {
		if (no != 2) aerr2("two operands expected");
		put32((mnem[0] == 's' ? 0x1e220000 : 0x1e230000) | ((uint32_t)sfof(&o[1]) << 31) | ftype(&o[0]) | (o[1].reg << 5) | o[0].reg);
	}
	else if (strcmp(mnem, "dmb") == 0 || strcmp(mnem, "dsb") == 0) {
		int b = no == 1 && o[0].kind == 5 ? barrier(o[0].word) : -1;
		if (b < 0) aerr2("barrier option expected");
		put32((mnem[1] == 'm' ? 0xd50330bf : 0xd503309f) | ((uint32_t)b << 8));
	}
	else if (strcmp(mnem, "isb") == 0) put32(0xd5033fdf);
	else if (strcmp(mnem, "clrex") == 0) put32(0xd5033f5f);
	else if (strcmp(mnem, "dc") == 0 || strcmp(mnem, "ic") == 0) {
		/* údržba cache: sys #3, c7, CRm, op2, Xt */
		uint32_t crm, op2;
		if (no != 2 || o[0].kind != 5 || o[1].kind != 1)
			aerr2("operation and register expected");
		if (strcmp(o[0].word, "cvau") == 0) crm = 11, op2 = 1;
		else if (strcmp(o[0].word, "civac") == 0) crm = 14, op2 = 1;
		else if (strcmp(o[0].word, "cvac") == 0) crm = 10, op2 = 1;
		else if (strcmp(o[0].word, "ivau") == 0) crm = 5, op2 = 1;
		else { aerr2("unknown cache operation"); return 0; }
		put32(0xd50b7000 | (crm << 8) | (op2 << 5) | o[1].reg);
	}
	else if (strcmp(mnem, "mrs") == 0) {
		if (no != 2 || o[1].kind != 5) aerr2("register and system register expected");
		put32(0xd5300000 | (sysreg(o[1].word) << 5) | o[0].reg);
	}
	else if (strcmp(mnem, "msr") == 0) {
		if (no != 2 || o[0].kind != 5) aerr2("system register and register expected");
		put32(0xd5100000 | (sysreg(o[0].word) << 5) | o[1].reg);
	}
	else if (strcmp(mnem, "svc") == 0 || strcmp(mnem, "brk") == 0 || strcmp(mnem, "hlt") == 0) {
		if (no != 1 || o[0].kind != 2) aerr2("immediate expected");
		put32((mnem[0] == 's' ? 0xd4000001 : mnem[0] == 'b' ? 0xd4200000 : 0xd4400000) | ((uint32_t)(o[0].imm & 0xffff) << 5));
	}
	else if (strcmp(mnem, "hint") == 0) {
		if (no != 1 || o[0].kind != 2) aerr2("immediate expected");
		put32(0xd503201f | ((uint32_t)(o[0].imm & 0x7f) << 5));
	}
	else if (strcmp(mnem, "nop") == 0) put32(0xd503201f);
	else if (strcmp(mnem, "yield") == 0) put32(0xd503203f);
	else if (strcmp(mnem, "wfe") == 0) put32(0xd503205f);
	else if (strcmp(mnem, "sev") == 0) put32(0xd503209f);
	else if (strcmp(mnem, "bti") == 0) {
		int k = no == 1 && o[0].kind == 5 ? (strcmp(o[0].word, "c") == 0 ? 1 : strcmp(o[0].word, "j") == 0 ? 2 : 3) : 0;
		put32(0xd503241f | ((uint32_t)k << 6));
	}
	else
		return 0;
	return 1;
}

/* skoky mají pevnou šířku: IJump se nepoužívá */
static uint
a64jsize(AItem *i)
{
	(void)i;
	return 4;
}

static int
a64jfits(AItem *i, int64_t d)
{
	(void)i; (void)d;
	return 1;
}

static void
a64jemit(AItem *i, uchar *b, int64_t d, int r)
{
	(void)i; (void)b; (void)d; (void)r;
	die("arm64: unexpected jump item");
}

static void
a64nops(uchar *b, uint n)
{
	uint k;

	if (n % 4)
		aerr("alignment padding in code is not a multiple of 4");
	for (k=0; k<n; k+=4) {
		b[k] = 0x1f;
		b[k+1] = 0x20;
		b[k+2] = 0x03;
		b[k+3] = 0xd5;
	}
}

static int
a64reloc(AFix *f)
{
	switch (f->enc) {
	case ENone:
		if (f->kind == FAbs)
			return f->size == 8 ? 257 : f->size == 4 ? 258 : f->size == 2 ? 259 : 0;
		return f->size == 8 ? 260 : f->size == 4 ? 261 : 0;
	case EBr26: return 282;   /* JUMP26 */
	case EBl26: return 283;   /* CALL26 */
	case EBr19: return 280;   /* CONDBR19 */
	case EBr14: return 279;   /* TSTBR14 */
	case EAdrp: return 275;   /* ADR_PREL_PG_HI21 */
	case EAdd12: return 277;  /* ADD_ABS_LO12_NC */
	case ELd8: return 278;
	case ELd16: return 284;
	case ELd32: return 285;
	case ELd64: return 286;
	case ELd128: return 299;
	}
	return 0;
}

static void
a64fixenc(uchar *b, AFix *f, int64_t v)
{
	uint32_t w;

	w = b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
	if (v & 3)
		aerr("branch target is not aligned");
	v >>= 2;
	switch (f->enc) {
	case EBr26:
	case EBl26:
		if (v < -(1 << 25) || v >= (1 << 25))
			aerr("branch out of range");
		w |= (uint32_t)(v & 0x3ffffff);
		break;
	case EBr19:
		if (v < -(1 << 18) || v >= (1 << 18))
			aerr("branch out of range");
		w |= (uint32_t)(v & 0x7ffff) << 5;
		break;
	case EBr14:
		if (v < -(1 << 13) || v >= (1 << 13))
			aerr("branch out of range");
		w |= (uint32_t)(v & 0x3fff) << 5;
		break;
	default:
		aerr("cannot resolve relocation locally");
	}
	b[0] = w;
	b[1] = w >> 8;
	b[2] = w >> 16;
	b[3] = w >> 24;
}

AArch AARM64 = {
	.name = "arm64",
	.machine = 183,
	.elf64 = 1,
	.insn = arm64insn,
	.jsize = a64jsize,
	.jfits = a64jfits,
	.jemit = a64jemit,
	.nops = a64nops,
	.reloc = a64reloc,
	.fixenc = a64fixenc,
	.resolveglobal = 0,
	.minalign = 4,
	.comment = 0,
	.wordsize = 4,
	.keeplocalsym = 1,
};

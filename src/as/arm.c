#include "as.h"

/* Kodér ARM (A32, ARMv7-A s VFPv3-D16) pro vestavěný assembler: unified
 * syntax GNU as, instrukce, které vypisuje emitor arm LuxIR a vrstva
 * architektury runtime LSL (linux_arm.S), kódování a náhrady jako GNU as
 * (mov s nekódovatelnou konstantou do 16 bitů jako movw, mov/mvn,
 * and/bic, add/sub, adc/sbc, cmp/cmn se zápornou konstantou). Skoky mají
 * dosah ±32 MiB, relaxace není třeba; skoky na místní symboly téže sekce
 * se vyřeší, na globální zůstává relokace (R_ARM_CALL / R_ARM_JUMP24
 * s addendem -8 v poli). movw/movt #:lower16:/#:upper16: mají relokaci
 * vždy na symbol (i .L), data přes symbol sekce. Objekt je REL. */

enum {
	ENone,
	EBranch,  /* b<cc>: R_ARM_JUMP24 */
	ECall,    /* bl: R_ARM_CALL */
	EMovw,    /* R_ARM_MOVW_ABS_NC */
	EMovt,    /* R_ARM_MOVT_ABS */
	ELdrLit,  /* ldr Rd, návěští: [pc, #±imm12], jen místně */
	EVldrLit, /* vldr: [pc, #±imm8*4], jen místně */
};

typedef struct AOp AOp;
struct AOp {
	int kind;      /* 0 nic, 1 reg r, 2 reg s, 3 reg d, 4 výraz/#imm, 5 paměť, 6 seznam, 7 slovo, 8 posun (lsl #n …) */
	int reg;
	AExpr e;
	int imm;       /* kind 4: začíná '#' */
	int mod;       /* 1 :lower16:, 2 :upper16: */
	int isfloat;
	double fval;
	/* paměť */
	int base, idx, idxneg, pre, post, wb;
	int shtype, shamt, shreg;  /* posun indexu / kind 8 */
	int64_t off;
	int hasoff;
	/* seznam registrů */
	uint32_t list;
	int listkind;  /* 1 r, 2 s, 3 d */
	int listlo, listn;
	char word[16];
};

static char *curmnem;
static int curcond;

static void
merr(char *msg)
{
	aerr("%s: %s", curmnem, msg);
}

/* ---- registry ---- */

static int
regname(char *w, int *kind, int *reg)
{
	static struct { char *n; int r; } alias[] = {
		{"sp", 13}, {"lr", 14}, {"pc", 15}, {"fp", 11}, {"ip", 12}, {"sl", 10},
		{"sb", 9}, {"a1", 0}, {"a2", 1}, {"a3", 2}, {"a4", 3},
		{"v1", 4}, {"v2", 5}, {"v3", 6}, {"v4", 7}, {"v5", 8}, {"v6", 9}, {"v7", 10}, {"v8", 11},
		{0, 0},
	};
	int n;
	char *end;

	if ((w[0] == 'r' || w[0] == 's' || w[0] == 'd') && isdigit((uchar)w[1])) {
		n = strtol(w + 1, &end, 10);
		if (*end == 0 && n >= 0 && n < 32 && (w[0] != 'r' || n < 16)) {
			*kind = w[0] == 'r' ? 1 : w[0] == 's' ? 2 : 3;
			*reg = n;
			return 1;
		}
	}
	for (n=0; alias[n].n; n++)
		if (strcmp(w, alias[n].n) == 0) {
			*kind = 1;
			*reg = alias[n].r;
			return 1;
		}
	return 0;
}

static char *conds[] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", 0};

static int
condcode(char *s)
{
	int n;

	if (strcmp(s, "hs") == 0)
		return 2;
	if (strcmp(s, "lo") == 0)
		return 3;
	for (n=0; conds[n]; n++)
		if (strcmp(s, conds[n]) == 0)
			return n;
	return -1;
}

static int
shiftname(char *w)
{
	if (strcmp(w, "lsl") == 0) return 0;
	if (strcmp(w, "lsr") == 0) return 1;
	if (strcmp(w, "asr") == 0) return 2;
	if (strcmp(w, "ror") == 0) return 3;
	if (strcmp(w, "rrx") == 0) return 4;
	return -1;
}

/* ---- operandy ---- */

static void
parseshift(char **ps, AOp *o)
{
	char *p, *q, w[8];
	int n, k, r;
	int64_t v;

	p = *ps;
	aspace(&p);
	q = p;
	while (isalpha((uchar)*q))
		q++;
	n = q - p;
	if (n != 3 || n >= (int)sizeof w)
		merr("shift expected");
	memcpy(w, p, n);
	w[n] = 0;
	o->shtype = shiftname(w);
	if (o->shtype < 0)
		merr("shift expected");
	p = q;
	aspace(&p);
	o->shreg = -1;
	o->shamt = 0;
	if (o->shtype == 4) {
		/* rrx = ror #0 */
		o->shtype = 3;
	} else if (*p == '#') {
		p++;
		if (!anumber(&p, &v))
			merr("shift amount expected");
		if (v < 0 || v > 32)
			merr("shift amount out of range");
		if (v == 32 && (o->shtype == 1 || o->shtype == 2))
			v = 0;
		else if (v == 32)
			merr("shift amount out of range");
		o->shamt = v;
	} else {
		q = p;
		while (identchar((uchar)*q))
			q++;
		n = q - p;
		if (n <= 0 || n >= (int)sizeof w)
			merr("shift register expected");
		memcpy(w, p, n);
		w[n] = 0;
		if (!regname(w, &k, &r) || k != 1)
			merr("shift register expected");
		o->shreg = r;
		p = q;
	}
	*ps = p;
}

static void
parselist(char *p, AOp *o)
{
	char *q, w[8];
	int n, k, r, lo, hi, kind, k2, r2;

	o->kind = 6;
	o->list = 0;
	o->listkind = 0;
	o->listlo = -1;
	o->listn = 0;
	aspace(&p);
	if (*p != '{')
		merr("'{' expected");
	p++;
	for (;;) {
		aspace(&p);
		q = p;
		while (identchar((uchar)*q))
			q++;
		n = q - p;
		if (n <= 0 || n >= (int)sizeof w)
			merr("register expected in list");
		memcpy(w, p, n);
		w[n] = 0;
		if (!regname(w, &kind, &r))
			merr("register expected in list");
		p = q;
		aspace(&p);
		lo = hi = r;
		if (*p == '-') {
			p++;
			aspace(&p);
			q = p;
			while (identchar((uchar)*q))
				q++;
			n = q - p;
			if (n <= 0 || n >= (int)sizeof w)
				merr("register expected in list");
			memcpy(w, p, n);
			w[n] = 0;
			if (!regname(w, &k2, &r2) || k2 != kind || r2 < lo)
				merr("bad register range");
			hi = r2;
			p = q;
			aspace(&p);
		}
		if (o->listkind && o->listkind != kind)
			merr("mixed register list");
		o->listkind = kind;
		for (k=lo; k<=hi; k++) {
			if (kind == 1)
				o->list |= 1u << k;
			else {
				if (o->listlo < 0)
					o->listlo = k;
				else if (k != o->listlo + o->listn)
					merr("register list is not consecutive");
				o->listn++;
			}
		}
		if (*p == ',') {
			p++;
			continue;
		}
		if (*p == '}') {
			p++;
			break;
		}
		merr("',' or '}' expected in list");
	}
	aspace(&p);
	if (*p)
		merr("junk after register list");
}

static void
parsemem(char *p, AOp *o)
{
	char *q, w[8];
	int n, k, r;

	o->kind = 5;
	o->base = o->idx = -1;
	o->shreg = -1;
	o->pre = o->post = o->wb = 0;
	o->off = 0;
	o->hasoff = 0;
	o->idxneg = 0;
	o->shtype = 0;
	o->shamt = 0;
	if (*p != '[')
		merr("'[' expected");
	p++;
	aspace(&p);
	q = p;
	while (identchar((uchar)*q))
		q++;
	n = q - p;
	if (n <= 0 || n >= (int)sizeof w)
		merr("base register expected");
	memcpy(w, p, n);
	w[n] = 0;
	if (!regname(w, &k, &r) || k != 1)
		merr("base register expected");
	o->base = r;
	p = q;
	aspace(&p);
	if (*p == ',') {
		p++;
		aspace(&p);
		o->pre = 1;
		if (*p == '#') {
			p++;
			if (!aexpr(&p, &o->e) || o->e.sym >= 0)
				merr("offset expected");
			o->off = o->e.add;
			o->hasoff = 1;
		} else {
			if (*p == '-') {
				o->idxneg = 1;
				p++;
			} else if (*p == '+')
				p++;
			q = p;
			while (identchar((uchar)*q))
				q++;
			n = q - p;
			if (n <= 0 || n >= (int)sizeof w)
				merr("index register expected");
			memcpy(w, p, n);
			w[n] = 0;
			if (!regname(w, &k, &r) || k != 1)
				merr("index register expected");
			o->idx = r;
			p = q;
			aspace(&p);
			if (*p == ',') {
				p++;
				parseshift(&p, o);
			}
		}
		aspace(&p);
	}
	if (*p != ']')
		merr("']' expected");
	p++;
	aspace(&p);
	if (*p == '!') {
		o->wb = 1;
		p++;
		aspace(&p);
	}
	if (*p == ',') {
		/* post-index: [Rn], #imm / Rm */
		if (o->pre)
			merr("invalid addressing mode");
		o->post = 1;
		p++;
		aspace(&p);
		if (*p == '#') {
			p++;
			if (!aexpr(&p, &o->e) || o->e.sym >= 0)
				merr("offset expected");
			o->off = o->e.add;
			o->hasoff = 1;
		} else {
			if (*p == '-') {
				o->idxneg = 1;
				p++;
			}
			q = p;
			while (identchar((uchar)*q))
				q++;
			n = q - p;
			if (n <= 0 || n >= (int)sizeof w)
				merr("index register expected");
			memcpy(w, p, n);
			w[n] = 0;
			if (!regname(w, &k, &r) || k != 1)
				merr("index register expected");
			o->idx = r;
			p = q;
		}
		aspace(&p);
	}
	if (!o->pre && !o->post)
		o->pre = 1;   /* [Rn] = [Rn, #0] */
	if (*p)
		merr("junk after memory operand");
}

static void
parseop(char *s, AOp *o)
{
	char *p, *q, w[32];
	int k, r, n;

	memset(o, 0, sizeof *o);
	o->e.sym = -1;
	o->shreg = -1;
	p = s;
	aspace(&p);
	if (*p == '[') {
		parsemem(p, o);
		return;
	}
	if (*p == '{') {
		parselist(p, o);
		return;
	}
	if (*p == '#') {
		p++;
		o->kind = 4;
		o->imm = 1;
		aspace(&p);
		if (*p == ':') {
			if (strncmp(p, ":lower16:", 9) == 0)
				o->mod = 1;
			else if (strncmp(p, ":upper16:", 9) == 0)
				o->mod = 2;
			else
				merr("unsupported immediate modifier");
			p += 9;
			if (!aexpr(&p, &o->e) || o->e.sym < 0)
				merr("symbol expected");
			aspace(&p);
			if (*p)
				merr("junk after operand");
			return;
		}
		/* plovoucí konstanta (vmov, vcmp): obsahuje '.' nebo 'e' bez 0x */
		q = p;
		if (*q == '-' || *q == '+')
			q++;
		if (isdigit((uchar)*q) && !(q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) && (strchr(q, '.') || strchr(q, 'e'))) {
			char *end;
			o->fval = strtod(p, &end);
			o->isfloat = 1;
			p = end;
			aspace(&p);
			if (*p)
				merr("junk after operand");
			return;
		}
		if (!aexpr(&p, &o->e))
			merr("immediate expected");
		aspace(&p);
		if (*p)
			merr("junk after operand");
		return;
	}
	/* registr, slovo nebo posun */
	q = p;
	while (identchar((uchar)*q))
		q++;
	n = q - p;
	if (n > 0 && n < (int)sizeof w) {
		char *r2 = q;
		memcpy(w, p, n);
		w[n] = 0;
		aspace(&r2);
		if ((*r2 == 0 || (*r2 == '!' && r2[1] == 0)) && regname(w, &k, &r)) {
			o->kind = k;
			o->reg = r;
			o->wb = *r2 == '!';   /* Rn! u ldm/stm */
			return;
		}
		if (shiftname(w) >= 0) {
			o->kind = 8;
			parseshift(&p, o);
			aspace(&p);
			if (*p)
				merr("junk after shift");
			return;
		}
		if (*r2 == 0 && (strcmp(w, "APSR_nzcv") == 0 || strcmp(w, "fpscr") == 0 || strcmp(w, "FPSCR") == 0
		|| strcmp(w, "cpsr") == 0 || strcmp(w, "CPSR") == 0 || strcmp(w, "ish") == 0 || strcmp(w, "sy") == 0
		|| strcmp(w, "ishst") == 0 || strcmp(w, "st") == 0 || strcmp(w, "nsh") == 0 || strcmp(w, "osh") == 0
		|| strcmp(w, "oshst") == 0 || strcmp(w, "nshst") == 0
		|| ((w[0] == 'p' || w[0] == 'c') && isdigit((uchar)w[1]) && n <= 3))) {
			o->kind = 7;
			strcpy(o->word, w);
			return;
		}
	}
	if (!aexpr(&p, &o->e))
		merr("invalid operand");
	aspace(&p);
	if (*p)
		merr("junk after operand");
	o->kind = 4;
}

static int
parseops(char *s, AOp *o, int max)
{
	int n, depth, closed;
	char *p, *start;

	n = 0;
	aspace(&s);
	if (!*s)
		return 0;
	start = s;
	depth = 0;
	closed = 0;
	for (p=s; ; p++) {
		if (*p == '[' || *p == '{')
			depth++;
		else if (*p == ']' || *p == '}') {
			depth--;
			/* za ']' patří k operandu ještě post-index: [Rn], #imm / Rm */
			closed = *p == ']';
		} else if (*p == ',' && depth == 0 && closed) {
			closed = 0;
			continue;
		} else if (*p != ' ' && *p != '\t' && *p != '!')
			closed = 0;
		if ((*p == ',' && depth == 0) || *p == 0) {
			if (n >= max)
				merr("too many operands");
			if (*p) {
				*p = 0;
				parseop(start, &o[n++]);
				start = p + 1;
			} else {
				parseop(start, &o[n++]);
				break;
			}
		}
	}
	return n;
}

/* ---- zápis ---- */

static void
put32(uint32_t w)
{
	uchar *b;

	b = aemit(4);
	if (!b)
		merr("instruction in a nobits section");
	b[0] = w;
	b[1] = w >> 8;
	b[2] = w >> 16;
	b[3] = w >> 24;
}

static void
put32fix(uint32_t w, int kind, int64_t pcoff, int enc, AExpr *e)
{
	AItem *it;
	uint off;

	put32(w);
	it = &asecs[acursec].item[asecs[acursec].nitem - 1];
	off = it->size - 4;
	afixenc(off, 4, kind, pcoff, enc, e);
}

static uint32_t
cc(void)
{
	return (uint32_t)curcond << 28;
}

/* modifikovaná konstanta: imm8 rotovaná doprava o 2*rot; -1 nelze */
static int
armimm(uint32_t v)
{
	int i;
	uint32_t a;

	if (v <= 0xff)
		return v;
	for (i=2; i<32; i+=2) {
		a = (v << i) | (v >> (32 - i));
		if (a <= 0xff)
			return a | (i << 7);
	}
	return -1;
}

static int
xreg(AOp *o)
{
	if (o->kind != 1)
		merr("core register expected");
	return o->reg;
}

static int
sreg(AOp *o)
{
	if (o->kind != 2)
		merr("single-precision register expected");
	return o->reg;
}

static int
dreg(AOp *o)
{
	if (o->kind != 3)
		merr("double-precision register expected");
	return o->reg;
}

static int64_t
constant(AOp *o)
{
	if (o->kind != 4 || o->e.sym >= 0 || o->isfloat)
		merr("constant expected");
	return o->e.add;
}

/* operand2: #imm (kódovaný), Rm, Rm s posunem; vrací 12 bitů a bit I */
static uint32_t
op2(AOp *o, int no, int idx, int *immneg, uint32_t *negop)
{
	int64_t v;
	int e;
	uint32_t w;

	(void)negop;
	if (o[idx].kind == 4) {
		v = constant(&o[idx]);
		e = armimm((uint32_t)v);
		if (e < 0) {
			*immneg = 1;
			return 0;
		}
		return (1u << 25) | (uint32_t)e;
	}
	w = xreg(&o[idx]);
	if (no > idx + 1) {
		if (o[idx+1].kind != 8)
			merr("shift expected");
		if (o[idx+1].shreg >= 0)
			w |= ((uint32_t)o[idx+1].shreg << 8) | ((uint32_t)o[idx+1].shtype << 5) | 0x10;
		else
			w |= ((uint32_t)o[idx+1].shamt << 7) | ((uint32_t)o[idx+1].shtype << 5);
	}
	return w;
}

/* datové instrukce: opcode 0-15, S, Rd, Rn, operand2 na indexu idx */
static void
dataproc(int opc, int sflag, int rd, int rn, AOp *o, int no, int idx)
{
	int immneg, e, alt;
	int64_t v;
	uint32_t w;

	immneg = 0;
	w = op2(o, no, idx, &immneg, 0);
	if (immneg) {
		/* záporná náhrada jako GNU as: mov/mvn a and/bic ~v, adc/sbc ~v,
		 * add/sub a cmp/cmn -v; mov do 16 bitů jako movw */
		v = constant(&o[idx]);
		alt = -1;
		e = -1;
		switch (opc) {
		case 13: alt = 15; e = armimm(~(uint32_t)v); break;
		case 15: alt = 13; e = armimm(~(uint32_t)v); break;
		case 0: alt = 14; e = armimm(~(uint32_t)v); break;
		case 14: alt = 0; e = armimm(~(uint32_t)v); break;
		case 5: alt = 6; e = armimm(~(uint32_t)v); break;
		case 6: alt = 5; e = armimm(~(uint32_t)v); break;
		case 4: alt = 2; e = armimm(-(uint32_t)v); break;
		case 2: alt = 4; e = armimm(-(uint32_t)v); break;
		case 10: alt = 11; e = armimm(-(uint32_t)v); break;
		case 11: alt = 10; e = armimm(-(uint32_t)v); break;
		}
		if (e >= 0)
			opc = alt;
		else if (opc == 13 && !sflag && v >= 0 && v <= 0xffff) {
			put32(cc() | 0x03000000 | (((uint32_t)v >> 12) << 16) | ((uint32_t)rd << 12) | ((uint32_t)v & 0xfff));
			return;
		} else
			merr("invalid constant");
		w = (1u << 25) | (uint32_t)e;
	}
	put32(cc() | ((uint32_t)opc << 21) | ((uint32_t)sflag << 20) | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | w);
}

/* ---- tabulky ---- */

static struct {
	char *name;
	int opc;
	int form;   /* 0 Rd,Rn,op2; 1 Rd,op2 (mov/mvn); 2 Rn,op2 (cmp…) */
} dops[] = {
	{"and", 0, 0}, {"eor", 1, 0}, {"sub", 2, 0}, {"rsb", 3, 0}, {"add", 4, 0},
	{"adc", 5, 0}, {"sbc", 6, 0}, {"rsc", 7, 0}, {"tst", 8, 2}, {"teq", 9, 2},
	{"cmp", 10, 2}, {"cmn", 11, 2}, {"orr", 12, 0}, {"mov", 13, 1}, {"bic", 14, 0},
	{"mvn", 15, 1},
	{0, 0, 0},
};

/* mnemoniky, které mohou mít příponu podmínky a případně s */
static struct {
	char *name;
	int scap;
} bases[] = {
	{"and", 1}, {"eor", 1}, {"sub", 1}, {"rsb", 1}, {"add", 1}, {"adc", 1}, {"sbc", 1}, {"rsc", 1},
	{"tst", 0}, {"teq", 0}, {"cmp", 0}, {"cmn", 0}, {"orr", 1}, {"mov", 1}, {"bic", 1}, {"mvn", 1},
	{"lsl", 1}, {"lsr", 1}, {"asr", 1}, {"ror", 1}, {"rrx", 1},
	{"mul", 1}, {"mla", 1}, {"mls", 0}, {"umull", 1}, {"umlal", 1}, {"smull", 1}, {"smlal", 1},
	{"ldr", 0}, {"str", 0}, {"ldrb", 0}, {"strb", 0}, {"ldrh", 0}, {"strh", 0}, {"ldrsb", 0}, {"ldrsh", 0},
	{"ldrd", 0}, {"strd", 0}, {"ldrex", 0}, {"strex", 0}, {"ldrexb", 0}, {"strexb", 0}, {"ldrexh", 0}, {"strexh", 0},
	{"ldrexd", 0}, {"strexd", 0},
	{"push", 0}, {"pop", 0}, {"stm", 0}, {"ldm", 0}, {"stmia", 0}, {"ldmia", 0}, {"stmdb", 0}, {"ldmdb", 0},
	{"stmfd", 0}, {"ldmfd", 0}, {"stmib", 0}, {"ldmib", 0}, {"stmda", 0}, {"ldmda", 0}, {"stmea", 0}, {"ldmea", 0},
	{"b", 0}, {"bl", 0}, {"bx", 0}, {"blx", 0}, {"svc", 0}, {"udf", 0}, {"nop", 0}, {"yield", 0},
	{"wfe", 0}, {"wfi", 0}, {"sev", 0}, {"dmb", 0}, {"dsb", 0}, {"isb", 0}, {"mrc", 0}, {"mcr", 0},
	{"mrs", 0}, {"msr", 0}, {"clz", 0}, {"rev", 0}, {"rev16", 0}, {"uxtb", 0}, {"uxth", 0}, {"sxtb", 0}, {"sxth", 0},
	{"ubfx", 0}, {"sbfx", 0}, {"bfi", 0}, {"bfc", 0}, {"movw", 0}, {"movt", 0}, {"pld", 0}, {"clrex", 0},
	{"vmov", 0}, {"vldr", 0}, {"vstr", 0}, {"vadd", 0}, {"vsub", 0}, {"vmul", 0}, {"vdiv", 0}, {"vneg", 0},
	{"vabs", 0}, {"vsqrt", 0}, {"vcmp", 0}, {"vcmpe", 0}, {"vcvt", 0}, {"vmrs", 0}, {"vmsr", 0}, {"vpush", 0},
	{"vpop", 0}, {"vmla", 0}, {"vmls", 0},
	{0, 0},
};

/* rozklad mnemoniky: základ, podmínka, s, datový typ (.f32/.f64 …) */
static int
splitmnem(char *mnem, char *base, int *cond, int *sflag, char *dtype)
{
	char *dot, full[32], *suf;
	int n, k, len, c;

	*cond = 14;
	*sflag = 0;
	dtype[0] = 0;
	if (strlen(mnem) >= sizeof full)
		return 0;
	strcpy(full, mnem);
	dot = strchr(full, '.');
	if (dot) {
		strcpy(dtype, dot + 1);
		*dot = 0;
	}
	len = strlen(full);
	/* přesná shoda, pak podmínka, s, s + podmínka */
	for (k=0; bases[k].name; k++)
		if (strcmp(full, bases[k].name) == 0) {
			strcpy(base, full);
			return 1;
		}
	for (n=0; n<3; n++) {
		int withs = n == 1 || n == 2;
		int withc = n == 0 || n == 2;
		int cut = len - (withs ? 1 : 0) - (withc ? 2 : 0);
		if (cut <= 0)
			continue;
		suf = full + cut;
		c = 14;
		if (withc) {
			char cs[3];
			cs[0] = suf[withs ? 1 : 0];
			cs[1] = suf[withs ? 2 : 1];
			cs[2] = 0;
			c = condcode(cs);
			if (c < 0)
				continue;
		}
		if (withs && suf[0] != 's')
			continue;
		for (k=0; bases[k].name; k++)
			if ((int)strlen(bases[k].name) == cut && strncmp(full, bases[k].name, cut) == 0 && (!withs || bases[k].scap)) {
				memcpy(base, full, cut);
				base[cut] = 0;
				*cond = c;
				*sflag = withs;
				return 1;
			}
	}
	return 0;
}

static int
barrieropt(AOp *o, int no)
{
	if (no == 0)
		return 0xf;
	if (o[0].kind == 7) {
		if (strcmp(o[0].word, "sy") == 0) return 0xf;
		if (strcmp(o[0].word, "st") == 0) return 0xe;
		if (strcmp(o[0].word, "ish") == 0) return 0xb;
		if (strcmp(o[0].word, "ishst") == 0) return 0xa;
		if (strcmp(o[0].word, "nsh") == 0) return 0x7;
		if (strcmp(o[0].word, "nshst") == 0) return 0x6;
		if (strcmp(o[0].word, "osh") == 0) return 0x3;
		if (strcmp(o[0].word, "oshst") == 0) return 0x2;
	}
	merr("barrier option expected");
	return 0;
}

static int
copword(AOp *o, char c)
{
	char *end;
	int v;

	if (o->kind != 7 || o->word[0] != c)
		merr("coprocessor operand expected");
	v = strtol(o->word + 1, &end, 10);
	if (*end || v < 0 || v > 15)
		merr("coprocessor operand expected");
	return v;
}

/* VFP konstanta: imm8 (znaménko, 3 bity exponentu, 4 bity mantisy) */
static int
vfpimm(double v, int dbl)
{
	int imm8, sign, b, cd, efgh;
	uint64_t bits, want;
	float f;

	if (dbl)
		memcpy(&want, &v, 8);
	else {
		uint32_t w32;
		f = (float)v;
		memcpy(&w32, &f, 4);
		want = w32;
	}
	for (imm8=0; imm8<256; imm8++) {
		sign = imm8 >> 7;
		b = (imm8 >> 6) & 1;
		cd = (imm8 >> 4) & 3;
		efgh = imm8 & 0xf;
		if (dbl)
			bits = ((uint64_t)sign << 63) | ((uint64_t)(!b) << 62) | (b ? (uint64_t)0xff << 54 : 0) | ((uint64_t)cd << 52) | ((uint64_t)efgh << 48);
		else
			bits = ((uint64_t)sign << 31) | ((uint64_t)(!b) << 30) | (b ? (uint64_t)0x1f << 25 : 0) | ((uint64_t)cd << 23) | ((uint64_t)efgh << 19);
		if (bits == want)
			return imm8;
	}
	return -1;
}

/* registry VFP v polích: Vd (15:12) + D (22), Vn (19:16) + N (7), Vm (3:0) + M (5) */
static uint32_t
vd(int r, int dbl)
{
	return dbl ? ((uint32_t)(r & 15) << 12) | ((uint32_t)(r >> 4) << 22) : ((uint32_t)(r >> 1) << 12) | ((uint32_t)(r & 1) << 22);
}

static uint32_t
vn(int r, int dbl)
{
	return dbl ? ((uint32_t)(r & 15) << 16) | ((uint32_t)(r >> 4) << 7) : ((uint32_t)(r >> 1) << 16) | ((uint32_t)(r & 1) << 7);
}

static uint32_t
vm(int r, int dbl)
{
	return dbl ? (uint32_t)(r & 15) | ((uint32_t)(r >> 4) << 5) : (uint32_t)(r >> 1) | ((uint32_t)(r & 1) << 5);
}

static int
vreg(AOp *o, int dbl)
{
	return dbl ? dreg(o) : sreg(o);
}

/* adresa load/store slova a bajtu (imm12 nebo registr s posunem) */
static uint32_t
addrmode2(AOp *m, int size)
{
	uint32_t w;
	int64_t off;

	(void)size;
	w = ((uint32_t)m->base << 16);
	if (m->pre && !m->post)
		w |= 1u << 24;
	if (m->wb)
		w |= 1u << 21;
	if (m->idx >= 0) {
		w |= 1u << 25;
		if (!m->idxneg)
			w |= 1u << 23;
		w |= (uint32_t)m->idx | ((uint32_t)m->shamt << 7) | ((uint32_t)m->shtype << 5);
		if (m->shreg >= 0)
			merr("register shift is not allowed in addresses");
	} else {
		off = m->off;
		if (off >= 0)
			w |= 1u << 23;
		else
			off = -off;
		if (off > 4095)
			merr("offset out of range");
		w |= (uint32_t)off;
	}
	return w;
}

/* adresa půlslova/dvojslova (imm8 rozdělený nebo registr) */
static uint32_t
addrmode3(AOp *m)
{
	uint32_t w;
	int64_t off;

	w = ((uint32_t)m->base << 16);
	if (m->pre && !m->post)
		w |= 1u << 24;
	if (m->wb)
		w |= 1u << 21;
	if (m->idx >= 0) {
		if (m->shamt || m->shreg >= 0)
			merr("shifted index is not allowed here");
		if (!m->idxneg)
			w |= 1u << 23;
		w |= (uint32_t)m->idx;
	} else {
		w |= 1u << 22;
		off = m->off;
		if (off >= 0)
			w |= 1u << 23;
		else
			off = -off;
		if (off > 255)
			merr("offset out of range");
		w |= ((uint32_t)(off >> 4) << 8) | ((uint32_t)off & 0xf);
	}
	return w;
}

static void
literal(uint32_t w, int enc, AOp *target)
{
	if (target->kind != 4 || target->e.sym < 0 || target->imm)
		merr("label expected");
	/* [pc, #±off] k návěští téže sekce: P = adresa instrukce + 8 */
	put32fix(w | (15u << 16) | (1u << 24), FPcrel, -8, enc, &target->e);
}

/* ---- instrukce ---- */

static int
arminsn(char *mnem, char *ops)
{
	AOp o[6];
	int no, n, rd, rn, rm, sflag, cond, dbl;
	int64_t v;
	uint32_t w;
	char base[32], dtype[16];

	curmnem = mnem;
	if (!splitmnem(mnem, base, &cond, &sflag, dtype))
		return 0;
	curcond = cond;
	no = parseops(ops, o, 6);

	/* datové instrukce */
	for (n=0; dops[n].name; n++)
		if (strcmp(base, dops[n].name) == 0) {
			if (dops[n].form == 0) {
				if (no < 3 || no > 4)
					merr("three operands expected");
				dataproc(dops[n].opc, sflag, xreg(&o[0]), xreg(&o[1]), o, no, 2);
			} else if (dops[n].form == 1) {
				if (no < 2 || no > 3)
					merr("two operands expected");
				dataproc(dops[n].opc, sflag, xreg(&o[0]), 0, o, no, 1);
			} else {
				if (no < 2 || no > 3)
					merr("two operands expected");
				dataproc(dops[n].opc, 1, 0, xreg(&o[0]), o, no, 1);
			}
			return 1;
		}
	/* posuny jako mov s posunutým registrem */
	if (strcmp(base, "lsl") == 0 || strcmp(base, "lsr") == 0 || strcmp(base, "asr") == 0
	|| strcmp(base, "ror") == 0 || strcmp(base, "rrx") == 0) {
		int st = shiftname(base);
		if (st == 4) {
			if (no != 2)
				merr("two operands expected");
			put32(cc() | (13u << 21) | ((uint32_t)sflag << 20) | ((uint32_t)xreg(&o[0]) << 12) | (3u << 5) | (uint32_t)xreg(&o[1]));
			return 1;
		}
		if (no != 3)
			merr("three operands expected");
		rd = xreg(&o[0]);
		rm = xreg(&o[1]);
		if (o[2].kind == 4) {
			v = constant(&o[2]);
			if (v < 0 || v > 32 || (v == 32 && st != 1 && st != 2))
				merr("shift amount out of range");
			if (v == 32)
				v = 0;
			put32(cc() | (13u << 21) | ((uint32_t)sflag << 20) | ((uint32_t)rd << 12) | ((uint32_t)v << 7) | ((uint32_t)st << 5) | (uint32_t)rm);
		} else
			put32(cc() | (13u << 21) | ((uint32_t)sflag << 20) | ((uint32_t)rd << 12) | ((uint32_t)xreg(&o[2]) << 8) | ((uint32_t)st << 5) | 0x10 | (uint32_t)rm);
		return 1;
	}
	/* násobení */
	if (strcmp(base, "mul") == 0) {
		if (no != 3)
			merr("three operands expected");
		put32(cc() | ((uint32_t)sflag << 20) | ((uint32_t)xreg(&o[0]) << 16) | ((uint32_t)xreg(&o[2]) << 8) | 0x90 | (uint32_t)xreg(&o[1]));
		return 1;
	}
	if (strcmp(base, "mla") == 0 || strcmp(base, "mls") == 0) {
		if (no != 4)
			merr("four operands expected");
		w = base[1] == 'l' && base[2] == 's' ? 0x00600090 : 0x00200090 | ((uint32_t)sflag << 20);
		put32(cc() | w | ((uint32_t)xreg(&o[0]) << 16) | ((uint32_t)xreg(&o[3]) << 12) | ((uint32_t)xreg(&o[2]) << 8) | (uint32_t)xreg(&o[1]));
		return 1;
	}
	if (strcmp(base, "umull") == 0 || strcmp(base, "umlal") == 0 || strcmp(base, "smull") == 0 || strcmp(base, "smlal") == 0) {
		if (no != 4)
			merr("four operands expected");
		w = base[0] == 'u' ? (base[2] == 'u' ? 0x00800090 : 0x00a00090) : (base[2] == 'u' ? 0x00c00090 : 0x00e00090);
		put32(cc() | w | ((uint32_t)sflag << 20) | ((uint32_t)xreg(&o[1]) << 16) | ((uint32_t)xreg(&o[0]) << 12) | ((uint32_t)xreg(&o[3]) << 8) | (uint32_t)xreg(&o[2]));
		return 1;
	}
	if (strcmp(base, "clz") == 0 || strcmp(base, "rev") == 0 || strcmp(base, "rev16") == 0
	|| strcmp(base, "uxtb") == 0 || strcmp(base, "uxth") == 0 || strcmp(base, "sxtb") == 0 || strcmp(base, "sxth") == 0) {
		if (no != 2)
			merr("two operands expected");
		w = strcmp(base, "clz") == 0 ? 0x016f0f10 : strcmp(base, "rev") == 0 ? 0x06bf0f30 : strcmp(base, "rev16") == 0 ? 0x06bf0fb0
			: strcmp(base, "uxtb") == 0 ? 0x06ef0070 : strcmp(base, "uxth") == 0 ? 0x06ff0070 : strcmp(base, "sxtb") == 0 ? 0x06af0070 : 0x06bf0070;
		put32(cc() | w | ((uint32_t)xreg(&o[0]) << 12) | (uint32_t)xreg(&o[1]));
		return 1;
	}
	if (strcmp(base, "ubfx") == 0 || strcmp(base, "sbfx") == 0) {
		if (no != 4)
			merr("four operands expected");
		v = constant(&o[2]);
		n = constant(&o[3]);
		if (v < 0 || v > 31 || n < 1 || v + n > 32)
			merr("bitfield out of range");
		put32(cc() | (base[0] == 'u' ? 0x07e00050 : 0x07a00050) | ((uint32_t)(n - 1) << 16) | ((uint32_t)xreg(&o[0]) << 12) | ((uint32_t)v << 7) | (uint32_t)xreg(&o[1]));
		return 1;
	}
	if (strcmp(base, "bfi") == 0) {
		if (no != 4)
			merr("four operands expected");
		v = constant(&o[2]);
		n = constant(&o[3]);
		if (v < 0 || v > 31 || n < 1 || v + n > 32)
			merr("bitfield out of range");
		put32(cc() | 0x07c00010 | ((uint32_t)(v + n - 1) << 16) | ((uint32_t)xreg(&o[0]) << 12) | ((uint32_t)v << 7) | (uint32_t)xreg(&o[1]));
		return 1;
	}
	if (strcmp(base, "bfc") == 0) {
		if (no != 3)
			merr("three operands expected");
		v = constant(&o[1]);
		n = constant(&o[2]);
		if (v < 0 || v > 31 || n < 1 || v + n > 32)
			merr("bitfield out of range");
		put32(cc() | 0x07c0001f | ((uint32_t)(v + n - 1) << 16) | ((uint32_t)xreg(&o[0]) << 12) | ((uint32_t)v << 7));
		return 1;
	}
	/* movw/movt */
	if (strcmp(base, "movw") == 0 || strcmp(base, "movt") == 0) {
		if (no != 2)
			merr("two operands expected");
		rd = xreg(&o[0]);
		w = cc() | (base[3] == 'w' ? 0x03000000 : 0x03400000) | ((uint32_t)rd << 12);
		if (o[1].kind == 4 && o[1].mod) {
			if ((o[1].mod == 1) != (base[3] == 'w'))
				merr("modifier does not match the instruction");
			put32fix(w, FAbs, 0, base[3] == 'w' ? EMovw : EMovt, &o[1].e);
			return 1;
		}
		v = constant(&o[1]);
		if (v < 0 || v > 0xffff)
			merr("immediate out of range");
		put32(w | (((uint32_t)v >> 12) << 16) | ((uint32_t)v & 0xfff));
		return 1;
	}
	/* loady a story */
	if (strcmp(base, "ldr") == 0 || strcmp(base, "str") == 0 || strcmp(base, "ldrb") == 0 || strcmp(base, "strb") == 0) {
		int load = base[0] == 'l', byte = base[3] == 'b';
		if (no != 2)
			merr("two operands expected");
		rd = xreg(&o[0]);
		w = cc() | 0x04000000 | ((uint32_t)load << 20) | ((uint32_t)byte << 22) | ((uint32_t)rd << 12);
		if (o[1].kind == 5) {
			put32(w | addrmode2(&o[1], 4));
			return 1;
		}
		if (!load || byte)
			merr("memory operand expected");
		literal(w, ELdrLit, &o[1]);
		return 1;
	}
	if (strcmp(base, "ldrh") == 0 || strcmp(base, "strh") == 0 || strcmp(base, "ldrsb") == 0 || strcmp(base, "ldrsh") == 0
	|| strcmp(base, "ldrd") == 0 || strcmp(base, "strd") == 0) {
		int load, sh, mi;
		load = strcmp(base, "ldrh") == 0 || strcmp(base, "ldrsb") == 0 || strcmp(base, "ldrsh") == 0;
		sh = strcmp(base, "ldrh") == 0 || strcmp(base, "strh") == 0 ? 0xb : strcmp(base, "ldrsb") == 0 || strcmp(base, "ldrd") == 0 ? 0xd : 0xf;
		mi = 1;
		if (base[3] == 'd') {
			/* ldrd/strd Rt, [Rt2,] [..] */
			if (no == 3) {
				if (xreg(&o[1]) != xreg(&o[0]) + 1)
					merr("second register must follow the first");
				mi = 2;
			} else if (no != 2)
				merr("two or three operands expected");
		} else if (no != 2)
			merr("two operands expected");
		rd = xreg(&o[0]);
		if (o[mi].kind != 5)
			merr("memory operand expected");
		put32(cc() | ((uint32_t)load << 20) | ((uint32_t)rd << 12) | ((uint32_t)sh << 4) | 0x90 | addrmode3(&o[mi]));
		return 1;
	}
	if (strncmp(base, "ldrex", 5) == 0 || strncmp(base, "strex", 5) == 0) {
		int load = base[0] == 'l';
		char sz = base[5];
		uint32_t op = sz == 0 ? 0x01900f9f : sz == 'd' ? 0x01b00f9f : sz == 'b' ? 0x01d00f9f : 0x01f00f9f;
		if (!load)
			op = (op & ~0x0010000fu) | 0;  /* strex: bit 20 = 0, Rt místo 0xf */
		if (load) {
			if (sz == 'd') {
				if (no != 3 || o[2].kind != 5 || xreg(&o[1]) != xreg(&o[0]) + 1)
					merr("ldrexd Rt, Rt2, [Rn] expected");
				put32(cc() | op | ((uint32_t)o[2].base << 16) | ((uint32_t)xreg(&o[0]) << 12));
			} else {
				if (no != 2 || o[1].kind != 5)
					merr("ldrex Rt, [Rn] expected");
				put32(cc() | op | ((uint32_t)o[1].base << 16) | ((uint32_t)xreg(&o[0]) << 12));
			}
			return 1;
		}
		if (sz == 'd') {
			if (no != 4 || o[3].kind != 5 || xreg(&o[2]) != xreg(&o[1]) + 1)
				merr("strexd Rd, Rt, Rt2, [Rn] expected");
			put32(cc() | op | ((uint32_t)o[3].base << 16) | ((uint32_t)xreg(&o[0]) << 12) | (uint32_t)xreg(&o[1]));
		} else {
			if (no != 3 || o[2].kind != 5)
				merr("strex Rd, Rt, [Rn] expected");
			put32(cc() | op | ((uint32_t)o[2].base << 16) | ((uint32_t)xreg(&o[0]) << 12) | (uint32_t)xreg(&o[1]));
		}
		return 1;
	}
	/* bloky registrů */
	if (strcmp(base, "push") == 0 || strcmp(base, "pop") == 0) {
		if (no != 1 || o[0].kind != 6 || o[0].listkind != 1)
			merr("register list expected");
		put32(cc() | (base[1] == 'u' ? 0x092d0000 : 0x08bd0000) | o[0].list);
		return 1;
	}
	if (strncmp(base, "stm", 3) == 0 || strncmp(base, "ldm", 3) == 0) {
		int load = base[0] == 'l', wbk;
		char *m = base + 3;
		uint32_t pu;
		if (*m == 0 || strcmp(m, "ia") == 0 || (load ? strcmp(m, "fd") == 0 : strcmp(m, "ea") == 0))
			pu = 0x00800000;   /* IA: P=0 U=1 */
		else if (strcmp(m, "db") == 0 || (load ? strcmp(m, "ea") == 0 : strcmp(m, "fd") == 0))
			pu = 0x01000000;   /* DB */
		else if (strcmp(m, "ib") == 0)
			pu = 0x01800000;
		else if (strcmp(m, "da") == 0)
			pu = 0;
		else
			return 0;
		if (no != 2 || o[1].kind != 6 || o[1].listkind != 1)
			merr("Rn, {list} expected");
		/* Rn! přijde jako výraz? registr se zapsaným '!' parser nezná:
		 * zkusí se slovo před '!' */
		if (o[0].kind == 1)
			rn = o[0].reg;
		else
			merr("base register expected");
		wbk = o[0].wb;
		put32(cc() | 0x08000000 | pu | ((uint32_t)wbk << 21) | ((uint32_t)load << 20) | ((uint32_t)rn << 16) | o[1].list);
		return 1;
	}
	/* skoky */
	if (strcmp(base, "b") == 0 || strcmp(base, "bl") == 0) {
		if (no != 1 || o[0].kind != 4 || o[0].e.sym < 0)
			merr("branch target expected");
		put32fix(cc() | (base[1] ? 0x0b000000 : 0x0a000000), FBranch, -8, base[1] ? ECall : EBranch, &o[0].e);
		return 1;
	}
	if (strcmp(base, "bx") == 0 || strcmp(base, "blx") == 0) {
		if (no != 1)
			merr("one operand expected");
		put32(cc() | (base[1] == 'x' ? 0x012fff10 : 0x012fff30) | (uint32_t)xreg(&o[0]));
		return 1;
	}
	if (strcmp(base, "svc") == 0) {
		if (no != 1)
			merr("one operand expected");
		v = constant(&o[0]);
		if (v < 0 || v > 0xffffff)
			merr("immediate out of range");
		put32(cc() | 0x0f000000 | (uint32_t)v);
		return 1;
	}
	if (strcmp(base, "udf") == 0) {
		if (no != 1)
			merr("one operand expected");
		v = constant(&o[0]);
		if (v < 0 || v > 0xffff)
			merr("immediate out of range");
		put32(0xe7f000f0 | (((uint32_t)v >> 4) << 8) | ((uint32_t)v & 0xf));
		return 1;
	}
	if (strcmp(base, "nop") == 0) { put32(cc() | 0x0320f000); return 1; }
	if (strcmp(base, "yield") == 0) { put32(cc() | 0x0320f001); return 1; }
	if (strcmp(base, "wfe") == 0) { put32(cc() | 0x0320f002); return 1; }
	if (strcmp(base, "wfi") == 0) { put32(cc() | 0x0320f003); return 1; }
	if (strcmp(base, "sev") == 0) { put32(cc() | 0x0320f004); return 1; }
	if (strcmp(base, "dmb") == 0) { put32(0xf57ff050 | (uint32_t)barrieropt(o, no)); return 1; }
	if (strcmp(base, "dsb") == 0) { put32(0xf57ff040 | (uint32_t)barrieropt(o, no)); return 1; }
	if (strcmp(base, "isb") == 0) { put32(0xf57ff060 | (uint32_t)barrieropt(o, no)); return 1; }
	if (strcmp(base, "clrex") == 0) { put32(0xf57ff01f); return 1; }
	if (strcmp(base, "pld") == 0) {
		if (no != 1 || o[0].kind != 5)
			merr("memory operand expected");
		put32(0xf550f000 | addrmode2(&o[0], 4));
		return 1;
	}
	if (strcmp(base, "mrc") == 0 || strcmp(base, "mcr") == 0) {
		if (no != 6)
			merr("six operands expected");
		w = (uint32_t)copword(&o[0], 'p') << 8;
		w |= (uint32_t)constant(&o[1]) << 21;
		w |= (uint32_t)xreg(&o[2]) << 12;
		w |= (uint32_t)copword(&o[3], 'c') << 16;
		w |= (uint32_t)copword(&o[4], 'c');
		w |= (uint32_t)constant(&o[5]) << 5;
		put32(cc() | (base[1] == 'r' ? 0x0e100010 : 0x0e000010) | w);
		return 1;
	}
	if (strcmp(base, "mrs") == 0) {
		if (no != 2 || o[1].kind != 7)
			merr("mrs Rd, cpsr expected");
		put32(cc() | 0x010f0000 | ((uint32_t)xreg(&o[0]) << 12));
		return 1;
	}

	/* VFP */
	dbl = strcmp(dtype, "f64") == 0;
	if (strcmp(base, "vldr") == 0 || strcmp(base, "vstr") == 0) {
		int load = base[1] == 'l';
		if (no != 2)
			merr("two operands expected");
		dbl = o[0].kind == 3;
		rd = vreg(&o[0], dbl);
		w = cc() | 0x0d000a00 | ((uint32_t)load << 20) | ((uint32_t)dbl << 8) | vd(rd, dbl);
		if (o[1].kind == 5) {
			int64_t off = o[1].off;
			if (o[1].idx >= 0 || o[1].post || o[1].wb)
				merr("only [Rn, #imm] is allowed");
			w |= 1u << 24 | ((uint32_t)o[1].base << 16);
			if (off >= 0)
				w |= 1u << 23;
			else
				off = -off;
			if (off & 3 || off > 1020)
				merr("offset out of range");
			put32(w | (uint32_t)(off >> 2));
			return 1;
		}
		if (!load)
			merr("memory operand expected");
		literal(w, EVldrLit, &o[1]);
		return 1;
	}
	if (strcmp(base, "vpush") == 0 || strcmp(base, "vpop") == 0) {
		if (no != 1 || o[0].kind != 6 || o[0].listkind == 1)
			merr("VFP register list expected");
		dbl = o[0].listkind == 3;
		w = cc() | (base[1] == 'p' && base[2] == 'u' ? 0x0d2d0a00 : 0x0cbd0a00) | ((uint32_t)dbl << 8) | vd(o[0].listlo, dbl);
		put32(w | (uint32_t)(dbl ? 2 * o[0].listn : o[0].listn));
		return 1;
	}
	if (strcmp(base, "vmrs") == 0) {
		if (no != 2)
			merr("two operands expected");
		if (o[0].kind == 7)
			put32(cc() | 0x0ef1fa10);
		else
			put32(cc() | 0x0ef10a10 | ((uint32_t)xreg(&o[0]) << 12));
		return 1;
	}
	if (strcmp(base, "vmsr") == 0) {
		if (no != 2)
			merr("two operands expected");
		put32(cc() | 0x0ee10a10 | ((uint32_t)xreg(&o[1]) << 12));
		return 1;
	}
	if (strcmp(base, "vmov") == 0) {
		if (no == 2 && o[0].kind == 2 && o[1].kind == 1) {
			put32(cc() | 0x0e000a10 | vn(o[0].reg, 0) | ((uint32_t)o[1].reg << 12));
			return 1;
		}
		if (no == 2 && o[0].kind == 1 && o[1].kind == 2) {
			put32(cc() | 0x0e100a10 | vn(o[1].reg, 0) | ((uint32_t)o[0].reg << 12));
			return 1;
		}
		if (no == 3 && o[0].kind == 1 && o[1].kind == 1 && o[2].kind == 3) {
			put32(cc() | 0x0c500b10 | ((uint32_t)o[1].reg << 16) | ((uint32_t)o[0].reg << 12) | vm(o[2].reg, 1));
			return 1;
		}
		if (no == 3 && o[0].kind == 3 && o[1].kind == 1 && o[2].kind == 1) {
			put32(cc() | 0x0c400b10 | ((uint32_t)o[2].reg << 16) | ((uint32_t)o[1].reg << 12) | vm(o[0].reg, 1));
			return 1;
		}
		if (no == 2 && (o[0].kind == 2 || o[0].kind == 3) && o[1].kind == 4) {
			int imm;
			dbl = o[0].kind == 3;
			if (!o[1].isfloat)
				o[1].fval = (double)constant(&o[1]);
			imm = vfpimm(o[1].fval, dbl);
			if (imm < 0)
				merr("invalid floating-point constant");
			put32(cc() | 0x0eb00a00 | ((uint32_t)dbl << 8) | vd(o[0].reg, dbl) | ((uint32_t)(imm >> 4) << 16) | ((uint32_t)imm & 0xf));
			return 1;
		}
		if (no == 2 && o[0].kind == o[1].kind && (o[0].kind == 2 || o[0].kind == 3)) {
			dbl = o[0].kind == 3;
			put32(cc() | 0x0eb00a40 | ((uint32_t)dbl << 8) | vd(o[0].reg, dbl) | vm(o[1].reg, dbl));
			return 1;
		}
		merr("unsupported vmov form");
	}
	if (strcmp(base, "vadd") == 0 || strcmp(base, "vsub") == 0 || strcmp(base, "vmul") == 0 || strcmp(base, "vdiv") == 0
	|| strcmp(base, "vmla") == 0 || strcmp(base, "vmls") == 0) {
		if (no != 3)
			merr("three operands expected");
		w = strcmp(base, "vadd") == 0 ? 0x0e300a00 : strcmp(base, "vsub") == 0 ? 0x0e300a40 : strcmp(base, "vmul") == 0 ? 0x0e200a00
			: strcmp(base, "vdiv") == 0 ? 0x0e800a00 : strcmp(base, "vmla") == 0 ? 0x0e000a00 : 0x0e000a40;
		put32(cc() | w | ((uint32_t)dbl << 8) | vd(vreg(&o[0], dbl), dbl) | vn(vreg(&o[1], dbl), dbl) | vm(vreg(&o[2], dbl), dbl));
		return 1;
	}
	if (strcmp(base, "vneg") == 0 || strcmp(base, "vabs") == 0 || strcmp(base, "vsqrt") == 0) {
		if (no != 2)
			merr("two operands expected");
		w = base[1] == 'n' ? 0x0eb10a40 : base[1] == 'a' ? 0x0eb00ac0 : 0x0eb10ac0;
		put32(cc() | w | ((uint32_t)dbl << 8) | vd(vreg(&o[0], dbl), dbl) | vm(vreg(&o[1], dbl), dbl));
		return 1;
	}
	if (strcmp(base, "vcmp") == 0 || strcmp(base, "vcmpe") == 0) {
		if (no != 2)
			merr("two operands expected");
		w = base[4] == 'e' ? 0x0eb40ac0 : 0x0eb40a40;
		if (o[1].kind == 4) {
			if ((o[1].isfloat ? o[1].fval : (double)constant(&o[1])) != 0.0)
				merr("only #0 is allowed");
			put32(cc() | (w | 0x00010000) | ((uint32_t)dbl << 8) | vd(vreg(&o[0], dbl), dbl));
			return 1;
		}
		put32(cc() | w | ((uint32_t)dbl << 8) | vd(vreg(&o[0], dbl), dbl) | vm(vreg(&o[1], dbl), dbl));
		return 1;
	}
	if (strcmp(base, "vcvt") == 0) {
		/* dtype: to.from */
		char *from = strchr(dtype, '.');
		char to[8];
		int tdbl, fdbl;
		if (!from || no != 2)
			merr("vcvt.<to>.<from> expected");
		n = from - dtype;
		if (n >= (int)sizeof to)
			return 0;
		memcpy(to, dtype, n);
		to[n] = 0;
		from++;
		if (strcmp(to, "f64") == 0 && strcmp(from, "f32") == 0) {
			put32(cc() | 0x0eb70ac0 | vd(dreg(&o[0]), 1) | vm(sreg(&o[1]), 0));
			return 1;
		}
		if (strcmp(to, "f32") == 0 && strcmp(from, "f64") == 0) {
			put32(cc() | 0x0eb70bc0 | vd(sreg(&o[0]), 0) | vm(dreg(&o[1]), 1));
			return 1;
		}
		tdbl = strcmp(to, "f64") == 0;
		fdbl = strcmp(from, "f64") == 0;
		if ((strcmp(to, "f32") == 0 || tdbl) && (strcmp(from, "s32") == 0 || strcmp(from, "u32") == 0)) {
			w = from[0] == 's' ? 0x0eb80ac0 : 0x0eb80a40;
			put32(cc() | w | ((uint32_t)tdbl << 8) | vd(vreg(&o[0], tdbl), tdbl) | vm(sreg(&o[1]), 0));
			return 1;
		}
		if ((strcmp(to, "s32") == 0 || strcmp(to, "u32") == 0) && (strcmp(from, "f32") == 0 || fdbl)) {
			w = to[0] == 's' ? 0x0ebd0ac0 : 0x0ebc0ac0;
			put32(cc() | w | ((uint32_t)fdbl << 8) | vd(sreg(&o[0]), 0) | vm(vreg(&o[1], fdbl), fdbl));
			return 1;
		}
		merr("unsupported vcvt form");
	}
	return 0;
}

/* ---- rozhraní jádra ---- */

static uint
armjsize(AItem *i)
{
	(void)i;
	return 4;
}

static int
armjfits(AItem *i, int64_t d)
{
	(void)i; (void)d;
	return 1;
}

static void
armjemit(AItem *i, uchar *b, int64_t d, int r)
{
	(void)i; (void)b; (void)d; (void)r;
	die("arm: unexpected jump item");
}

static void
putw(uchar *b, uint32_t w)
{
	b[0] = w;
	b[1] = w >> 8;
	b[2] = w >> 16;
	b[3] = w >> 24;
}

static uint32_t
getw(uchar *b)
{
	return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void
armnops(uchar *b, uint n)
{
	uint k;

	if (n % 4)
		aerr("alignment padding in code is not a multiple of 4");
	for (k=0; k<n; k+=4)
		putw(b + k, 0xe320f000);
}

static int
armreloc(AFix *f)
{
	switch (f->enc) {
	case ENone:
		if (f->kind == FAbs)
			return f->size == 4 ? 2 : f->size == 2 ? 5 : f->size == 1 ? 8 : 0;
		return f->size == 4 ? 3 : 0;   /* REL32 */
	case EBranch: return 29;
	case ECall: return 28;
	case EMovw: return 43;
	case EMovt: return 44;
	}
	return 0;
}

static void
armfixenc(uchar *b, AFix *f, int64_t v)
{
	uint32_t w;

	w = getw(b);
	switch (f->enc) {
	case EBranch:
	case ECall:
		if (v & 3)
			aerr("branch target is not aligned");
		v >>= 2;
		if (v < -(1 << 23) || v >= (1 << 23))
			aerr("branch out of range");
		putw(b, (w & 0xff000000) | ((uint32_t)v & 0xffffff));
		break;
	case ELdrLit:
		if (v < 0) {
			v = -v;
			w &= ~(1u << 23);
		} else
			w |= 1u << 23;
		if (v > 4095)
			aerr("literal out of range");
		putw(b, (w & ~0xfffu) | (uint32_t)v);
		break;
	case EVldrLit:
		if (v < 0) {
			v = -v;
			w &= ~(1u << 23);
		} else
			w |= 1u << 23;
		if ((v & 3) || v > 1020)
			aerr("literal out of range");
		putw(b, (w & ~0xffu) | (uint32_t)(v >> 2));
		break;
	default:
		aerr("cannot resolve relocation locally");
	}
}

/* REL: addend v poli podle kódování */
static void
armrelenc(uchar *b, AFix *f, int64_t a)
{
	uint32_t w;

	w = getw(b);
	switch (f->enc) {
	case EBranch:
	case ECall:
		putw(b, (w & 0xff000000) | ((uint32_t)(a >> 2) & 0xffffff));
		break;
	case EMovw:
	case EMovt:
		if (a < -32768 || a > 32767)
			aerr("offset out of range");
		putw(b, (w & 0xfff0f000) | ((((uint32_t)a >> 12) & 0xf) << 16) | ((uint32_t)a & 0xfff));
		break;
	default:
		putw(b, (uint32_t)a);
	}
}

static int
armsymreloc(AFix *f)
{
	return f->enc == EMovw || f->enc == EMovt;
}

AArch AARM = {
	.name = "arm",
	.machine = 40,
	.elf64 = 0,
	.insn = arminsn,
	.jsize = armjsize,
	.jfits = armjfits,
	.jemit = armjemit,
	.nops = armnops,
	.reloc = armreloc,
	.fixenc = armfixenc,
	.relenc = armrelenc,
	.symreloc = armsymreloc,
	.resolveglobal = 0,
	.minalign = 4,
	.comment = '@',
	.wordsize = 4,
	.keeplocalsym = 0,
	.eflags = 0x5000000,   /* EABI verze 5 */
	.padcode = 2,
};

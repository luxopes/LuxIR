#include "as.h"

/* Kodér x86 (64bitový režim: amd64, 32bitový: i386) pro vestavěný
 * assembler, syntaxe AT&T jako u GNU as. Pokrývá instrukce, které vypisují
 * emitory LuxIR a vrstvy architektury runtime LSL; volba kódování (krátké
 * formy s akumulátorem, imm8, movabs, nop výplně) kopíruje GNU as, aby
 * byly objekty bajtově shodné. */

enum {
	ONone,
	OReg,
	OImm,
	OMem,
};

enum {
	RGpr8,
	RGpr16,
	RGpr32,
	RGpr64,
	RXmm,
	RSeg,
	RRip,
};

typedef struct Opnd Opnd;
struct Opnd {
	int kind;
	int cls;
	int reg;
	int high;      /* ah, ch, dh, bh */
	int indirect;  /* *operand */
	int seg;       /* -1, nebo číslo segmentového registru */
	int base, index, scale;
	AExpr e;       /* imm nebo posun */
	int hasdisp;
};

static int mode64;
static char *curmnem;

/* instrukce ve výstavbě */
static uchar ib[32];
static int in;
static struct {
	int pos, size, kind, sext;
	AExpr e;
} ifix[4];
static int nifix;

static void
xerr(char *msg)
{
	aerr("%s: %s", curmnem, msg);
}

/* ---- registry ---- */

static struct {
	char *name;
	int cls;
	int num;
	int high;
} regs[] = {
	{"rax", RGpr64, 0, 0}, {"rcx", RGpr64, 1, 0}, {"rdx", RGpr64, 2, 0}, {"rbx", RGpr64, 3, 0},
	{"rsp", RGpr64, 4, 0}, {"rbp", RGpr64, 5, 0}, {"rsi", RGpr64, 6, 0}, {"rdi", RGpr64, 7, 0},
	{"r8", RGpr64, 8, 0}, {"r9", RGpr64, 9, 0}, {"r10", RGpr64, 10, 0}, {"r11", RGpr64, 11, 0},
	{"r12", RGpr64, 12, 0}, {"r13", RGpr64, 13, 0}, {"r14", RGpr64, 14, 0}, {"r15", RGpr64, 15, 0},
	{"eax", RGpr32, 0, 0}, {"ecx", RGpr32, 1, 0}, {"edx", RGpr32, 2, 0}, {"ebx", RGpr32, 3, 0},
	{"esp", RGpr32, 4, 0}, {"ebp", RGpr32, 5, 0}, {"esi", RGpr32, 6, 0}, {"edi", RGpr32, 7, 0},
	{"r8d", RGpr32, 8, 0}, {"r9d", RGpr32, 9, 0}, {"r10d", RGpr32, 10, 0}, {"r11d", RGpr32, 11, 0},
	{"r12d", RGpr32, 12, 0}, {"r13d", RGpr32, 13, 0}, {"r14d", RGpr32, 14, 0}, {"r15d", RGpr32, 15, 0},
	{"ax", RGpr16, 0, 0}, {"cx", RGpr16, 1, 0}, {"dx", RGpr16, 2, 0}, {"bx", RGpr16, 3, 0},
	{"sp", RGpr16, 4, 0}, {"bp", RGpr16, 5, 0}, {"si", RGpr16, 6, 0}, {"di", RGpr16, 7, 0},
	{"r8w", RGpr16, 8, 0}, {"r9w", RGpr16, 9, 0}, {"r10w", RGpr16, 10, 0}, {"r11w", RGpr16, 11, 0},
	{"r12w", RGpr16, 12, 0}, {"r13w", RGpr16, 13, 0}, {"r14w", RGpr16, 14, 0}, {"r15w", RGpr16, 15, 0},
	{"al", RGpr8, 0, 0}, {"cl", RGpr8, 1, 0}, {"dl", RGpr8, 2, 0}, {"bl", RGpr8, 3, 0},
	{"spl", RGpr8, 4, 0}, {"bpl", RGpr8, 5, 0}, {"sil", RGpr8, 6, 0}, {"dil", RGpr8, 7, 0},
	{"ah", RGpr8, 4, 1}, {"ch", RGpr8, 5, 1}, {"dh", RGpr8, 6, 1}, {"bh", RGpr8, 7, 1},
	{"r8b", RGpr8, 8, 0}, {"r9b", RGpr8, 9, 0}, {"r10b", RGpr8, 10, 0}, {"r11b", RGpr8, 11, 0},
	{"r12b", RGpr8, 12, 0}, {"r13b", RGpr8, 13, 0}, {"r14b", RGpr8, 14, 0}, {"r15b", RGpr8, 15, 0},
	{"r8l", RGpr8, 8, 0}, {"r9l", RGpr8, 9, 0}, {"r10l", RGpr8, 10, 0}, {"r11l", RGpr8, 11, 0},
	{"r12l", RGpr8, 12, 0}, {"r13l", RGpr8, 13, 0}, {"r14l", RGpr8, 14, 0}, {"r15l", RGpr8, 15, 0},
	{"xmm0", RXmm, 0, 0}, {"xmm1", RXmm, 1, 0}, {"xmm2", RXmm, 2, 0}, {"xmm3", RXmm, 3, 0},
	{"xmm4", RXmm, 4, 0}, {"xmm5", RXmm, 5, 0}, {"xmm6", RXmm, 6, 0}, {"xmm7", RXmm, 7, 0},
	{"xmm8", RXmm, 8, 0}, {"xmm9", RXmm, 9, 0}, {"xmm10", RXmm, 10, 0}, {"xmm11", RXmm, 11, 0},
	{"xmm12", RXmm, 12, 0}, {"xmm13", RXmm, 13, 0}, {"xmm14", RXmm, 14, 0}, {"xmm15", RXmm, 15, 0},
	{"es", RSeg, 0, 0}, {"cs", RSeg, 1, 0}, {"ss", RSeg, 2, 0}, {"ds", RSeg, 3, 0},
	{"fs", RSeg, 4, 0}, {"gs", RSeg, 5, 0},
	{"rip", RRip, 0, 0},
	{0, 0, 0, 0}
};

static int
findreg(char *name, int *cls, int *num, int *high)
{
	int n;

	for (n=0; regs[n].name; n++)
		if (strcmp(regs[n].name, name) == 0) {
			*cls = regs[n].cls;
			*num = regs[n].num;
			*high = regs[n].high;
			if (!mode64 && (*num >= 8 || *cls == RGpr64 || *cls == RRip
			|| (*cls == RGpr8 && *num >= 4 && !*high)))
				xerr("register is not available in 32-bit mode");
			return 1;
		}
	return 0;
}

/* ---- rozbor operandů ---- */

static char *
regname(char **ps)
{
	static char buf[16];
	char *p;
	int n;

	p = *ps;
	if (*p != '%')
		xerr("register expected");
	p++;
	n = 0;
	while (isalnum((uchar)*p) && n < 15)
		buf[n++] = *p++;
	buf[n] = 0;
	*ps = p;
	return buf;
}

static void
parsereg(char **ps, Opnd *o)
{
	o->kind = OReg;
	if (!findreg(regname(ps), &o->cls, &o->reg, &o->high))
		xerr("unknown register");
}

static void
parsemem(char **ps, Opnd *o)
{
	char *p;
	int cls, num, high;
	int64_t v;

	p = *ps;
	o->kind = OMem;
	o->base = o->index = -1;
	o->scale = 1;
	o->hasdisp = 0;
	o->e.sym = -1;
	o->e.add = 0;
	aspace(&p);
	if (*p != '(') {
		if (!aexpr(&p, &o->e))
			xerr("operand expected");
		o->hasdisp = 1;
		aspace(&p);
	}
	if (*p == '(') {
		p++;
		aspace(&p);
		if (*p == '%') {
			if (!findreg(regname(&p), &cls, &num, &high))
				xerr("unknown register");
			if (cls == RRip)
				o->base = 16;
			else if (cls == (mode64 ? RGpr64 : RGpr32))
				o->base = num;
			else
				xerr("invalid base register");
			aspace(&p);
		}
		if (*p == ',') {
			p++;
			aspace(&p);
			if (*p == '%') {
				if (!findreg(regname(&p), &cls, &num, &high))
					xerr("unknown register");
				if (cls != (mode64 ? RGpr64 : RGpr32) || num == 4)
					xerr("invalid index register");
				o->index = num;
				aspace(&p);
			}
			if (*p == ',') {
				p++;
				aspace(&p);
				if (!anumber(&p, &v) || (v != 1 && v != 2 && v != 4 && v != 8))
					xerr("invalid scale");
				o->scale = v;
				aspace(&p);
			}
		}
		if (*p != ')')
			xerr("')' expected");
		p++;
	}
	*ps = p;
}

static void
parseopnd(char *p, Opnd *o)
{
	int cls, num, high;
	char *q;

	memset(o, 0, sizeof *o);
	o->seg = -1;
	o->e.sym = -1;
	aspace(&p);
	if (*p == '*') {
		o->indirect = 1;
		p++;
		aspace(&p);
	}
	if (*p == '$') {
		p++;
		o->kind = OImm;
		if (!aexpr(&p, &o->e))
			xerr("immediate expected");
	} else if (*p == '%') {
		/* segmentový prefix před pamětí, jinak registr */
		q = p;
		if (!findreg(regname(&q), &cls, &num, &high))
			xerr("unknown register");
		aspace(&q);
		if (cls == RSeg && *q == ':') {
			q++;
			o->seg = num;
			parsemem(&q, o);
			p = q;
		} else
			parsereg(&p, o);
	} else
		parsemem(&p, o);
	aspace(&p);
	if (*p)
		xerr("junk after operand");
}

/* Rozdělení řádku operandů čárkami mimo závorky. */
static int
splitops(char *s, char *parts[4])
{
	int n, depth;
	char *p;

	n = 0;
	aspace(&s);
	if (!*s)
		return 0;
	parts[n++] = s;
	depth = 0;
	for (p=s; *p; p++) {
		if (*p == '(')
			depth++;
		else if (*p == ')')
			depth--;
		else if (*p == ',' && depth == 0) {
			*p = 0;
			if (n == 4)
				xerr("too many operands");
			parts[n++] = p + 1;
		}
	}
	return n;
}

/* ---- výstavba instrukce ---- */

static void
put(int b)
{
	ib[in++] = b;
}

static void
addfix(int pos, int size, int kind, int sext, AExpr *e)
{
	ifix[nifix].pos = pos;
	ifix[nifix].size = size;
	ifix[nifix].kind = kind;
	ifix[nifix].sext = sext;
	ifix[nifix].e = *e;
	nifix++;
}

/* Úzký immediate (1 a 2 bajty) jako GNU as: hodnota v rozsahu 32 bitů
 * se nejprve znaménkově rozšíří (0xffffffff je -1), takže se vejde i do
 * krátké formy imm8. */
static void
narrow(AExpr *e, int size)
{
	/* LLVM (Mach-O) 32bitovou hodnotu u 16bitové operace nepřevádí
	 * na zápornou: 0xffffffff dostane imm16, ne imm8 */
	if (amacho)
		return;
	if (size <= 2 && e->sym < 0 && e->add >= 0x80000000 && e->add <= 0xffffffff)
		e->add -= (int64_t)1 << 32;
}

static void
putimm(AExpr *e, int size, int sext)
{
	int n;
	int64_t v, mask;

	if (e->sym >= 0) {
		if (size != 4 && size != 8)
			xerr("symbol in a narrow immediate");
		addfix(in, size, FAbs, sext, e);
		for (n=0; n<size; n++)
			put(0);
		return;
	}
	narrow(e, size);
	v = e->add;
	if (size < 8) {
		/* bity nad polem musí být samé nuly nebo jedničky, jinak se
		 * hodnota zkrátí s varováním (offset_in_range v GNU as) */
		mask = ((int64_t)1 << (8 * size)) - 1;
		if ((v & ~mask) != 0 && (v | mask) != -1)
			awarn("0x%llx shortened to 0x%llx", (unsigned long long)v, (unsigned long long)(v & mask));
	}
	for (n=0; n<size; n++)
		put((v >> (8 * n)) & 0xff);
}

/* Odeslání hotové instrukce; opravy relativně k PC dostanou pcoff podle
 * počtu bajtů za polem. */
static int lockpfx;  /* prefix lock/rep před následující instrukcí */

static void
flush(void)
{
	uchar *b;
	int n, off;
	AItem *it;

	if (in == 0)
		return;
	if (lockpfx) {
		memmove(ib + 1, ib, in);
		ib[0] = lockpfx;
		in++;
		for (n=0; n<nifix; n++)
			ifix[n].pos++;
		lockpfx = 0;
	}
	b = aemit(in);
	if (!b)
		xerr("instruction in a nobits section");
	memcpy(b, ib, in);
	/* posun začátku instrukce v položce */
	it = &asecs[acursec].item[asecs[acursec].nitem - 1];
	off = it->size - in;
	for (n=0; n<nifix; n++) {
		if (ifix[n].kind == FAbs)
			afix(off + ifix[n].pos, ifix[n].size, FAbs, ifix[n].sext, &ifix[n].e);
		else
			afix(off + ifix[n].pos, ifix[n].size, ifix[n].kind, -(in - ifix[n].pos), &ifix[n].e);
	}
	in = 0;
	nifix = 0;
}

/* Prefixy, REX a opcode: rexw, reg (pole reg nebo rozšíření opcode),
 * rm operand (registr nebo paměť), případné REX kvůli 8bitovým registrům
 * sil/dil (needrex). Vrátí se po zápisu ModRM/SIB/disp; immediate přidá
 * volající (pcoff opravy posunu se dopočítá ve flush). */
static int rex_needed;

static void
segprefix(Opnd *m)
{
	static uchar pfx[] = {0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65};

	if (m->seg >= 0)
		put(pfx[m->seg]);
}

static void
regcheck8(Opnd *o)
{
	if (o->kind == OReg && o->cls == RGpr8) {
		if (o->high && rex_needed)
			xerr("cannot encode high byte register with REX");
		if (!o->high && o->reg >= 4 && o->reg < 8)
			rex_needed = 1;
	}
}

static void
modrm(int w, int reg, Opnd *rm, int opsize16, int *legacy, int nlegacy, int opc0, int opc1, int opc2)
{
	int rex, mod, rmf, sib, disp, dispsize, n, hassib;
	int64_t d;

	/* pole reg >= 8 -> REX.R, rm/base >= 8 -> REX.B, index >= 8 -> REX.X */
	rex = 0;
	if (w)
		rex |= 8;
	if (reg >= 8)
		rex |= 4;
	if (rm->kind == OReg) {
		if (rm->reg >= 8)
			rex |= 1;
	} else {
		if (rm->base >= 8 && rm->base != 16)
			rex |= 1;
		if (rm->index >= 8)
			rex |= 2;
	}
	if (rex_needed)
		rex |= 0x40;
	if (rex && !mode64)
		xerr("instruction needs 64-bit mode");
	if (rm->kind == OMem)
		segprefix(rm);
	if (opsize16)
		put(0x66);
	for (n=0; n<nlegacy; n++)
		put(legacy[n]);
	if (rex)
		put(0x40 | (rex & 0xf));
	put(opc0);
	if (opc1 >= 0)
		put(opc1);
	if (opc2 >= 0)
		put(opc2);
	if (rm->kind == OReg) {
		put(0xc0 | ((reg & 7) << 3) | (rm->reg & 7));
		return;
	}
	/* paměť */
	hassib = 0;
	sib = 0;
	disp = 0;
	dispsize = 0;
	d = rm->e.add;
	if (rm->base == 16) {
		/* rip-relativní */
		if (rm->index >= 0)
			xerr("rip with index");
		put(((reg & 7) << 3) | 5);
		if (rm->e.sym >= 0) {
			addfix(in, 4, FPcrel, 0, &rm->e);
			put(0), put(0), put(0), put(0);
		} else
			for (n=0; n<4; n++)
				put((d >> (8 * n)) & 0xff);
		return;
	}
	if (rm->base < 0 && rm->index < 0) {
		/* absolutní adresa: 64bitový režim přes SIB, 32bitový mod 00 rm 101 */
		if (mode64) {
			put(((reg & 7) << 3) | 4);
			put(0x25);
		} else
			put(((reg & 7) << 3) | 5);
		if (rm->e.sym >= 0) {
			addfix(in, 4, FAbs, 1, &rm->e);
			put(0), put(0), put(0), put(0);
		} else
			for (n=0; n<4; n++)
				put((d >> (8 * n)) & 0xff);
		return;
	}
	/* velikost posunu */
	if (rm->e.sym >= 0)
		dispsize = 4;
	else if (d == 0 && (rm->base & 7) != 5)
		dispsize = 0;
	else if (d >= -128 && d <= 127)
		dispsize = 1;
	else
		dispsize = 4;
	if (rm->base < 0) {
		/* jen index: mod 00, base 101, disp32 */
		hassib = 1;
		mod = 0;
		dispsize = 4;
		sib = ((rm->scale == 8 ? 3 : rm->scale == 4 ? 2 : rm->scale == 2 ? 1 : 0) << 6) | ((rm->index & 7) << 3) | 5;
		rmf = 4;
	} else {
		mod = dispsize == 0 ? 0 : dispsize == 1 ? 1 : 2;
		if (rm->index >= 0 || (rm->base & 7) == 4) {
			hassib = 1;
			rmf = 4;
			sib = ((rm->scale == 8 ? 3 : rm->scale == 4 ? 2 : rm->scale == 2 ? 1 : 0) << 6)
				| ((rm->index >= 0 ? rm->index & 7 : 4) << 3) | (rm->base & 7);
		} else
			rmf = rm->base & 7;
	}
	put((mod << 6) | ((reg & 7) << 3) | rmf);
	if (hassib)
		put(sib);
	if (dispsize == 1)
		put(d & 0xff);
	else if (dispsize == 4) {
		if (rm->e.sym >= 0) {
			addfix(in, 4, FAbs, 1, &rm->e);
			put(0), put(0), put(0), put(0);
		} else
			for (n=0; n<4; n++)
				put((d >> (8 * n)) & 0xff);
	}
	(void)disp;
}

/* Velikost operandu podle přípony mnemoniky nebo registrů: 1, 2, 4, 8. */
static int
opsize(char *suffix, Opnd *o, int n)
{
	int k, s;

	if (suffix && *suffix) {
		switch (*suffix) {
		case 'b': return 1;
		case 'w': return 2;
		case 'l': return 4;
		case 'q': return 8;
		}
	}
	for (k=0; k<n; k++)
		if (o[k].kind == OReg) {
			s = o[k].cls == RGpr8 ? 1 : o[k].cls == RGpr16 ? 2 : o[k].cls == RGpr32 ? 4 : o[k].cls == RGpr64 ? 8 : 0;
			if (s)
				return s;
		}
	return 0;
}

static int
isacc(Opnd *o)
{
	return o->kind == OReg && o->cls <= RGpr64 && o->reg == 0 && !o->high;
}

static int
fits8(AExpr *e)
{
	return e->sym < 0 && e->add >= -128 && e->add <= 127;
}

static void
checkreg(Opnd *o, int size)
{
	int s;

	if (o->kind != OReg)
		return;
	s = o->cls == RGpr8 ? 1 : o->cls == RGpr16 ? 2 : o->cls == RGpr32 ? 4 : o->cls == RGpr64 ? 8 : 0;
	if (s != size)
		xerr("operand size mismatch");
}

/* ---- skupiny instrukcí ---- */

/* add/or/adc/sbb/and/sub/xor/cmp: n = číslo operace */
static void
alu(int n, Opnd *o, int no, char *suf)
{
	int size, w, s16, imm8;
	int legacy[1];

	if (no != 2)
		xerr("two operands expected");
	size = opsize(suf, o, no);
	if (!size)
		xerr("operand size unknown");
	checkreg(&o[0], size);
	checkreg(&o[1], size);
	w = size == 8;
	s16 = size == 2;
	rex_needed = 0;
	regcheck8(&o[0]);
	regcheck8(&o[1]);
	if (o[0].kind == OImm) {
		if (o[1].kind == OImm)
			xerr("invalid operands");
		narrow(&o[0].e, size);
		imm8 = size > 1 && fits8(&o[0].e);
		if (size == 1) {
			if (isacc(&o[1])) {
				/* krátká forma s AL */
				if (rex_needed)
					put(0x40);
				put(0x04 + 8 * n);
				putimm(&o[0].e, 1, 0);
			} else {
				modrm(0, n, &o[1], 0, legacy, 0, 0x80, -1, -1);
				putimm(&o[0].e, 1, 0);
			}
		} else if (imm8) {
			modrm(w, n, &o[1], s16, legacy, 0, 0x83, -1, -1);
			putimm(&o[0].e, 1, 0);
		} else if (isacc(&o[1])) {
			if (s16)
				put(0x66);
			if (w)
				put(0x48);
			put(0x05 + 8 * n);
			putimm(&o[0].e, s16 ? 2 : 4, w);
		} else {
			modrm(w, n, &o[1], s16, legacy, 0, 0x81, -1, -1);
			putimm(&o[0].e, s16 ? 2 : 4, w);
		}
		return;
	}
	if (o[0].kind == OReg && (o[1].kind == OReg || o[1].kind == OMem))
		modrm(w, o[0].reg, &o[1], s16, legacy, 0, (8 * n) + (size == 1 ? 0 : 1), -1, -1);
	else if (o[0].kind == OMem && o[1].kind == OReg)
		modrm(w, o[1].reg, &o[0], s16, legacy, 0, (8 * n) + (size == 1 ? 2 : 3), -1, -1);
	else
		xerr("invalid operands");
}

static void
test(Opnd *o, int no, char *suf)
{
	int size, w, s16;
	int legacy[1];

	if (no != 2)
		xerr("two operands expected");
	size = opsize(suf, o, no);
	if (!size)
		xerr("operand size unknown");
	w = size == 8;
	s16 = size == 2;
	rex_needed = 0;
	regcheck8(&o[0]);
	regcheck8(&o[1]);
	if (o[0].kind == OImm) {
		if (isacc(&o[1])) {
			if (s16)
				put(0x66);
			if (w)
				put(0x48);
			put(size == 1 ? 0xa8 : 0xa9);
		} else
			modrm(w, 0, &o[1], s16, legacy, 0, size == 1 ? 0xf6 : 0xf7, -1, -1);
		putimm(&o[0].e, size == 1 ? 1 : s16 ? 2 : 4, w);
		return;
	}
	if (o[0].kind == OReg)
		modrm(w, o[0].reg, &o[1], s16, legacy, 0, size == 1 ? 0x84 : 0x85, -1, -1);
	else if (o[1].kind == OReg)
		modrm(w, o[1].reg, &o[0], s16, legacy, 0, size == 1 ? 0x84 : 0x85, -1, -1);
	else
		xerr("invalid operands");
}

static void
mov(Opnd *o, int no, char *suf)
{
	int size, w, s16, n;
	int legacy[1];
	int64_t v;

	if (no != 2)
		xerr("two operands expected");
	if (o[0].kind == OImm && o[1].kind == OReg && o[1].cls == RXmm)
		xerr("invalid operands");
	/* segmentové registry */
	if (o[1].kind == OReg && o[1].cls == RSeg) {
		rex_needed = 0;
		modrm(0, o[1].reg, &o[0], 0, legacy, 0, 0x8e, -1, -1);
		return;
	}
	if (o[0].kind == OReg && o[0].cls == RSeg) {
		rex_needed = 0;
		modrm(0, o[0].reg, &o[1], 0, legacy, 0, 0x8c, -1, -1);
		return;
	}
	size = opsize(suf, o, no);
	if (!size)
		xerr("operand size unknown");
	checkreg(&o[0], size);
	checkreg(&o[1], size);
	w = size == 8;
	s16 = size == 2;
	rex_needed = 0;
	regcheck8(&o[0]);
	regcheck8(&o[1]);
	if (o[0].kind == OImm) {
		if (o[1].kind == OReg) {
			if (size == 8 && o[0].e.sym < 0 && (o[0].e.add < INT32_MIN || o[0].e.add > INT32_MAX)) {
				/* movabs */
				put(0x48 | (o[1].reg >= 8 ? 1 : 0));
				put(0xb8 + (o[1].reg & 7));
				v = o[0].e.add;
				for (n=0; n<8; n++)
					put((v >> (8 * n)) & 0xff);
				return;
			}
			if (size == 8) {
				modrm(1, 0, &o[1], 0, legacy, 0, 0xc7, -1, -1);
				putimm(&o[0].e, 4, 1);
				return;
			}
			if (s16)
				put(0x66);
			if (rex_needed || o[1].reg >= 8)
				put(0x40 | (o[1].reg >= 8 ? 1 : 0));
			put((size == 1 ? 0xb0 : 0xb8) + (o[1].reg & 7));
			putimm(&o[0].e, size, 0);
			return;
		}
		modrm(w, 0, &o[1], s16, legacy, 0, size == 1 ? 0xc6 : 0xc7, -1, -1);
		putimm(&o[0].e, size == 1 ? 1 : s16 ? 2 : 4, w);
		return;
	}
	/* akumulátor a absolutní adresa ve 32bitovém režimu: moffs */
	if (!mode64 && o[0].kind == OMem && o[0].base < 0 && o[0].index < 0 && isacc(&o[1])) {
		segprefix(&o[0]);
		if (s16)
			put(0x66);
		put(size == 1 ? 0xa0 : 0xa1);
		putimm(&o[0].e, 4, 0);
		return;
	}
	if (!mode64 && o[1].kind == OMem && o[1].base < 0 && o[1].index < 0 && isacc(&o[0])) {
		segprefix(&o[1]);
		if (s16)
			put(0x66);
		put(size == 1 ? 0xa2 : 0xa3);
		putimm(&o[1].e, 4, 0);
		return;
	}
	if (o[0].kind == OReg)
		modrm(w, o[0].reg, &o[1], s16, legacy, 0, size == 1 ? 0x88 : 0x89, -1, -1);
	else if (o[1].kind == OReg)
		modrm(w, o[1].reg, &o[0], s16, legacy, 0, size == 1 ? 0x8a : 0x8b, -1, -1);
	else
		xerr("invalid operands");
}

/* movzb/movzw/movsb/movsw/movsl: op = druhý bajt opcode (B6, B7, BE, BF)
 * nebo 0x63 pro movslq; srcsize velikost zdroje */
static void
movx(int op, int srcsize, Opnd *o, int no, char *suf)
{
	int size, w;
	int legacy[1];

	if (no != 2 || o[1].kind != OReg)
		xerr("invalid operands");
	size = opsize(suf, &o[1], 1);
	if (!size || size <= srcsize)
		xerr("operand size mismatch");
	checkreg(&o[0], srcsize);
	w = size == 8;
	rex_needed = 0;
	regcheck8(&o[0]);
	if (op == 0x63)
		modrm(w, o[1].reg, &o[0], 0, legacy, 0, 0x63, -1, -1);
	else
		modrm(w, o[1].reg, &o[0], size == 2, legacy, 0, 0x0f, op, -1);
}

static void
lea(Opnd *o, int no, char *suf)
{
	int size;
	int legacy[1];

	if (no != 2 || o[0].kind != OMem || o[1].kind != OReg)
		xerr("invalid operands");
	size = opsize(suf, &o[1], 1);
	rex_needed = 0;
	modrm(size == 8, o[1].reg, &o[0], size == 2, legacy, 0, 0x8d, -1, -1);
}

static void
pushpop(int push, Opnd *o, int no, char *suf)
{
	int legacy[1];

	if (no != 1)
		xerr("one operand expected");
	if (o[0].kind == OReg) {
		if (suf && *suf == 'w')
			put(0x66);
		if (o[0].reg >= 8)
			put(0x41);
		put((push ? 0x50 : 0x58) + (o[0].reg & 7));
	} else if (o[0].kind == OImm && push) {
		if (fits8(&o[0].e)) {
			put(0x6a);
			putimm(&o[0].e, 1, 0);
		} else {
			put(0x68);
			putimm(&o[0].e, 4, 1);
		}
	} else if (o[0].kind == OMem) {
		rex_needed = 0;
		modrm(0, push ? 6 : 0, &o[0], suf && *suf == 'w', legacy, 0, push ? 0xff : 0x8f, -1, -1);
	} else
		xerr("invalid operands");
}

static void
xchg(Opnd *o, int no, char *suf)
{
	int size, w, s16, other;
	int legacy[1];

	if (no != 2)
		xerr("two operands expected");
	size = opsize(suf, o, no);
	w = size == 8;
	s16 = size == 2;
	rex_needed = 0;
	regcheck8(&o[0]);
	regcheck8(&o[1]);
	if (o[0].kind == OReg && o[1].kind == OReg && size >= 2 && (isacc(&o[0]) || isacc(&o[1]))) {
		other = isacc(&o[0]) ? o[1].reg : o[0].reg;
		if (s16)
			put(0x66);
		if (w || other >= 8)
			put(0x40 | (w ? 8 : 0) | (other >= 8 ? 1 : 0));
		put(0x90 + (other & 7));
		return;
	}
	/* dva registry: GNU as dává první operand do pole reg, LLVM druhý
	 * (Mach-O podle LLVM) */
	if (amacho && o[0].kind == OReg && o[1].kind == OReg)
		modrm(w, o[1].reg, &o[0], s16, legacy, 0, size == 1 ? 0x86 : 0x87, -1, -1);
	else if (o[0].kind == OReg)
		modrm(w, o[0].reg, &o[1], s16, legacy, 0, size == 1 ? 0x86 : 0x87, -1, -1);
	else if (o[1].kind == OReg)
		modrm(w, o[1].reg, &o[0], s16, legacy, 0, size == 1 ? 0x86 : 0x87, -1, -1);
	else
		xerr("invalid operands");
}

static void
imul(Opnd *o, int no, char *suf)
{
	int size, w, s16;
	int legacy[1];
	Opnd *src, *dst;

	size = opsize(suf, o, no);
	if (!size)
		xerr("operand size unknown");
	w = size == 8;
	s16 = size == 2;
	rex_needed = 0;
	if (no == 1) {
		modrm(w, 5, &o[0], s16, legacy, 0, size == 1 ? 0xf6 : 0xf7, -1, -1);
		return;
	}
	if (o[0].kind == OImm) {
		src = no == 3 ? &o[1] : &o[1];
		dst = no == 3 ? &o[2] : &o[1];
		if (dst->kind != OReg)
			xerr("invalid operands");
		narrow(&o[0].e, s16 ? 2 : 4);
		if (fits8(&o[0].e)) {
			modrm(w, dst->reg, src, s16, legacy, 0, 0x6b, -1, -1);
			putimm(&o[0].e, 1, 0);
		} else {
			modrm(w, dst->reg, src, s16, legacy, 0, 0x69, -1, -1);
			putimm(&o[0].e, s16 ? 2 : 4, w);
		}
		return;
	}
	if (no != 2 || o[1].kind != OReg)
		xerr("invalid operands");
	modrm(w, o[1].reg, &o[0], s16, legacy, 0, 0x0f, 0xaf, -1);
}

/* posuny a rotace: n = rozšíření opcode (shl 4, shr 5, sar 7, rol 0, ror 1) */
static void
shift(int n, Opnd *o, int no, char *suf)
{
	int size, w, s16;
	int legacy[1];
	Opnd one;

	if (no == 1) {
		/* implicitní posun o 1 */
		memset(&one, 0, sizeof one);
		one.kind = OImm;
		one.e.sym = -1;
		one.e.add = 1;
		o[1] = o[0];
		o[0] = one;
		no = 2;
	}
	if (no != 2)
		xerr("invalid operands");
	size = opsize(suf, &o[1], 1);
	if (!size)
		xerr("operand size unknown");
	w = size == 8;
	s16 = size == 2;
	rex_needed = 0;
	regcheck8(&o[1]);
	if (o[0].kind == OImm) {
		if (o[0].e.sym < 0 && o[0].e.add == 1)
			modrm(w, n, &o[1], s16, legacy, 0, size == 1 ? 0xd0 : 0xd1, -1, -1);
		else {
			modrm(w, n, &o[1], s16, legacy, 0, size == 1 ? 0xc0 : 0xc1, -1, -1);
			putimm(&o[0].e, 1, 0);
		}
	} else if (o[0].kind == OReg && o[0].cls == RGpr8 && o[0].reg == 1 && !o[0].high)
		modrm(w, n, &o[1], s16, legacy, 0, size == 1 ? 0xd2 : 0xd3, -1, -1);
	else
		xerr("invalid shift count");
}

/* neg 3, not 2, mul 4, div 6, idiv 7; inc 0, dec 1 (FE/FF) */
static void
unary(int n, int opc, Opnd *o, int no, char *suf)
{
	int size, w, s16;
	int legacy[1];

	if (no != 1)
		xerr("one operand expected");
	size = opsize(suf, o, 1);
	if (!size)
		xerr("operand size unknown");
	w = size == 8;
	s16 = size == 2;
	rex_needed = 0;
	regcheck8(&o[0]);
	modrm(w, n, &o[0], s16, legacy, 0, size == 1 ? opc - 1 : opc, -1, -1);
}

static char *ccnames[] = {
	"o", "no", "b", "ae", "z", "nz", "be", "a",
	"s", "ns", "p", "np", "l", "ge", "le", "g",
	0
};

/* alias podmínky -> číslo; -1 neznámá */
static int
condcode(char *s)
{
	int n;

	for (n=0; ccnames[n]; n++)
		if (strcmp(s, ccnames[n]) == 0)
			return n;
	if (strcmp(s, "c") == 0 || strcmp(s, "nae") == 0) return 2;
	if (strcmp(s, "nc") == 0 || strcmp(s, "nb") == 0) return 3;
	if (strcmp(s, "e") == 0) return 4;
	if (strcmp(s, "ne") == 0) return 5;
	if (strcmp(s, "na") == 0) return 6;
	if (strcmp(s, "nbe") == 0) return 7;
	if (strcmp(s, "pe") == 0) return 10;
	if (strcmp(s, "po") == 0) return 11;
	if (strcmp(s, "nge") == 0) return 12;
	if (strcmp(s, "nl") == 0) return 13;
	if (strcmp(s, "ng") == 0) return 14;
	if (strcmp(s, "nle") == 0) return 15;
	return -1;
}

static void
setcc(int cc, Opnd *o, int no)
{
	int legacy[1];

	if (no != 1)
		xerr("one operand expected");
	checkreg(&o[0], 1);
	rex_needed = 0;
	regcheck8(&o[0]);
	modrm(0, 0, &o[0], 0, legacy, 0, 0x0f, 0x90 + cc, -1);
}

static void
cmovcc(int cc, Opnd *o, int no, char *suf)
{
	int size;
	int legacy[1];

	if (no != 2 || o[1].kind != OReg)
		xerr("invalid operands");
	size = opsize(suf, o, no);
	rex_needed = 0;
	modrm(size == 8, o[1].reg, &o[0], size == 2, legacy, 0, 0x0f, 0x40 + cc, -1);
}

/* skoky a volání: jop 0-15 jcc, 16 jmp */
static void
branch(int jop, int call, Opnd *o, int no)
{
	int legacy[1];

	if (no != 1)
		xerr("one operand expected");
	if (o[0].indirect || o[0].kind == OReg || (o[0].kind == OMem && o[0].indirect)) {
		if (jop < 16)
			xerr("indirect conditional jump");
		rex_needed = 0;
		modrm(0, call ? 2 : 4, &o[0], 0, legacy, 0, 0xff, -1, -1);
		return;
	}
	if (o[0].kind != OMem || o[0].base >= 0 || o[0].index >= 0 || o[0].e.sym < 0)
		xerr("branch target expected");
	if (call) {
		put(0xe8);
		addfix(in, 4, FBranch, 0, &o[0].e);
		put(0), put(0), put(0), put(0);
		return;
	}
	flush();
	ajump(jop, &o[0].e);
}

/* SSE: pfx (0, 0x66, 0xf2, 0xf3), opcode 0F op; store = pole reg je
 * první operand (zápis xmm do r/m), wreg: REX.W podle 64bitového
 * registru v operandu k */
static void
sse(int pfx, int op, int store, int wsrc, Opnd *o, int no)
{
	int legacy[1], nl, w;
	Opnd *r, *m;

	if (no != 2)
		xerr("two operands expected");
	nl = 0;
	if (pfx)
		legacy[nl++] = pfx;
	if (store) {
		r = &o[0];
		m = &o[1];
	} else {
		r = &o[1];
		m = &o[0];
	}
	if (r->kind != OReg)
		xerr("register expected");
	w = 0;
	if (wsrc) {
		if (m->kind == OReg && m->cls == RGpr64)
			w = 1;
		if (r->kind == OReg && r->cls == RGpr64)
			w = 1;
	}
	rex_needed = 0;
	modrm(w, r->reg, m, 0, legacy, nl, 0x0f, op, -1);
}

static void
noarg(int no, int a, int b, int c, int d)
{
	if (no != 0)
		xerr("no operands expected");
	put(a);
	if (b >= 0)
		put(b);
	if (c >= 0)
		put(c);
	if (d >= 0)
		put(d);
}

/* ---- rozbor mnemoniky ---- */

static int
hasprefix(char *s, char *p, char **rest)
{
	size_t n;

	n = strlen(p);
	if (strncmp(s, p, n) == 0) {
		*rest = s + n;
		return 1;
	}
	return 0;
}

/* Přípona velikosti: pro celočíselné mnemoniky poslední znak b/w/l/q;
 * vrátí základ a ukazatel na příponu ("" bez ní). */
static int
split(char *mnem, char *base, char **suf, int allowsuffix)
{
	size_t n;
	char c;

	strcpy(base, mnem);
	*suf = "";
	n = strlen(base);
	if (!allowsuffix || n < 2)
		return 1;
	c = base[n-1];
	/* mnemoniky, jejichž poslední písmeno není přípona */
	if (strcmp(base, "sub") == 0 || strcmp(base, "shl") == 0 || strcmp(base, "sal") == 0
	|| strcmp(base, "mul") == 0 || strcmp(base, "rol") == 0 || strcmp(base, "imul") == 0)
		return 1;
	if (c == 'b' || c == 'w' || c == 'l' || c == 'q') {
		static char sb[2];
		sb[0] = c;
		sb[1] = 0;
		base[n-1] = 0;
		*suf = sb;
	}
	return 1;
}

static char *alunames[] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp", 0};

static int
x86insn(char *mnem, char *ops)
{
	Opnd o[4];
	char *parts[4], base[32], *suf, *rest;
	int no, n, cc, k;

	curmnem = mnem;
	in = 0;
	nifix = 0;
	if (strlen(mnem) >= sizeof base)
		return 0;
	/* prefix lock/rep: zbytek řádku je instrukce */
	if (strcmp(mnem, "lock") == 0 || strcmp(mnem, "rep") == 0 || strcmp(mnem, "repe") == 0 || strcmp(mnem, "repne") == 0) {
		char *p = ops;
		while (*p && *p != ' ' && *p != '\t')
			p++;
		if (*p)
			*p++ = 0;
		aspace(&p);
		lockpfx = strcmp(mnem, "lock") == 0 ? 0xf0 : strcmp(mnem, "repne") == 0 ? 0xf2 : 0xf3;
		return x86insn(ops, p);
	}
	no = splitops(ops, parts);
	for (n=0; n<no; n++)
		parseopnd(parts[n], &o[n]);

	/* bez přípony: SSE, skoky, setcc, cmovcc, movx */
	if (hasprefix(mnem, "j", &rest) && (cc = condcode(rest)) >= 0) {
		branch(cc, 0, o, no);
		return 1;
	}
	if (strcmp(mnem, "jmp") == 0 || strcmp(mnem, "jmpq") == 0 || strcmp(mnem, "jmpl") == 0) {
		branch(16, 0, o, no);
		return 1;
	}
	if (strcmp(mnem, "call") == 0 || strcmp(mnem, "callq") == 0 || strcmp(mnem, "calll") == 0) {
		branch(16, 1, o, no);
		flush();
		return 1;
	}
	if (hasprefix(mnem, "set", &rest) && (cc = condcode(rest)) >= 0) {
		setcc(cc, o, no);
		flush();
		return 1;
	}
	if (hasprefix(mnem, "cmov", &rest)) {
		char cbuf[8];
		strcpy(cbuf, rest);
		k = strlen(cbuf);
		suf = "";
		if (k > 1 && (cbuf[k-1] == 'q' || cbuf[k-1] == 'l' || cbuf[k-1] == 'w') && condcode(cbuf) < 0) {
			static char sb[2];
			sb[0] = cbuf[k-1];
			sb[1] = 0;
			suf = sb;
			cbuf[k-1] = 0;
		}
		cc = condcode(cbuf);
		if (cc >= 0) {
			cmovcc(cc, o, no, suf);
			flush();
			return 1;
		}
	}
	/* movz/movs: movzbl, movzbq, movzbw, movzwl, movzwq, movsbl, movsbq,
	 * movsbw, movswl, movswq, movslq */
	if ((hasprefix(mnem, "movz", &rest) || hasprefix(mnem, "movs", &rest)) && strlen(rest) == 2
	&& (rest[0] == 'b' || rest[0] == 'w' || rest[0] == 'l') && (rest[1] == 'w' || rest[1] == 'l' || rest[1] == 'q')) {
		int sign = mnem[3] == 's';
		int srcsize = rest[0] == 'b' ? 1 : rest[0] == 'w' ? 2 : 4;
		if (srcsize == 4) {
			if (!sign || rest[1] != 'q')
				return 0;
			movx(0x63, 4, o, no, rest + 1);
		} else
			movx((sign ? 0xbe : 0xb6) + (srcsize == 2), srcsize, o, no, rest + 1);
		flush();
		return 1;
	}
	/* bez operandů */
	if (strcmp(mnem, "ret") == 0 || strcmp(mnem, "retq") == 0 || strcmp(mnem, "retl") == 0) noarg(no, 0xc3, -1, -1, -1);
	else if (strcmp(mnem, "leave") == 0 || strcmp(mnem, "leaveq") == 0) noarg(no, 0xc9, -1, -1, -1);
	else if (strcmp(mnem, "nop") == 0) noarg(no, 0x90, -1, -1, -1);
	else if (strcmp(mnem, "hlt") == 0) noarg(no, 0xf4, -1, -1, -1);
	else if (strcmp(mnem, "int3") == 0) noarg(no, 0xcc, -1, -1, -1);
	else if (strcmp(mnem, "ud2") == 0) noarg(no, 0x0f, 0x0b, -1, -1);
	else if (strcmp(mnem, "syscall") == 0) noarg(no, 0x0f, 0x05, -1, -1);
	else if (strcmp(mnem, "pause") == 0) noarg(no, 0xf3, 0x90, -1, -1);
	else if (strcmp(mnem, "endbr64") == 0) noarg(no, 0xf3, 0x0f, 0x1e, 0xfa);
	else if (strcmp(mnem, "endbr32") == 0) noarg(no, 0xf3, 0x0f, 0x1e, 0xfb);
	else if (strcmp(mnem, "cqto") == 0 || strcmp(mnem, "cqo") == 0) noarg(no, 0x48, 0x99, -1, -1);
	else if (strcmp(mnem, "cltd") == 0 || strcmp(mnem, "cdq") == 0) noarg(no, 0x99, -1, -1, -1);
	else if (strcmp(mnem, "cltq") == 0 || strcmp(mnem, "cdqe") == 0) noarg(no, 0x48, 0x98, -1, -1);
	else if (strcmp(mnem, "cwtl") == 0 || strcmp(mnem, "cwde") == 0) noarg(no, 0x98, -1, -1, -1);
	else if (strcmp(mnem, "cpuid") == 0) noarg(no, 0x0f, 0xa2, -1, -1);
	else if (strcmp(mnem, "rdtsc") == 0) noarg(no, 0x0f, 0x31, -1, -1);
	else if (strcmp(mnem, "mfence") == 0) noarg(no, 0x0f, 0xae, 0xf0, -1);
	else if (strcmp(mnem, "lfence") == 0) noarg(no, 0x0f, 0xae, 0xe8, -1);
	else if (strcmp(mnem, "sfence") == 0) noarg(no, 0x0f, 0xae, 0xf8, -1);
	else if (strcmp(mnem, "int") == 0) {
		if (no != 1 || o[0].kind != OImm)
			xerr("immediate expected");
		put(0xcd);
		putimm(&o[0].e, 1, 0);
	}
	/* SSE */
	else if (strcmp(mnem, "movsd") == 0 || strcmp(mnem, "movss") == 0) {
		int pfx = mnem[4] == 'd' ? 0xf2 : 0xf3;
		if (o[1].kind == OReg && o[1].cls == RXmm)
			sse(pfx, 0x10, 0, 0, o, no);
		else
			sse(pfx, 0x11, 1, 0, o, no);
	}
	else if ((strcmp(mnem, "movq") == 0 || strcmp(mnem, "movd") == 0) && no == 2
	&& ((o[0].kind == OReg && o[0].cls == RXmm) || (o[1].kind == OReg && o[1].cls == RXmm))) {
		int w = mnem[3] == 'q';
		if (o[1].kind == OReg && o[1].cls == RXmm) {
			/* do xmm: 66 (REX.W) 0F 6E */
			if (o[0].kind == OReg && o[0].cls == RXmm)
				sse(0xf3, 0x7e, 0, 0, o, no);
			else if (o[0].kind == OMem && w)
				sse(0xf3, 0x7e, 0, 0, o, no);
			else {
				int legacy[1];
				legacy[0] = 0x66;
				rex_needed = 0;
				modrm(w, o[1].reg, &o[0], 0, legacy, 1, 0x0f, 0x6e, -1);
			}
		} else {
			if (o[1].kind == OMem && w)
				sse(0x66, 0xd6, 1, 0, o, no);
			else {
				int legacy[1];
				legacy[0] = 0x66;
				rex_needed = 0;
				modrm(w, o[0].reg, &o[1], 0, legacy, 1, 0x0f, 0x7e, -1);
			}
		}
	}
	else if (strcmp(mnem, "movdqu") == 0) sse(0xf3, o[1].kind == OReg ? 0x6f : 0x7f, o[1].kind != OReg, 0, o, no);
	else if (strcmp(mnem, "movdqa") == 0) sse(0x66, o[1].kind == OReg ? 0x6f : 0x7f, o[1].kind != OReg, 0, o, no);
	else if (strcmp(mnem, "movaps") == 0) sse(0, o[1].kind == OReg ? 0x28 : 0x29, o[1].kind != OReg, 0, o, no);
	else if (strcmp(mnem, "movups") == 0) sse(0, o[1].kind == OReg ? 0x10 : 0x11, o[1].kind != OReg, 0, o, no);
	else if (strcmp(mnem, "movapd") == 0) sse(0x66, o[1].kind == OReg ? 0x28 : 0x29, o[1].kind != OReg, 0, o, no);
	else if (strcmp(mnem, "addsd") == 0) sse(0xf2, 0x58, 0, 0, o, no);
	else if (strcmp(mnem, "mulsd") == 0) sse(0xf2, 0x59, 0, 0, o, no);
	else if (strcmp(mnem, "subsd") == 0) sse(0xf2, 0x5c, 0, 0, o, no);
	else if (strcmp(mnem, "divsd") == 0) sse(0xf2, 0x5e, 0, 0, o, no);
	else if (strcmp(mnem, "sqrtsd") == 0) sse(0xf2, 0x51, 0, 0, o, no);
	else if (strcmp(mnem, "minsd") == 0) sse(0xf2, 0x5d, 0, 0, o, no);
	else if (strcmp(mnem, "maxsd") == 0) sse(0xf2, 0x5f, 0, 0, o, no);
	else if (strcmp(mnem, "addss") == 0) sse(0xf3, 0x58, 0, 0, o, no);
	else if (strcmp(mnem, "mulss") == 0) sse(0xf3, 0x59, 0, 0, o, no);
	else if (strcmp(mnem, "subss") == 0) sse(0xf3, 0x5c, 0, 0, o, no);
	else if (strcmp(mnem, "divss") == 0) sse(0xf3, 0x5e, 0, 0, o, no);
	else if (strcmp(mnem, "sqrtss") == 0) sse(0xf3, 0x51, 0, 0, o, no);
	else if (strcmp(mnem, "ucomisd") == 0) sse(0x66, 0x2e, 0, 0, o, no);
	else if (strcmp(mnem, "comisd") == 0) sse(0x66, 0x2f, 0, 0, o, no);
	else if (strcmp(mnem, "ucomiss") == 0) sse(0, 0x2e, 0, 0, o, no);
	else if (strcmp(mnem, "comiss") == 0) sse(0, 0x2f, 0, 0, o, no);
	else if (strcmp(mnem, "cvtss2sd") == 0) sse(0xf3, 0x5a, 0, 0, o, no);
	else if (strcmp(mnem, "cvtsd2ss") == 0) sse(0xf2, 0x5a, 0, 0, o, no);
	else if (strcmp(mnem, "cvtsi2sd") == 0 || strcmp(mnem, "cvtsi2sdl") == 0) sse(0xf2, 0x2a, 0, 1, o, no);
	else if (strcmp(mnem, "cvtsi2sdq") == 0) { if (o[0].kind == OMem) { int legacy[1]; legacy[0] = 0xf2; rex_needed = 0; modrm(1, o[1].reg, &o[0], 0, legacy, 1, 0x0f, 0x2a, -1); } else sse(0xf2, 0x2a, 0, 1, o, no); }
	else if (strcmp(mnem, "cvtsi2ss") == 0 || strcmp(mnem, "cvtsi2ssl") == 0) sse(0xf3, 0x2a, 0, 1, o, no);
	else if (strcmp(mnem, "cvtsi2ssq") == 0) { if (o[0].kind == OMem) { int legacy[1]; legacy[0] = 0xf3; rex_needed = 0; modrm(1, o[1].reg, &o[0], 0, legacy, 1, 0x0f, 0x2a, -1); } else sse(0xf3, 0x2a, 0, 1, o, no); }
	else if (strcmp(mnem, "cvttsd2si") == 0 || strcmp(mnem, "cvttsd2sil") == 0) sse(0xf2, 0x2c, 0, 1, o, no);
	else if (strcmp(mnem, "cvttsd2siq") == 0) { int legacy[1]; legacy[0] = 0xf2; rex_needed = 0; modrm(1, o[1].reg, &o[0], 0, legacy, 1, 0x0f, 0x2c, -1); }
	else if (strcmp(mnem, "cvttss2si") == 0 || strcmp(mnem, "cvttss2sil") == 0) sse(0xf3, 0x2c, 0, 1, o, no);
	else if (strcmp(mnem, "cvttss2siq") == 0) { int legacy[1]; legacy[0] = 0xf3; rex_needed = 0; modrm(1, o[1].reg, &o[0], 0, legacy, 1, 0x0f, 0x2c, -1); }
	else if (strcmp(mnem, "xorpd") == 0) sse(0x66, 0x57, 0, 0, o, no);
	else if (strcmp(mnem, "xorps") == 0) sse(0, 0x57, 0, 0, o, no);
	else if (strcmp(mnem, "andpd") == 0) sse(0x66, 0x54, 0, 0, o, no);
	else if (strcmp(mnem, "andps") == 0) sse(0, 0x54, 0, 0, o, no);
	else if (strcmp(mnem, "orpd") == 0) sse(0x66, 0x56, 0, 0, o, no);
	else if (strcmp(mnem, "pxor") == 0) sse(0x66, 0xef, 0, 0, o, no);
	else if (strcmp(mnem, "punpckldq") == 0) sse(0x66, 0x62, 0, 0, o, no);
	else if (strcmp(mnem, "pshufd") == 0) {
		if (no != 3 || o[0].kind != OImm)
			xerr("invalid operands");
		sse(0x66, 0x70, 0, 0, o + 1, 2);
		putimm(&o[0].e, 1, 0);
	}
	else if (strcmp(mnem, "psrlq") == 0 || strcmp(mnem, "psllq") == 0 || strcmp(mnem, "psrldq") == 0 || strcmp(mnem, "pslldq") == 0) {
		int legacy[1];
		int ext = strcmp(mnem, "psrlq") == 0 ? 2 : strcmp(mnem, "psllq") == 0 ? 6 : strcmp(mnem, "psrldq") == 0 ? 3 : 7;
		if (no != 2 || o[0].kind != OImm || o[1].kind != OReg)
			xerr("invalid operands");
		legacy[0] = 0x66;
		rex_needed = 0;
		modrm(0, ext, &o[1], 0, legacy, 1, 0x0f, 0x73, -1);
		putimm(&o[0].e, 1, 0);
	}
	/* x87 */
	else if (strcmp(mnem, "fldl") == 0) { int legacy[1]; rex_needed = 0; modrm(0, 0, &o[0], 0, legacy, 0, 0xdd, -1, -1); }
	else if (strcmp(mnem, "fstpl") == 0) { int legacy[1]; rex_needed = 0; modrm(0, 3, &o[0], 0, legacy, 0, 0xdd, -1, -1); }
	else if (strcmp(mnem, "flds") == 0) { int legacy[1]; rex_needed = 0; modrm(0, 0, &o[0], 0, legacy, 0, 0xd9, -1, -1); }
	else if (strcmp(mnem, "fstps") == 0) { int legacy[1]; rex_needed = 0; modrm(0, 3, &o[0], 0, legacy, 0, 0xd9, -1, -1); }
	else if (strcmp(mnem, "fstp") == 0 && no == 1 && o[0].kind == OReg) { put(0xdd); put(0xd8 + o[0].reg); }
	/* atomické */
	else if (strcmp(mnem, "xadd") == 0 || strcmp(mnem, "xaddq") == 0 || strcmp(mnem, "xaddl") == 0) {
		int size, legacy[1];
		size = opsize(mnem[4] ? mnem + 4 : "", o, no);
		rex_needed = 0;
		modrm(size == 8, o[0].reg, &o[1], size == 2, legacy, 0, 0x0f, size == 1 ? 0xc0 : 0xc1, -1);
	}
	else if (strcmp(mnem, "cmpxchg") == 0 || strcmp(mnem, "cmpxchgq") == 0 || strcmp(mnem, "cmpxchgl") == 0) {
		int size, legacy[1];
		size = opsize(mnem[7] ? mnem + 7 : "", o, no);
		rex_needed = 0;
		modrm(size == 8, o[0].reg, &o[1], size == 2, legacy, 0, 0x0f, size == 1 ? 0xb0 : 0xb1, -1);
	}
	else if (strcmp(mnem, "cmpxchg8b") == 0) { int legacy[1]; rex_needed = 0; modrm(0, 1, &o[0], 0, legacy, 0, 0x0f, 0xc7, -1); }
	else if (strcmp(mnem, "cmpxchg16b") == 0) { int legacy[1]; rex_needed = 0; modrm(1, 1, &o[0], 0, legacy, 0, 0x0f, 0xc7, -1); }
	else {
		/* celočíselné mnemoniky s příponou */
		split(mnem, base, &suf, 1);
		k = -1;
		for (n=0; alunames[n]; n++)
			if (strcmp(base, alunames[n]) == 0)
				k = n;
		if (k >= 0) alu(k, o, no, suf);
		else if (strcmp(base, "test") == 0) test(o, no, suf);
		else if (strcmp(base, "mov") == 0) mov(o, no, suf);
		else if (strcmp(base, "lea") == 0) lea(o, no, suf);
		else if (strcmp(base, "push") == 0) pushpop(1, o, no, suf);
		else if (strcmp(base, "pop") == 0) pushpop(0, o, no, suf);
		else if (strcmp(base, "xchg") == 0) xchg(o, no, suf);
		else if (strcmp(base, "imul") == 0) imul(o, no, suf);
		else if (strcmp(base, "shl") == 0 || strcmp(base, "sal") == 0) shift(4, o, no, suf);
		else if (strcmp(base, "shr") == 0) shift(5, o, no, suf);
		else if (strcmp(base, "sar") == 0) shift(7, o, no, suf);
		else if (strcmp(base, "rol") == 0) shift(0, o, no, suf);
		else if (strcmp(base, "ror") == 0) shift(1, o, no, suf);
		else if (strcmp(base, "neg") == 0) unary(3, 0xf7, o, no, suf);
		else if (strcmp(base, "not") == 0) unary(2, 0xf7, o, no, suf);
		else if (strcmp(base, "mul") == 0) unary(4, 0xf7, o, no, suf);
		else if (strcmp(base, "div") == 0) unary(6, 0xf7, o, no, suf);
		else if (strcmp(base, "idiv") == 0) unary(7, 0xf7, o, no, suf);
		else if (strcmp(base, "inc") == 0) unary(0, 0xff, o, no, suf);
		else if (strcmp(base, "dec") == 0) unary(1, 0xff, o, no, suf);
		else if (strcmp(base, "movabs") == 0) {
			if (no != 2 || o[0].kind != OImm || o[1].kind != OReg || o[1].cls != RGpr64)
				xerr("invalid operands");
			put(0x48 | (o[1].reg >= 8 ? 1 : 0));
			put(0xb8 + (o[1].reg & 7));
			if (o[0].e.sym >= 0) {
				addfix(in, 8, FAbs, 0, &o[0].e);
				for (n=0; n<8; n++)
					put(0);
			} else
				for (n=0; n<8; n++)
					put((o[0].e.add >> (8 * n)) & 0xff);
		}
		else
			return 0;
	}
	flush();
	return 1;
}

/* ---- skoky, nop, relokace ---- */

static uint
x86jsize(AItem *i)
{
	if (!i->jlong)
		return 2;
	return i->jop == 16 ? 5 : 6;
}

static int
x86jfits(AItem *i, int64_t disp)
{
	(void)i;
	return disp >= -128 && disp <= 127;
}

static void
x86jemit(AItem *i, uchar *b, int64_t disp, int resolved)
{
	int n;

	if (!i->jlong) {
		b[0] = i->jop == 16 ? 0xeb : 0x70 + i->jop;
		b[1] = disp & 0xff;
		return;
	}
	if (i->jop == 16) {
		b[0] = 0xe9;
		b++;
	} else {
		b[0] = 0x0f;
		b[1] = 0x80 + i->jop;
		b += 2;
	}
	if (!resolved)
		disp = 0;
	for (n=0; n<4; n++)
		b[n] = (disp >> (8 * n)) & 0xff;
}

/* výplň nop jako GNU as: vzory po 11 (64 bitů) nebo 8 bajtech
 * (32 bitů), zbytek napřed */
static uchar nop64[12][11] = {
	{0},
	{0x90},
	{0x66, 0x90},
	{0x0f, 0x1f, 0x00},
	{0x0f, 0x1f, 0x40, 0x00},
	{0x0f, 0x1f, 0x44, 0x00, 0x00},
	{0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00},
	{0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00},
	{0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
};

static uchar nop32[9][8] = {
	{0},
	{0x90},
	{0x66, 0x90},
	{0x8d, 0x76, 0x00},
	{0x8d, 0x74, 0x26, 0x00},
	{0x2e, 0x8d, 0x74, 0x26, 0x00},
	{0x8d, 0xb6, 0x00, 0x00, 0x00, 0x00},
	{0x8d, 0xb4, 0x26, 0x00, 0x00, 0x00, 0x00},
	{0x2e, 0x8d, 0xb4, 0x26, 0x00, 0x00, 0x00, 0x00},
};

static void
x86nops(uchar *b, uint n)
{
	uint max, r;

	mode64 = A->elf64;
	max = mode64 ? 11 : 8;
	if (n > max) {
		r = n % max;
		if (r) {
			memcpy(b, mode64 ? nop64[r] : nop32[r], r);
			b += r;
			n -= r;
		}
		while (n) {
			memcpy(b, mode64 ? nop64[max] : nop32[max], max);
			b += max;
			n -= max;
		}
		return;
	}
	memcpy(b, mode64 ? nop64[n] : nop32[n], n);
}

static int
x86reloc(AFix *f)
{
	if (mode64) {
		if (f->kind == FAbs) {
			if (f->size == 8)
				return 1;  /* R_X86_64_64 */
			if (f->size == 4)
				return f->pcoff ? 11 : 10; /* 32S : 32 (pcoff nese příznak znaménka) */
			return 0;
		}
		if (f->size != 4)
			return 0;
		return f->kind == FBranch ? 4 : 2; /* PLT32 : PC32 */
	}
	if (f->size != 4)
		return 0;
	return f->kind == FAbs ? 1 : 2; /* R_386_32 : R_386_PC32 */
}

static int
insn64(char *m, char *o)
{
	mode64 = 1;
	return x86insn(m, o);
}

static int
insn32(char *m, char *o)
{
	mode64 = 0;
	return x86insn(m, o);
}

AArch AX86_64 = {
	.name = "x86_64",
	.machine = 62,
	.elf64 = 1,
	.insn = insn64,
	.jsize = x86jsize,
	.jfits = x86jfits,
	.jemit = x86jemit,
	.nops = x86nops,
	.reloc = x86reloc,
	.resolveglobal = 1,
	.minalign = 1,
	.comment = '#',
	.wordsize = 2,
};

AArch AI386 = {
	.name = "i386",
	.machine = 3,
	.elf64 = 0,
	.insn = insn32,
	.jsize = x86jsize,
	.jfits = x86jfits,
	.jemit = x86jemit,
	.nops = x86nops,
	.reloc = x86reloc,
	.resolveglobal = 1,
	.minalign = 1,
	.comment = '#',
	.wordsize = 2,
};

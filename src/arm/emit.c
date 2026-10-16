#include "all.h"

/* Assembler arm (GNU as, unified syntax, režim ARM, VFPv3-D16).
 *
 * Rámec (fp = r11 ukazuje na uložený fp):
 *
 *   [fp+8 ...]    parametry na zásobníku (SLOT(-2) ...)
 *   [fp+4]        lr
 *   [fp]          uložený fp
 *   r4-r10        použité callee-save registry (push)
 *   d8-dN         použité callee-save d (vpush, souvislý rozsah od d8)
 *   lokální proměnné a odložené hodnoty (4*F bajtů, F sudé)
 *   výplň na 8 bajtů
 *   [sp ...]      argumenty volání na zásobníku (arm_argsz)
 *
 * ip a d7 (s14, s15) jsou pomocné registry: legalizace velkých posunů
 * a konstant, převody, výměny. */

typedef struct E E;

struct E {
	FILE *f;
	Fn *fn;
	int nr;      /* uložené r4-r10 */
	int nd;      /* uložené d8.. */
	uint below;  /* bajty pod fp do začátku lokálních proměnných */
	uint frame;  /* sub sp po uložení registrů */
};

#define CMP(X) \
	X(Cieq,       "eq", "ne") \
	X(Cine,       "ne", "eq") \
	X(Cisge,      "ge", "lt") \
	X(Cisgt,      "gt", "le") \
	X(Cisle,      "le", "gt") \
	X(Cislt,      "lt", "ge") \
	X(Ciuge,      "hs", "lo") \
	X(Ciugt,      "hi", "ls") \
	X(Ciule,      "ls", "hi") \
	X(Ciult,      "lo", "hs") \
	X(NCmpI+Cfeq, "eq", "ne") \
	X(NCmpI+Cfge, "ge", "lt") \
	X(NCmpI+Cfgt, "gt", "le") \
	X(NCmpI+Cfle, "ls", "hi") \
	X(NCmpI+Cflt, "mi", "pl") \
	X(NCmpI+Cfne, "ne", "eq") \
	X(NCmpI+Cfo,  "vc", "vs") \
	X(NCmpI+Cfuo, "vs", "vc")

static char *ctoa[][2] = {
#define X(c, s, n) [c] = {s, n},
	CMP(X)
#undef X
};

enum {
	Ki = -1, /* Kw */
	Ka = -2, /* všechny třídy */
};

/* Formát: %= %0 %1 operandy (třída instrukce), s předponou W (slovo),
 * S (float: s2n), D (double), l a h (dolní a horní s registru d), M
 * paměťový operand (legalizovaný předem). */
static struct {
	short op;
	short cls;
	char *fmt;
} omap[] = {
	{ Oadd,    Ki, "add %=, %0, %1" },
	{ Oadd,    Kd, "vadd.f64 %=, %0, %1" },
	{ Oadd,    Ks, "vadd.f32 %=, %0, %1" },
	{ Osub,    Ki, "sub %=, %0, %1" },
	{ Osub,    Kd, "vsub.f64 %=, %0, %1" },
	{ Osub,    Ks, "vsub.f32 %=, %0, %1" },
	{ Oneg,    Ki, "rsb %=, %0, #0" },
	{ Oneg,    Kd, "vneg.f64 %=, %0" },
	{ Oneg,    Ks, "vneg.f32 %=, %0" },
	{ Omul,    Ki, "mul %=, %0, %1" },
	{ Omul,    Kd, "vmul.f64 %=, %0, %1" },
	{ Omul,    Ks, "vmul.f32 %=, %0, %1" },
	{ Odiv,    Kd, "vdiv.f64 %=, %0, %1" },
	{ Odiv,    Ks, "vdiv.f32 %=, %0, %1" },
	{ Oaddc,   Ki, "adds %=, %0, %1" },
	{ Oadc,    Ki, "adc %=, %0, %1" },
	{ Osubc,   Ki, "subs %=, %0, %1" },
	{ Osbc,    Ki, "sbc %=, %0, %1" },
	{ Oand,    Ki, "and %=, %0, %1" },
	{ Oor,     Ki, "orr %=, %0, %1" },
	{ Oxor,    Ki, "eor %=, %0, %1" },
	{ Omulhu,  Ki, "umull ip, %=, %0, %1" },
	{ Oextsb,  Ki, "sxtb %=, %0" },
	{ Oextub,  Ki, "uxtb %=, %0" },
	{ Oextsh,  Ki, "sxth %=, %0" },
	{ Oextuh,  Ki, "uxth %=, %0" },
	{ Oexts,   Kd, "vcvt.f64.f32 %=, %S0" },
	{ Otruncd, Ks, "vcvt.f32.f64 %=, %D0" },
	{ Ostosi,  Ki, "vcvt.s32.f32 s14, %S0\n\tvmov %=, s14" },
	{ Ostoui,  Ki, "vcvt.u32.f32 s14, %S0\n\tvmov %=, s14" },
	{ Odtosi,  Ki, "vcvt.s32.f64 s14, %D0\n\tvmov %=, s14" },
	{ Odtoui,  Ki, "vcvt.u32.f64 s14, %D0\n\tvmov %=, s14" },
	{ Oswtof,  Kd, "vmov s14, %W0\n\tvcvt.f64.s32 %=, s14" },
	{ Oswtof,  Ks, "vmov s14, %W0\n\tvcvt.f32.s32 %=, s14" },
	{ Ouwtof,  Kd, "vmov s14, %W0\n\tvcvt.f64.u32 %=, s14" },
	{ Ouwtof,  Ks, "vmov s14, %W0\n\tvcvt.f32.u32 %=, s14" },
	{ Ocast,   Ki, "vmov %=, %S0" },
	{ Ocast,   Ks, "vmov %=, %W0" },
	{ Odlo,    Ki, "vmov %=, %l0" },
	{ Odhi,    Ki, "vmov %=, %h0" },
	{ Omkd,    Kd, "vmov %=, %W0, %W1" },
	{ Oacmp,   Ki, "cmp %0, %1" },
	{ Oacmn,   Ki, "cmn %0, %1" },
	{ Oafcmp,  Kd, "vcmp.f64 %0, %1\n\tvmrs APSR_nzcv, fpscr" },
	{ Oafcmp,  Ks, "vcmp.f32 %0, %1\n\tvmrs APSR_nzcv, fpscr" },
#define X(c, s, _) \
	{ Oflag+c, Ki, "mov %=, #0\n\tmov" s " %=, #1" },
	CMP(X)
#undef X
	{ NOp, 0, 0 }
};

static char *
rname(int r, int k)
{
	static char buf[8];

	if (r >= R0 && r <= R10)
		sprintf(buf, "r%d", r - R0);
	else if (r == FP)
		return "fp";
	else if (r == IP)
		return "ip";
	else if (r == SP)
		return "sp";
	else if (r == LR)
		return "lr";
	else if (r >= D0 && r <= D15) {
		if (k == Ks)
			sprintf(buf, "s%d", 2 * (r - D0));
		else
			sprintf(buf, "d%d", r - D0);
	} else
		die("arm: invalid register %d", r);
	return buf;
}

static int
slot(Ref r, E *e)
{
	int s;

	s = rsval(r);
	if (s < 0)
		return 4 * -s;
	return -(int)e->below - 4 * ((e->fn->slot + 1) & -2) + 4 * s;
}

/* Konstanta do registru rn. */
static void
loadimm(char *rn, uint32_t v, FILE *f)
{
	if (arm_imm(v))
		fprintf(f, "\tmov\t%s, #%u\n", rn, v);
	else if (arm_imm(~v))
		fprintf(f, "\tmvn\t%s, #%u\n", rn, ~v);
	else {
		fprintf(f, "\tmovw\t%s, #%u\n", rn, v & 0xffff);
		if (v >> 16)
			fprintf(f, "\tmovt\t%s, #%u\n", rn, v >> 16);
	}
}

static void
symname(Con *c, FILE *f)
{
	char *l, *p;

	if (c->sym.type != SGlo)
		die("arm: thread-local and external symbols are not supported");
	l = str(c->sym.id);
	p = l[0] == '"' ? "" : T.assym;
	fprintf(f, "%s%s", p, l);
	if (c->bits.i)
		fprintf(f, "%+d", (int32_t)c->bits.i);
}

static void
loadaddr(char *rn, Con *c, FILE *f)
{
	Con c0;
	int32_t off;

	/* addend relokace MOVW/MOVT je v instrukci, jen 16 bitů se znaménkem */
	c0 = *c;
	off = (int32_t)c->bits.i;
	if (off < -32768 || off > 32767)
		c0.bits.i = 0;
	fprintf(f, "\tmovw\t%s, #:lower16:", rn);
	symname(&c0, f);
	fprintf(f, "\n\tmovt\t%s, #:upper16:", rn);
	symname(&c0, f);
	fputc('\n', f);
	if (c0.bits.i != c->bits.i) {
		loadimm("ip", (uint32_t)off, f);
		fprintf(f, "\tadd\t%s, %s, ip\n", rn, rn);
	}
}

/* rd = base + off (base může být sp nebo fp) */
static void
addoff(char *rd, char *base, int off, FILE *f)
{
	if (off == 0) {
		if (strcmp(rd, base) != 0)
			fprintf(f, "\tmov\t%s, %s\n", rd, base);
	} else if (arm_imm(off))
		fprintf(f, "\tadd\t%s, %s, #%d\n", rd, base, off);
	else if (arm_imm(-off))
		fprintf(f, "\tsub\t%s, %s, #%d\n", rd, base, -off);
	else {
		loadimm("ip", (uint32_t)off, f);
		fprintf(f, "\tadd\t%s, %s, ip\n", rd, base);
	}
}

/* Paměťový operand: báze a posun; mimo rozsah instrukce se adresa spočte
 * do ip. lim: největší |posun|, al4: posun musí být násobek 4 (vldr). */
static void
memref(Ref r, int lim, int al4, char *out, E *e)
{
	char base[8];
	Mem *m;
	int off;

	off = 0;
	switch (rtype(r)) {
	case RTmp:
		assert(isreg(r));
		strcpy(base, rname(r.val, Kw));
		break;
	case RSlot:
		strcpy(base, "fp");
		off = slot(r, e);
		break;
	case RMem:
		m = &e->fn->mem[r.val];
		off = m->offset.type == CBits ? (int)m->offset.bits.i : 0;
		if (rtype(m->base) == RSlot) {
			strcpy(base, "fp");
			off += slot(m->base, e);
		} else {
			assert(isreg(m->base));
			strcpy(base, rname(m->base.val, Kw));
		}
		break;
	default:
		die("arm: invalid memory operand");
	}
	if (off < -lim || off > lim || (al4 && (off & 3))) {
		addoff("ip", base, off, e->f);
		strcpy(base, "ip");
		off = 0;
	}
	if (off)
		sprintf(out, "[%s, #%d]", base, off);
	else
		sprintf(out, "[%s]", base);
}

static void
emitf(char *s, Ins *i, E *e)
{
	Ref r;
	int k, c;
	Con *pc;

	fputc('\t', e->f);
	for (;;) {
		k = i->cls;
		while ((c = *s++) != '%')
			if (!c) {
				fputc('\n', e->f);
				return;
			} else
				fputc(c, e->f);
	Switch:
		switch ((c = *s++)) {
		default:
			die("invalid escape");
		case 'W':
			k = Kw;
			goto Switch;
		case 'S':
			k = Ks;
			goto Switch;
		case 'D':
			k = Kd;
			goto Switch;
		case 'l':
		case 'h':
			r = *s++ == '0' ? i->arg[0] : i->arg[1];
			assert(isreg(r) && r.val >= D0);
			fprintf(e->f, "s%d", 2 * (r.val - D0) + (c == 'h'));
			break;
		case '=':
		case '0':
			r = c == '=' ? i->to : i->arg[0];
			assert(isreg(r));
			fputs(rname(r.val, k), e->f);
			break;
		case '1':
			r = i->arg[1];
			switch (rtype(r)) {
			default:
				die("invalid second argument");
			case RTmp:
				assert(isreg(r));
				fputs(rname(r.val, k), e->f);
				break;
			case RCon:
				pc = &e->fn->con[r.val];
				assert(pc->type == CBits);
				fprintf(e->f, "#%d", (int32_t)pc->bits.i);
				break;
			}
			break;
		}
	}
}

static void emitins(Ins *, E *);

/* Načtení/uložení: op a třída, registr a paměťový operand. */
static void
ldst(char *op, Ref reg, int k, Ref mem, int lim, int al4, E *e)
{
	char m[64];

	memref(mem, lim, al4, m, e);
	fprintf(e->f, "\t%s\t%s, %s\n", op, rname(reg.val, k), m);
}

static void
emitload(Ins *i, E *e)
{
	int k;

	k = i->cls;
	switch (i->op) {
	case Oload:
	case Oloadsw:
	case Oloaduw:
		if (k == Kd)
			ldst("vldr", i->to, Kd, i->arg[0], 1020, 1, e);
		else if (k == Ks)
			ldst("vldr", i->to, Ks, i->arg[0], 1020, 1, e);
		else
			ldst("ldr", i->to, Kw, i->arg[0], 4095, 0, e);
		break;
	case Oloadsb: ldst("ldrsb", i->to, Kw, i->arg[0], 255, 0, e); break;
	case Oloadub: ldst("ldrb", i->to, Kw, i->arg[0], 4095, 0, e); break;
	case Oloadsh: ldst("ldrsh", i->to, Kw, i->arg[0], 255, 0, e); break;
	case Oloaduh: ldst("ldrh", i->to, Kw, i->arg[0], 255, 0, e); break;
	default: die("unreachable");
	}
}

static void
emitstore(Ins *i, E *e)
{
	switch (i->op) {
	case Ostoreb: ldst("strb", i->arg[0], Kw, i->arg[1], 4095, 0, e); break;
	case Ostoreh: ldst("strh", i->arg[0], Kw, i->arg[1], 255, 0, e); break;
	case Ostorew: ldst("str", i->arg[0], Kw, i->arg[1], 4095, 0, e); break;
	case Ostores: ldst("vstr", i->arg[0], Ks, i->arg[1], 1020, 1, e); break;
	case Ostored: ldst("vstr", i->arg[0], Kd, i->arg[1], 1020, 1, e); break;
	default: die("unreachable");
	}
}

static void
emitcopy(Ins *i, E *e)
{
	Con *c;
	Ins t;
	int k;

	k = i->cls;
	if (req(i->to, i->arg[0]) || req(i->to, R))
		return;
	if (rtype(i->to) == RSlot) {
		/* do paměti; z paměti nebo konstanty přes ip/d7 */
		t = *i;
		if (!isreg(i->arg[0])) {
			t.to = TMP(KBASE(k) == 0 ? IP : D7);
			emitcopy(&t, e);
			t.arg[0] = t.to;
		}
		t.op = KBASE(k) == 0 ? Ostorew : (k == Kd ? Ostored : Ostores);
		t.arg[1] = i->to;
		emitstore(&t, e);
		return;
	}
	assert(isreg(i->to));
	switch (rtype(i->arg[0])) {
	case RCon:
		c = &e->fn->con[i->arg[0].val];
		assert(KBASE(k) == 0);
		if (c->type == CAddr)
			loadaddr(rname(i->to.val, Kw), c, e->f);
		else
			loadimm(rname(i->to.val, Kw), (uint32_t)c->bits.i, e->f);
		break;
	case RSlot:
	case RMem:
		t = *i;
		t.op = Oload;
		emitload(&t, e);
		break;
	default:
		assert(isreg(i->arg[0]));
		if (KBASE(k) == 0)
			fprintf(e->f, "\tmov\t%s, ", rname(i->to.val, Kw));
		else
			fprintf(e->f, "\tvmov.f%d\t%s, ", k == Kd ? 64 : 32, rname(i->to.val, k));
		fprintf(e->f, "%s\n", rname(i->arg[0].val, k));
		break;
	}
}

static void
emitshift(Ins *i, E *e)
{
	static char *name[] = {[Osar] = "asr", [Oshr] = "lsr", [Oshl] = "lsl"};
	int n;
	char *rd;

	rd = rname(i->to.val, Kw);
	fprintf(e->f, "\t%s\t%s, ", name[i->op], rd);
	fprintf(e->f, "%s, ", rname(i->arg[0].val, Kw));
	if (rtype(i->arg[1]) == RCon) {
		n = e->fn->con[i->arg[1].val].bits.i & 31;
		if (n == 0) {
			/* posun o 0: kopie */
			fprintf(e->f, "#0\n");
			return;
		}
		fprintf(e->f, "#%d\n", n);
		return;
	}
	/* posun o registr bere dolní bajt; IR počítá velikost mod 32 */
	fprintf(e->f, "ip\n");
}

static void
emitins(Ins *i, E *e)
{
	Ins t;
	Con *c;
	char m[64];
	int o, k;

	switch (i->op) {
	default:
		if (isload(i->op)) {
			emitload(i, e);
			break;
		}
		if (isstore(i->op)) {
			emitstore(i, e);
			break;
		}
		for (o=0;; o++) {
			if (omap[o].op == NOp)
				die("no match for %s(%c)",
					optab[i->op].name, "wlsd"[i->cls]);
			if (omap[o].op == i->op)
			if (omap[o].cls == i->cls || omap[o].cls == Ka
			|| (omap[o].cls == Ki && KBASE(i->cls) == 0))
				break;
		}
		emitf(omap[o].fmt, i, e);
		break;
	case Onop:
		break;
	case Ocopy:
		emitcopy(i, e);
		break;
	case Osar:
	case Oshr:
	case Oshl:
		if (rtype(i->arg[1]) != RCon)
			fprintf(e->f, "\tand\tip, %s, #31\n", rname(i->arg[1].val, Kw));
		if (rtype(i->arg[1]) == RCon
		&& (e->fn->con[i->arg[1].val].bits.i & 31) == 0) {
			t = *i;
			t.op = Ocopy;
			emitcopy(&t, e);
			break;
		}
		emitshift(i, e);
		break;
	case Oswap:
		k = i->cls;
		if (KBASE(k) == 0) {
			fprintf(e->f, "\tmov\tip, %s\n", rname(i->arg[0].val, Kw));
			fprintf(e->f, "\tmov\t%s, ", rname(i->arg[0].val, Kw));
			fprintf(e->f, "%s\n", rname(i->arg[1].val, Kw));
			fprintf(e->f, "\tmov\t%s, ip\n", rname(i->arg[1].val, Kw));
		} else {
			t = (Ins){.op = Ocopy, .cls = k, .to = TMP(D7), .arg = {i->arg[0]}};
			emitcopy(&t, e);
			t = (Ins){.op = Ocopy, .cls = k, .to = i->arg[0], .arg = {i->arg[1]}};
			emitcopy(&t, e);
			t = (Ins){.op = Ocopy, .cls = k, .to = i->arg[1], .arg = {TMP(D7)}};
			emitcopy(&t, e);
		}
		break;
	case Oaddr:
		assert(rtype(i->arg[0]) == RSlot);
		strcpy(m, rname(i->to.val, Kw));
		addoff(m, "fp", slot(i->arg[0], e), e->f);
		break;
	case Ocall:
		if (rtype(i->arg[0]) == RCon) {
			c = &e->fn->con[i->arg[0].val];
			if (c->type != CAddr || c->bits.i)
				die("invalid call argument");
			fputs("\tbl\t", e->f);
			symname(c, e->f);
			fputc('\n', e->f);
		} else
			fprintf(e->f, "\tblx\t%s\n", rname(i->arg[0].val, Kw));
		break;
	case Odbgloc:
		emitdbgloc(i->arg[0].val, i->arg[1].val, e->f);
		break;
	}
}

static void
framelayout(E *e)
{
	int *r, top;
	uint locals, pad;

	e->nr = 0;
	top = 0;
	for (r=arm_rclob; *r>=0; r++)
		if (e->fn->reg & BIT(*r)) {
			if (*r <= R10)
				e->nr++;
			else if (*r - D8 + 1 > top)
				top = *r - D8 + 1;
		}
	e->nd = top;
	e->below = 4 * e->nr + 8 * e->nd;
	locals = 4 * ((e->fn->slot + 1) & -2);
	pad = (4 * e->nr) & 4;
	e->frame = locals + pad + arm_argsz;
}

/* r4-r10 podle masky, fp a lr */
static void
reglist(E *e, char *last)
{
	int *r;

	fputc('{', e->f);
	for (r=arm_rclob; *r>=0 && *r<=R10; r++)
		if (e->fn->reg & BIT(*r))
			fprintf(e->f, "%s, ", rname(*r, Kw));
	fprintf(e->f, "fp, %s}\n", last);
}

void
arm_emitfn(Fn *fn, FILE *out)
{
	static int id0, head;
	int c, n, lbl;
	Blk *b, *t;
	Ins *i;
	E *e;

	e = &(E){.f = out, .fn = fn};
	if (!head) {
		fputs(".syntax unified\n.arm\n.fpu vfpv3-d16\n", out);
		head = 1;
	}
	fn->lnk.align = 4;
	emitfnlnk(fn->name, &fn->lnk, out);
	framelayout(e);
	fputs("\tpush\t", out);
	reglist(e, "lr");
	fprintf(out, "\tadd\tfp, sp, #%d\n", 4 * e->nr);
	if (e->nd == 1)
		fputs("\tvpush\t{d8}\n", out);
	else if (e->nd)
		fprintf(out, "\tvpush\t{d8-d%d}\n", 8 + e->nd - 1);
	if (e->frame)
		addoff("sp", "sp", -(int)e->frame, out);

	for (lbl=0, b=fn->start; b; b=b->link) {
		if (lbl || b->npred > 1)
			fprintf(out, "%s%d:\n", T.asloc, id0+b->id);
		for (i=b->ins; i!=&b->ins[b->nins]; i++)
			emitins(i, e);
		lbl = 1;
		switch (b->jmp.type) {
		case Jhlt:
			fputs("\tudf\t#0\n", out);
			break;
		case Jret0:
			if (e->nd) {
				addoff("sp", "fp", -(int)e->below, out);
				if (e->nd == 1)
					fputs("\tvpop\t{d8}\n", out);
				else
					fprintf(out, "\tvpop\t{d8-d%d}\n", 8 + e->nd - 1);
			} else
				addoff("sp", "fp", -(int)(4 * e->nr), out);
			fputs("\tpop\t", out);
			reglist(e, "pc");
			break;
		case Jjmp:
		Jmp:
			if (b->s1 != b->link)
				fprintf(out, "\tb\t%s%d\n", T.asloc, id0+b->s1->id);
			else
				lbl = 0;
			break;
		default:
			c = b->jmp.type - Jjf;
			if (c < 0 || c > NCmp)
				die("unhandled jump %d", b->jmp.type);
			if (b->link == b->s2) {
				t = b->s1;
				b->s1 = b->s2;
				b->s2 = t;
				n = 0;
			} else
				n = 1;
			fprintf(out, "\tb%s\t%s%d\n", ctoa[c][n], T.asloc, id0+b->s2->id);
			goto Jmp;
		}
	}
	id0 += fn->nblk;
	elf_emitfnfin(fn->name, out);
}

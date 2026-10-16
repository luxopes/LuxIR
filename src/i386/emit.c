#include "all.h"

/* Assembler i386 (AT&T, GNU as) podle amd64/emit.c.
 *
 * Rámec (ebp je vždy ukazatel rámce):
 *
 *   [ebp+8 ...]   parametry (SLOT(-2) ...)
 *   [ebp+4]       návratová adresa
 *   [ebp]         uložený ebp
 *   [ebp-8, ebp)  pomocné místo pro převod st(0) <-> xmm0
 *   lokální proměnné a odložené hodnoty (4*F bajtů, F zarovnané na 4)
 *   uložené ebx, esi, edi
 *   výplň
 *   [esp ...]     argumenty volání (i386_argsz)
 *
 * Při volání je esp zarovnaný na 16 bajtů. */

typedef struct E E;

struct E {
	FILE *f;
	Fn *fn;
	uint fsz;
	uint out;
	int nclob;
};

#define CMP(X) \
	X(Ciule,      "be", "a") \
	X(Ciult,      "b", "ae") \
	X(Cisle,      "le", "g") \
	X(Cislt,      "l", "ge") \
	X(Cisgt,      "g", "le") \
	X(Cisge,      "ge", "l") \
	X(Ciugt,      "a", "be") \
	X(Ciuge,      "ae", "b") \
	X(Cieq,       "z", "nz") \
	X(Cine,       "nz", "z") \
	X(NCmpI+Cfle, "?" , "?") \
	X(NCmpI+Cflt, "?",  "?") \
	X(NCmpI+Cfgt, "a", "be") \
	X(NCmpI+Cfge, "ae", "b") \
	X(NCmpI+Cfo,  "np", "p") \
	X(NCmpI+Cfuo, "p", "np")

enum {
	SWord = 0,
	SShort = 1,
	SByte = 2,

	Ki = -1, /* Kw */
	Ka = -2, /* všechny třídy */
};

/* Formát jako u amd64: - a + na začátku převede tříadresovou instrukci na
 * dvouadresovou (+ navíc komutativní), %k přípona třídy, %0 %1 %= operandy
 * s předponou M (paměť), W, H, B (32, 16, 8 bitů), S, D (float, double). */
static struct {
	short op;
	short cls;
	char *fmt;
} omap[] = {
	{ Oadd,     Ka, "+add%k %1, %=" },
	{ Osub,     Ka, "-sub%k %1, %=" },
	{ Oaddc,    Ki, "+addl %1, %=" },
	{ Oadc,     Ki, "+adcl %1, %=" },
	{ Oand,     Ki, "+and%k %1, %=" },
	{ Oor,      Ki, "+or%k %1, %=" },
	{ Oxor,     Ki, "+xor%k %1, %=" },
	{ Osar,     Ki, "-sar%k %B1, %=" },
	{ Oshr,     Ki, "-shr%k %B1, %=" },
	{ Oshl,     Ki, "-shl%k %B1, %=" },
	{ Omul,     Ki, "+imul%k %1, %=" },
	{ Omul,     Ks, "+mulss %1, %=" },
	{ Omul,     Kd, "+mulsd %1, %=" },
	{ Odiv,     Ka, "-div%k %1, %=" },
	{ Ostorew,  Ka, "movl %W0, %M1" },
	{ Ostoreh,  Ka, "movw %H0, %M1" },
	{ Ostoreb,  Ka, "movb %B0, %M1" },
	{ Ostores,  Ka, "movss %S0, %M1" },
	{ Ostored,  Ka, "movsd %D0, %M1" },
	{ Oload,    Ka, "mov%k %M0, %=" },
	{ Oloadsw,  Ki, "movl %M0, %W=" },
	{ Oloaduw,  Ki, "movl %M0, %W=" },
	{ Oloadsh,  Ki, "movswl %M0, %=" },
	{ Oloaduh,  Ki, "movzwl %M0, %=" },
	{ Oloadsb,  Ki, "movsbl %M0, %=" },
	{ Oloadub,  Ki, "movzbl %M0, %=" },
	{ Oextsh,   Ki, "movswl %H0, %=" },
	{ Oextuh,   Ki, "movzwl %H0, %=" },
	{ Oextsb,   Ki, "movsbl %B0, %=" },
	{ Oextub,   Ki, "movzbl %B0, %=" },

	{ Oexts,    Kd, "cvtss2sd %0, %=" },
	{ Otruncd,  Ks, "cvtsd2ss %0, %=" },
	{ Ostosi,   Ki, "cvttss2si %0, %=" },
	{ Odtosi,   Ki, "cvttsd2si %0, %=" },
	{ Oswtof,   Ka, "cvtsi2%k %W0, %=" },
	{ Ocast,    Ki, "movd %S0, %W=" },
	{ Ocast,    Ks, "movd %W0, %S=" },
	{ Odlo,     Ki, "movd %D0, %W=" },
	{ Odhi,     Ki, "pshufd $1, %D0, %%xmm7\n\tmovd %%xmm7, %W=" },
	{ Omkd,     Kd, "movd %W0, %=\n\tmovd %W1, %%xmm7\n\tpunpckldq %%xmm7, %=" },

	{ Oaddr,    Ki, "leal %M0, %=" },
	{ Oswap,    Ki, "xchgl %0, %1" },
	{ Osign,    Ki, "cltd" },
	{ Oxdiv,    Ki, "divl %0" },
	{ Oxidiv,   Ki, "idivl %0" },
	{ Omulhu,   Ki, "mull %0" },
	{ Oxcmp,    Ks, "ucomiss %S0, %S1" },
	{ Oxcmp,    Kd, "ucomisd %D0, %D1" },
	{ Oxcmp,    Ki, "cmpl %0, %1" },
	{ Oxtest,   Ki, "testl %0, %1" },
#define X(c, s, _) \
	{ Oflag+c,  Ki, "set" s " %B=\n\tmovzbl %B=, %=" },
	CMP(X)
#undef X
	{ Oflagfeq, Ki, "setz %B=\n\tmovzbl %B=, %=" },
	{ Oflagfne, Ki, "setnz %B=\n\tmovzbl %B=, %=" },
	{ NOp, 0, 0 }
};

static char *ctoa[][2] = {
#define X(c, s, n) [c] = {s, n},
	CMP(X)
#undef X
	[NCmpI+Cfeq] = {"z", "nz"},
	[NCmpI+Cfne] = {"nz", "z"},
};

static char *rname[][3] = {
	[EAX] = {"eax", "ax", "al"},
	[EBX] = {"ebx", "bx", "bl"},
	[ECX] = {"ecx", "cx", "cl"},
	[EDX] = {"edx", "dx", "dl"},
	[ESI] = {"esi", "si", 0},
	[EDI] = {"edi", "di", 0},
	[EBP] = {"ebp", "bp", 0},
	[ESP] = {"esp", "sp", 0},
};

static int
bytereg(int r)
{
	return r == EAX || r == EBX || r == ECX || r == EDX;
}

static int
slot(Ref r, E *e)
{
	int s;

	s = rsval(r);
	assert(s <= e->fn->slot);
	if (s < 0)
		return 4 * -s;
	return -(int)e->fsz + 4 * s;
}

static void
emitcon(Con *con, E *e)
{
	char *p, *l;

	switch (con->type) {
	case CAddr:
		if (con->sym.type != SGlo)
			die("i386: thread-local and external symbols are not supported");
		l = str(con->sym.id);
		p = l[0] == '"' ? "" : T.assym;
		fprintf(e->f, "%s%s", p, l);
		if (con->bits.i)
			fprintf(e->f, "%+"PRId64, (int64_t)(int32_t)con->bits.i);
		break;
	case CBits:
		fprintf(e->f, "%"PRId64, (int64_t)(int32_t)con->bits.i);
		break;
	default:
		die("unreachable");
	}
}

static char *
regtoa(int reg, int sz)
{
	static char buf[6];

	assert(reg <= XMM7);
	if (reg >= XMM0) {
		sprintf(buf, "xmm%d", reg-XMM0);
		return buf;
	}
	if (!rname[reg][sz])
		die("i386: no byte register for %s", rname[reg][0]);
	return rname[reg][sz];
}

static Ref
getarg(char c, Ins *i)
{
	switch (c) {
	case '0':
		return i->arg[0];
	case '1':
		return i->arg[1];
	case '=':
		return i->to;
	default:
		die("invalid arg letter %c", c);
	}
}

static void emitins(Ins, E *);

static void
emitcopy(Ref r1, Ref r2, int k, E *e)
{
	Ins icp;

	icp.op = Ocopy;
	icp.arg[0] = r2;
	icp.to = r1;
	icp.cls = k;
	emitins(icp, e);
}

static void
emitmem(Ref ref, E *e)
{
	Mem *m;
	Con off;

	m = &e->fn->mem[ref.val];
	if (rtype(m->base) == RSlot) {
		off.type = CBits;
		off.bits.i = slot(m->base, e);
		addcon(&m->offset, &off, 1);
		m->base = TMP(EBP);
	}
	if (m->offset.type != CUndef)
		emitcon(&m->offset, e);
	if (req(m->base, R) && req(m->index, R)) {
		if (m->offset.type == CUndef)
			fputc('0', e->f);
		return;
	}
	fputc('(', e->f);
	if (!req(m->base, R))
		fprintf(e->f, "%%%s", regtoa(m->base.val, SWord));
	if (!req(m->index, R))
		fprintf(e->f, ", %%%s, %d", regtoa(m->index.val, SWord), m->scale);
	fputc(')', e->f);
}

static void
emitf(char *s, Ins *i, E *e)
{
	static char clstoa[][3] = {"l", "?", "ss", "sd"};
	char c;
	int sz;
	Ref ref;
	Con off;

	switch (*s) {
	case '+':
		if (req(i->arg[1], i->to)) {
			ref = i->arg[0];
			i->arg[0] = i->arg[1];
			i->arg[1] = ref;
		}
		/* fall through */
	case '-':
		assert((!req(i->arg[1], i->to) || req(i->arg[0], i->to)) &&
			"cannot convert to 2-address");
		emitcopy(i->to, i->arg[0], i->cls, e);
		s++;
		break;
	}

	fputc('\t', e->f);
Next:
	while ((c = *s++) != '%')
		if (!c) {
			fputc('\n', e->f);
			return;
		} else
			fputc(c, e->f);
	switch ((c = *s++)) {
	case '%':
		fputc('%', e->f);
		break;
	case 'k':
		fputs(clstoa[i->cls], e->f);
		break;
	case '0':
	case '1':
	case '=':
		sz = SWord;
		s--;
		goto Ref;
	case 'D':
	case 'S':
		sz = SWord; /* u desetinných čísel bez významu */
	Ref:
		c = *s++;
		ref = getarg(c, i);
		switch (rtype(ref)) {
		case RTmp:
			assert(isreg(ref));
			fprintf(e->f, "%%%s", regtoa(ref.val, sz));
			break;
		case RSlot:
			fprintf(e->f, "%d(%%ebp)", slot(ref, e));
			break;
		case RMem:
			emitmem(ref, e);
			break;
		case RCon:
			fputc('$', e->f);
			emitcon(&e->fn->con[ref.val], e);
			break;
		default:
			die("unreachable");
		}
		break;
	case 'W':
		sz = SWord;
		goto Ref;
	case 'H':
		sz = SShort;
		goto Ref;
	case 'B':
		sz = SByte;
		goto Ref;
	case 'M':
		c = *s++;
		ref = getarg(c, i);
		switch (rtype(ref)) {
		case RMem:
			emitmem(ref, e);
			break;
		case RSlot:
			fprintf(e->f, "%d(%%ebp)", slot(ref, e));
			break;
		case RCon:
			off = e->fn->con[ref.val];
			emitcon(&off, e);
			break;
		case RTmp:
			assert(isreg(ref));
			fprintf(e->f, "(%%%s)", regtoa(ref.val, SWord));
			break;
		default:
			die("unreachable");
		}
		break;
	default:
		die("invalid format specifier %%%c", c);
	}
	goto Next;
}

static bits negmask[4] = {
	[Ks] = 0x80000000,
	[Kd] = 0x8000000000000000,
};

/* Registr bez 8bitové části ve storeb: hodnota se dočasně vymění
 * s registrem, který ji má a v adrese se nepoužívá. */
static void
storebyte(Ins i, E *e)
{
	static int cand[] = {EAX, ECX, EDX, EBX};
	Mem *m;
	int r, s, n, base, index;

	r = i.arg[0].val;
	base = index = -1;
	if (rtype(i.arg[1]) == RMem) {
		m = &e->fn->mem[i.arg[1].val];
		if (rtype(m->base) == RTmp)
			base = m->base.val;
		if (rtype(m->index) == RTmp)
			index = m->index.val;
	} else if (rtype(i.arg[1]) == RTmp)
		base = i.arg[1].val;
	s = -1;
	for (n=0; n<4; n++)
		if (cand[n] != base && cand[n] != index) {
			s = cand[n];
			break;
		}
	assert(s >= 0);
	/* po výměně je hodnota r v s a obsah s v r */
	if (rtype(i.arg[1]) == RMem) {
		m = &e->fn->mem[i.arg[1].val];
		if (base == r)
			m->base = TMP(s);
		if (index == r)
			m->index = TMP(s);
	} else if (base == r)
		i.arg[1] = TMP(s);
	fprintf(e->f, "\txchgl %%%s, %%%s\n", rname[r][0], rname[s][0]);
	i.arg[0] = TMP(s);
	emitf("movb %B0, %M1", &i, e);
	fprintf(e->f, "\txchgl %%%s, %%%s\n", rname[r][0], rname[s][0]);
	if (rtype(i.arg[1]) == RMem) {
		m = &e->fn->mem[i.arg[1].val];
		if (base == r)
			m->base = TMP(r);
		if (index == r)
			m->index = TMP(r);
	}
}

static void
emitins(Ins i, E *e)
{
	Ref r;
	int64_t val;
	int o, t0, c;
	Ins ineg;
	Con *con;

	switch (i.op) {
	default:
		if (i.op >= Oflag && i.op <= Oflag1
		&& isreg(i.to) && !bytereg(i.to.val)) {
			/* setcc jen do registrů s 8bitovou částí */
			c = i.op - Oflag;
			fprintf(e->f, "\tmovl $0, %%%s\n", rname[i.to.val][0]);
			fprintf(e->f, "\tj%s 1f\n", ctoa[c][1]);
			fprintf(e->f, "\tmovl $1, %%%s\n1:\n", rname[i.to.val][0]);
			break;
		}
	Table:
		for (o=0;; o++) {
			if (omap[o].op == NOp)
				die("no match for %s(%c)",
					optab[i.op].name, "wlsd"[i.cls]);
			if (omap[o].op == i.op)
			if (omap[o].cls == i.cls
			|| (omap[o].cls == Ki && KBASE(i.cls) == 0)
			|| (omap[o].cls == Ka))
				break;
		}
		emitf(omap[o].fmt, &i, e);
		break;
	case Onop:
		break;
	case Ostoreb:
		if (isreg(i.arg[0]) && !bytereg(i.arg[0].val)) {
			storebyte(i, e);
			break;
		}
		goto Table;
	case Oextsb:
	case Oextub:
		if (isreg(i.arg[0]) && !bytereg(i.arg[0].val)) {
			if (!req(i.to, i.arg[0]))
				emitf("movl %0, %=", &i, e);
			if (i.op == Oextub)
				emitf("andl $255, %=", &i, e);
			else {
				emitf("shll $24, %=", &i, e);
				emitf("sarl $24, %=", &i, e);
			}
			break;
		}
		goto Table;
	case Omul:
		/* tříadresová forma imul s konstantou */
		if (rtype(i.arg[1]) == RCon) {
			r = i.arg[0];
			i.arg[0] = i.arg[1];
			i.arg[1] = r;
		}
		if (KBASE(i.cls) == 0
		&& rtype(i.arg[0]) == RCon
		&& rtype(i.arg[1]) == RTmp) {
			emitf("imull %0, %1, %=", &i, e);
			break;
		}
		goto Table;
	case Osubc:
	case Osbc:
		/* příznak přenosu: bez triku s neg; když je cíl odečítanec,
		 * jde odečítanec přes pomocné místo -4(%ebp) */
		if (req(i.to, i.arg[1]) && !req(i.arg[0], i.to)) {
			emitf("movl %1, -4(%%ebp)", &i, e);
			emitf("movl %0, %=", &i, e);
			emitf(i.op == Osubc ? "subl -4(%%ebp), %=" : "sbbl -4(%%ebp), %=", &i, e);
			break;
		}
		emitf(i.op == Osubc ? "-subl %1, %=" : "-sbbl %1, %=", &i, e);
		break;
	case Osub:
		if (req(i.to, i.arg[1]) && !req(i.arg[0], i.to)) {
			ineg = (Ins){Oneg, i.cls, i.to, {i.to}};
			emitins(ineg, e);
			emitf("add%k %0, %=", &i, e);
			break;
		}
		goto Table;
	case Oneg:
		if (!req(i.to, i.arg[0]))
			emitf("mov%k %0, %=", &i, e);
		if (KBASE(i.cls) == 0)
			emitf("negl %=", &i, e);
		else
			fprintf(e->f,
				"\txorp%c %sfp%d, %%%s\n",
				"xxsd"[i.cls],
				T.asloc,
				stashbits(negmask[i.cls], 16),
				regtoa(i.to.val, SWord)
			);
		break;
	case Odiv:
		/* xmm7, když převod na dvouadresovou formu nejde */
		if (req(i.to, i.arg[1])) {
			i.arg[1] = TMP(XMM7);
			emitf("mov%k %=, %1", &i, e);
			emitf("mov%k %0, %=", &i, e);
			i.arg[0] = i.to;
		}
		goto Table;
	case Ocopy:
		assert(rtype(i.to) != RMem);
		if (req(i.to, R) || req(i.arg[0], R))
			break;
		if (req(i.to, i.arg[0]))
			break;
		t0 = rtype(i.arg[0]);
		if (KBASE(i.cls) == 0 && t0 == RCon) {
			val = e->fn->con[i.arg[0].val].bits.i;
			(void)val;
			emitf("movl %0, %=", &i, e);
			break;
		}
		if (rtype(i.to) == RSlot
		&& (t0 == RSlot || t0 == RMem)) {
			i.cls = i.cls == Kd ? Kd : Ks;
			i.arg[1] = TMP(XMM7);
			emitf("mov%k %0, %1", &i, e);
			emitf("mov%k %1, %=", &i, e);
			break;
		}
		emitf("mov%k %0, %=", &i, e);
		break;
	case Oaddr:
		if (rtype(i.arg[0]) == RCon) {
			emitf("movl %0, %=", &i, e);
			break;
		}
		goto Table;
	case Ocall:
		switch (rtype(i.arg[0])) {
		case RCon:
			con = &e->fn->con[i.arg[0].val];
			fprintf(e->f, "\tcalll ");
			emitcon(con, e);
			fprintf(e->f, "\n");
			break;
		case RTmp:
			emitf("calll *%W0", &i, e);
			break;
		default:
			die("invalid call argument");
		}
		/* desetinný výsledek z st(0) do xmm0 */
		if (rtype(i.arg[1]) == RCall && (i.arg[1].val & 4))
			fputs("\tfstpl -8(%ebp)\n\tmovsd -8(%ebp), %xmm0\n", e->f);
		if (rtype(i.arg[1]) == RCall && (i.arg[1].val & 8))
			fputs("\tfstps -8(%ebp)\n\tmovss -8(%ebp), %xmm0\n", e->f);
		break;
	case Oswap:
		if (KBASE(i.cls) == 0)
			goto Table;
		emitcopy(TMP(XMM7), i.arg[0], i.cls, e);
		emitcopy(i.arg[0], i.arg[1], i.cls, e);
		emitcopy(i.arg[1], TMP(XMM7), i.cls, e);
		break;
	case Odbgloc:
		emitdbgloc(i.arg[0].val, i.arg[1].val, e->f);
		break;
	}
}

static void
framesz(E *e)
{
	uint f, push;
	int *r;

	e->nclob = 0;
	for (r=i386_rclob; *r>=0; r++)
		if (e->fn->reg & BIT(*r))
			e->nclob++;
	f = (e->fn->slot + 3) & -4;
	e->fsz = 8 + 4*f;
	push = 4 * e->nclob;
	e->out = ((push + i386_argsz + 15) & -16) - push;
}

void
i386_emitfn(Fn *fn, FILE *f)
{
	static int id0;
	Blk *b, *s;
	Ins *i;
	int *r, c, n, lbl;
	uint p;
	E *e;

	e = &(E){.f = f, .fn = fn};
	emitfnlnk(fn->name, &fn->lnk, f);
	framesz(e);
	fputs("\tpushl %ebp\n\tmovl %esp, %ebp\n", f);
	fprintf(f, "\tsubl $%u, %%esp\n", e->fsz);
	for (r=i386_rclob; *r>=0; r++)
		if (fn->reg & BIT(*r))
			fprintf(f, "\tpushl %%%s\n", rname[*r][0]);
	if (e->out)
		fprintf(f, "\tsubl $%u, %%esp\n", e->out);

	for (lbl=0, b=fn->start; b; b=b->link) {
		if (lbl || b->npred > 1) {
			for (p=0; p<b->npred; p++)
				if (b->pred[p]->id >= b->id)
					break;
			if (p != b->npred)
				fprintf(f, ".p2align 4\n");
			fprintf(f, "%sbb%d:\n", T.asloc, id0+b->id);
		}
		for (i=b->ins; i!=&b->ins[b->nins]; i++)
			emitins(*i, e);
		lbl = 1;
		switch (b->jmp.type) {
		case Jhlt:
			fprintf(f, "\tud2\n");
			break;
		case Jret0:
			/* desetinný výsledek z xmm0 do st(0) */
			if (i386_fret == 4)
				fputs("\tmovsd %xmm0, -8(%ebp)\n\tfldl -8(%ebp)\n", f);
			if (i386_fret == 8)
				fputs("\tmovss %xmm0, -8(%ebp)\n\tflds -8(%ebp)\n", f);
			if (e->nclob) {
				fprintf(f, "\tleal %d(%%ebp), %%esp\n",
					-(int)(e->fsz + 4*e->nclob));
				for (r=&i386_rclob[NCLR]; r>i386_rclob;)
					if (fn->reg & BIT(*--r))
						fprintf(f, "\tpopl %%%s\n", rname[*r][0]);
			}
			fputs("\tleave\n\tret\n", f);
			break;
		case Jjmp:
		Jmp:
			if (b->s1 != b->link)
				fprintf(f, "\tjmp %sbb%d\n",
					T.asloc, id0+b->s1->id);
			else
				lbl = 0;
			break;
		default:
			c = b->jmp.type - Jjf;
			if (0 <= c && c <= NCmp) {
				if (b->link == b->s2) {
					s = b->s1;
					b->s1 = b->s2;
					b->s2 = s;
					n = 0;
				} else
					n = 1;
				fprintf(f, "\tj%s %sbb%d\n", ctoa[c][n],
					T.asloc, id0+b->s2->id);
				goto Jmp;
			}
			die("unhandled jump %d", b->jmp.type);
		}
	}
	id0 += fn->nblk;
	elf_emitfnfin(fn->name, f);
}

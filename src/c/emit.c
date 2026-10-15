#include "../all.h"

/* C (c32): přenositelné C99 s rozšířeními GNU pro clang a gcc na 32bitových
 * cílech.
 *
 * Hodnoty mají třídy jako jinde: Kw je uint32_t, Kl uint64_t, Ks float,
 * Kd double. Adresy jsou 64bitová slova a před přístupem do paměti se
 * zkrátí na šířku ukazatele cíle (jako u wasm32). Přístupy do paměti jdou
 * přes __builtin_memcpy (bez předpokladů o zarovnání a aliasingu), operace
 * s nedefinovaným chováním v C (dělení INT_MIN / -1 a nulou, převod
 * desetinného čísla mimo rozsah, velikost posunu) obstarají pomocné funkce
 * s chováním RISC-V.
 *
 * Funkce mají C prototypy podle tříd parametrů a výsledku, volání jde přes
 * přetypovaný ukazatel podle tříd argumentů v místě volání, takže sedí ABI
 * cíle i pro cizí (a variadické) funkce. Jména symbolů zachovává
 * __asm__("jméno"); v C se jmenují sN podle indexu jména.
 *
 * Data jsou zabalené struktury s přesnými posuny: bajty, nuly a odkazy na
 * symboly (slovo ukazatele, u 8bajtového odkazu s nulovou horní polovinou).
 *
 * Pořadí výstupu: pomocné funkce, prototypy funkcí a deklarace cizích
 * symbolů, typy a deklarace dat, data, těla funkcí. Těla se během překladu
 * zapisují do pomocného souboru (výstup.body) a připojí se na konci, proto
 * cíl potřebuje -o soubor.
 *
 * Konzervativní GC prochází nativní zásobník a registry (vrstva
 * architektury), stínový zásobník jako u wasm tu není potřeba. */

typedef struct CSym CSym;
typedef struct CField CField;
typedef struct CObj CObj;

struct CSym {
	uint32_t id;
	char func;     /* funkce definovaná v modulu */
	char data;     /* data definovaná v modulu */
	char used;     /* adresa použitá v kódu nebo datech */
	char thread;   /* TLS */
	char export;
	int ret;       /* třída výsledku, -1 bez výsledku */
	int par;       /* první třída parametru v cpars */
	int npar;
};

enum {
	FBytes,
	FZero,
	FRef,
};

struct CField {
	int kind;
	uint64_t start; /* FBytes: posun v cbytes */
	uint64_t len;   /* FBytes a FZero: počet bajtů */
	uint32_t sym;   /* FRef */
	int64_t add;
	int size;
};

struct CObj {
	uint32_t id;
	int align;
	char export;
	char thread;
	char *sec;
	uint field;
	uint nfield;
};

static CSym *csyms;
static uint ncsyms;
static int *cpars;
static uint ncpars;
static uchar *cbytes;
static uint64_t nbytes, capbytes;
static CField *fields;
static uint nfields;
static CObj *objs;
static uint nobjs;
static CObj *cur;
static FILE *body;
static char bodypath[4096];

char *c_outpath;

static CSym *
csym(uint32_t id)
{
	uint n;

	for (n=0; n<ncsyms; n++)
		if (csyms[n].id == id)
			return &csyms[n];
	if (!csyms)
		csyms = vnew(0, sizeof csyms[0], PHeap);
	vgrow(&csyms, ++ncsyms);
	csyms[ncsyms-1].id = id;
	csyms[ncsyms-1].func = 0;
	csyms[ncsyms-1].data = 0;
	csyms[ncsyms-1].used = 0;
	csyms[ncsyms-1].thread = 0;
	csyms[ncsyms-1].export = 0;
	csyms[ncsyms-1].ret = -1;
	csyms[ncsyms-1].par = 0;
	csyms[ncsyms-1].npar = 0;
	return &csyms[ncsyms-1];
}

static void
bput(int c)
{
	if (nbytes == capbytes) {
		capbytes = capbytes ? 2 * capbytes : 4096;
		cbytes = realloc(cbytes, capbytes);
		if (!cbytes)
			die("out of memory");
	}
	cbytes[nbytes++] = c;
}

static CField *
field(int kind)
{
	CField *f;

	if (!fields)
		fields = vnew(0, sizeof fields[0], PHeap);
	vgrow(&fields, ++nfields);
	f = &fields[nfields-1];
	f->kind = kind;
	f->start = nbytes;
	f->len = 0;
	f->sym = 0;
	f->add = 0;
	f->size = 0;
	cur->nfield++;
	return f;
}

/* Bajt dat aktuálního objektu (sousední bajty v jednom poli). */
static void
dbyte(int c)
{
	CField *f;

	f = cur->nfield ? &fields[nfields-1] : 0;
	if (!f || f->kind != FBytes)
		f = field(FBytes);
	bput(c);
	f->len++;
}

static void
dnum(int64_t v, int size)
{
	int n;

	for (n=0; n<size; n++)
		dbyte((v >> (8 * n)) & 0xff);
}

/* Řetězec z lexeru: v uvozovkách, escape \n \r \t \" \\ a \NNN. */
static void
dstr(char *s)
{
	int c;

	for (s++; *s && *s != '"'; s++) {
		if (*s != '\\') {
			dbyte((uchar)*s);
			continue;
		}
		s++;
		switch (*s) {
		case 'n': dbyte('\n'); break;
		case 'r': dbyte('\r'); break;
		case 't': dbyte('\t'); break;
		case '"': dbyte('"'); break;
		case '\\': dbyte('\\'); break;
		default:
			c = (s[0] - '0') * 64 + (s[1] - '0') * 8 + (s[2] - '0');
			dbyte(c);
			s += 2;
			break;
		}
	}
}

void
c_emitdat(Dat *d)
{
	static int size[] = {[DB] = 1, [DH] = 2, [DW] = 4, [DL] = 8};
	CField *f;
	CSym *s;

	switch (d->type) {
	case DStart:
		if (!objs)
			objs = vnew(0, sizeof objs[0], PHeap);
		vgrow(&objs, ++nobjs);
		cur = &objs[nobjs-1];
		cur->id = intern(d->name);
		cur->align = d->lnk->align ? d->lnk->align : 8;
		cur->export = d->lnk->export;
		cur->thread = d->lnk->thread;
		cur->sec = d->lnk->sec;
		cur->field = nfields;
		cur->nfield = 0;
		s = csym(cur->id);
		s->data = 1;
		s->thread = cur->thread;
		s->export = cur->export;
		break;
	case DEnd:
		cur = 0;
		break;
	case DZ:
		if (d->u.num > 0) {
			f = field(FZero);
			f->len = d->u.num;
		}
		break;
	default:
		if (d->isstr) {
			if (d->type != DB)
				err("strings only supported for 'b' currently");
			dstr(d->u.str);
		} else if (d->isref) {
			if (size[d->type] != 4 && size[d->type] != 8)
				die("c32: data reference must be 4 or 8 bytes");
			f = field(FRef);
			f->sym = intern(d->u.ref.name);
			f->add = d->u.ref.off;
			f->size = size[d->type];
			csym(f->sym)->used = 1;
		} else
			dnum(d->u.num, size[d->type]);
		break;
	}
}

/* Stav emitace funkce. */
static Fn *fn;
static FILE *out;
static int *allocof;   /* posun oblasti alloc v rámci, nebo -1 */
static int framesize;

static char *ctype[] = {[Kw] = "uint32_t", [Kl] = "uint64_t", [Ks] = "float", [Kd] = "double"};

static int
tcls(int t)
{
	int k;

	k = fn->tmp[t].cls;
	return k < 0 ? Kl : k;
}

/* Výraz pro adresu symbolu s posunem (Kl). */
static void
symaddr(FILE *f, uint32_t id, int64_t add)
{
	csym(id)->used = 1;
	if (add)
		fprintf(f, "((uint64_t)(uintptr_t)&s%u + (uint64_t)%"PRId64")", id, add);
	else
		fprintf(f, "((uint64_t)(uintptr_t)&s%u)", id);
}

/* Hodnota ref ve třídě k. Kw z Kl je dolních 32 bitů, Kl z Kw rozšíření
 * nulami (jako zápis Kw do 64bitového registru). */
static void
ref(Ref r, int k)
{
	Con *c;
	int t;
	union { uint32_t w; float s; } fs;

	switch (rtype(r)) {
	case RTmp:
		t = tcls(r.val);
		if (t == k)
			fprintf(out, "t%d", r.val);
		else if (KBASE(t) == 0 && KBASE(k) == 0)
			fprintf(out, "((%s)t%d)", ctype[k], r.val);
		else
			die("c32: class mismatch for t%d", r.val);
		break;
	case RCon:
		c = &fn->con[r.val];
		if (c->type == CAddr) {
			if (k == Kw)
				fputs("((uint32_t)", out);
			symaddr(out, c->sym.id, c->bits.i);
			if (k == Kw)
				fputs(")", out);
			if (KBASE(k) != 0)
				die("c32: symbol address used as a float");
		} else if (k == Ks) {
			fs.w = (uint32_t)c->bits.i;
			if (c->flt == 1)
				fs.s = c->bits.s;
			fprintf(out, "lux_wtos(UINT32_C(%u))", fs.w);
		} else if (k == Kd)
			fprintf(out, "lux_ltod(UINT64_C(%"PRIu64"))", (uint64_t)c->bits.i);
		else if (k == Kw)
			fprintf(out, "UINT32_C(%u)", (uint32_t)c->bits.i);
		else
			fprintf(out, "UINT64_C(%"PRIu64")", (uint64_t)c->bits.i);
		break;
	default:
		die("c32: unexpected operand");
	}
}

static void
assign(Ins *i)
{
	fprintf(out, "\tt%d = ", i->to.val);
}

static void
binop(Ins *i, char *op)
{
	assign(i);
	fputs("(", out);
	ref(i->arg[0], i->cls);
	fprintf(out, " %s ", op);
	ref(i->arg[1], i->cls);
	fputs(");\n", out);
}

/* Pomocná funkce lux_<jméno><třída>(a, b). */
static void
helper2(Ins *i, char *name)
{
	assign(i);
	fprintf(out, "lux_%s%c(", name, "wlsd"[i->cls]);
	ref(i->arg[0], i->cls);
	fputs(", ", out);
	ref(i->arg[1], i->cls);
	fputs(");\n", out);
}

static void
shift(Ins *i)
{
	static char *signedtype[] = {[Kw] = "int32_t", [Kl] = "int64_t"};
	int mask;

	mask = i->cls == Kw ? 31 : 63;
	assign(i);
	if (i->op == Osar) {
		fprintf(out, "(%s)((%s)", ctype[i->cls], signedtype[i->cls]);
		ref(i->arg[0], i->cls);
		fputs(" >> (", out);
		ref(i->arg[1], Kw);
		fprintf(out, " & %d));\n", mask);
	} else {
		fputs("(", out);
		ref(i->arg[0], i->cls);
		fprintf(out, " %s (", i->op == Oshl ? "<<" : ">>");
		ref(i->arg[1], Kw);
		fprintf(out, " & %d));\n", mask);
	}
}

/* Porovnání: třída operandů z operace, výsledek 0 nebo 1 ve třídě cíle. */
static void
compare(Ins *i)
{
	static char *iop[] = {"==", "!=", ">=", ">", "<=", "<", ">=", ">", "<=", "<"};
	static char *fop[] = {"==", ">=", ">", "<=", "<", "!="};
	static char *stype[] = {[Kw] = "int32_t", [Kl] = "int64_t"};
	int k, c;

	if (INRANGE(i->op, Ocmpw, Ocmpw1)) {
		k = Kw;
		c = i->op - Ocmpw;
	} else if (INRANGE(i->op, Ocmpl, Ocmpl1)) {
		k = Kl;
		c = i->op - Ocmpl;
	} else if (INRANGE(i->op, Ocmps, Ocmps1)) {
		k = Ks;
		c = i->op - Ocmps;
	} else {
		k = Kd;
		c = i->op - Ocmpd;
	}
	assign(i);
	fprintf(out, "(%s)(", ctype[i->cls]);
	if (KBASE(k) == 0) {
		if (c >= 2 && c < 6) {
			fprintf(out, "(%s)", stype[k]);
			ref(i->arg[0], k);
			fprintf(out, " %s (%s)", iop[c], stype[k]);
			ref(i->arg[1], k);
		} else {
			ref(i->arg[0], k);
			fprintf(out, " %s ", iop[c]);
			ref(i->arg[1], k);
		}
	} else if (c < 6) {
		ref(i->arg[0], k);
		fprintf(out, " %s ", fop[c]);
		ref(i->arg[1], k);
	} else {
		/* co: oba operandy nejsou NaN; cuo: aspoň jeden je */
		fputs(c == 6 ? "!__builtin_isunordered(" : "__builtin_isunordered(", out);
		ref(i->arg[0], k);
		fputs(", ", out);
		ref(i->arg[1], k);
		fputs(")", out);
	}
	fputs(");\n", out);
}

static void
load(Ins *i)
{
	static char *name[] = {"sb", "ub", "sh", "uh", "sw", "uw"};

	assign(i);
	if (i->op == Oload)
		fprintf(out, "lux_ld%c(", "wlsd"[i->cls]);
	else
		fprintf(out, "(%s)lux_ld%s(", ctype[i->cls], name[i->op - Oloadsb]);
	ref(i->arg[0], Kl);
	fputs(");\n", out);
}

static void
store(Ins *i)
{
	static char *name[] = {"b", "h", "w", "l", "s", "d"};
	static int cls[] = {Kw, Kw, Kw, Kl, Ks, Kd};
	int n;

	n = i->op - Ostoreb;
	fprintf(out, "\tlux_st%s(", name[n]);
	ref(i->arg[1], Kl);
	fputs(", ", out);
	ref(i->arg[0], cls[n]);
	fputs(");\n", out);
}

static void
extend(Ins *i)
{
	static char *from[] = {"int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t"};

	assign(i);
	fprintf(out, "(%s)(%s)", ctype[i->cls], from[i->op - Oextsb]);
	ref(i->arg[0], Kw);
	fputs(";\n", out);
}

/* Typ parametru/argumentu v prototypu: dílčí slova jako int32_t. */
static char *
ptype(int k)
{
	return ctype[k];
}

static void
call(Ins *i, Ins *args, int nargs)
{
	Con *c;
	int n, first;

	if (!req(i->to, R)) {
		assign(i);
	} else
		fputs("\t", out);
	fputs("((", out);
	fputs(req(i->to, R) ? "void" : ctype[i->cls], out);
	fputs(" (*)(", out);
	first = 1;
	for (n=0; n<nargs; n++) {
		if (args[n].op == Oargv) {
			/* typy variadických argumentů prototyp neuvádí */
			fputs(first ? "..." : ", ...", out);
			first = 0;
			break;
		}
		if (!INRANGE(args[n].op, Oarg, Oarguh))
			die("c32: aggregate and environment arguments are not supported");
		fprintf(out, "%s%s", first ? "" : ", ", ptype(isargbh(args[n].op) ? Kw : args[n].cls));
		first = 0;
	}
	if (first)
		fputs("void", out);
	fputs("))", out);
	if (rtype(i->arg[0]) == RCon && fn->con[i->arg[0].val].type == CAddr && !fn->con[i->arg[0].val].bits.i) {
		c = &fn->con[i->arg[0].val];
		csym(c->sym.id)->used = 1;
		fprintf(out, "(uintptr_t)&s%u)(", c->sym.id);
	} else {
		fputs("(uintptr_t)", out);
		ref(i->arg[0], Kl);
		fputs(")(", out);
	}
	first = 1;
	for (n=0; n<nargs; n++) {
		if (args[n].op == Oargv)
			continue;
		if (!first)
			fputs(", ", out);
		first = 0;
		if (isargbh(args[n].op)) {
			static char *sub[] = {"int8_t", "uint8_t", "int16_t", "uint16_t"};
			fprintf(out, "(uint32_t)(%s)", sub[args[n].op - Oargsb]);
			ref(args[n].arg[0], Kw);
		} else
			ref(args[n].arg[0], args[n].cls);
	}
	fputs(");\n", out);
}

static void
emitins(Ins *i)
{
	switch (i->op) {
	case Onop:
	case Odbgloc:
		break;
	case Ocopy:
		assign(i);
		ref(i->arg[0], i->cls);
		fputs(";\n", out);
		break;
	case Oadd: binop(i, "+"); break;
	case Osub: binop(i, "-"); break;
	case Omul: binop(i, "*"); break;
	case Oand: binop(i, "&"); break;
	case Oor: binop(i, "|"); break;
	case Oxor: binop(i, "^"); break;
	case Odiv:
		if (KBASE(i->cls))
			binop(i, "/");
		else
			helper2(i, "div");
		break;
	case Orem: helper2(i, "rem"); break;
	case Oudiv: helper2(i, "udiv"); break;
	case Ourem: helper2(i, "urem"); break;
	case Osar:
	case Oshr:
	case Oshl:
		shift(i);
		break;
	case Oneg:
		assign(i);
		fputs("-", out);
		ref(i->arg[0], i->cls);
		fputs(";\n", out);
		break;
	case Oexts:
		assign(i);
		fputs("(double)", out);
		ref(i->arg[0], Ks);
		fputs(";\n", out);
		break;
	case Otruncd:
		assign(i);
		fputs("(float)", out);
		ref(i->arg[0], Kd);
		fputs(";\n", out);
		break;
	case Ostosi:
	case Ostoui:
	case Odtosi:
	case Odtoui:
		/* nasycený převod: v C je převod mimo rozsah nedefinovaný */
		assign(i);
		fprintf(out, "lux_%cto%c%c(", (i->op == Ostosi || i->op == Ostoui) ? 's' : 'd',
			(i->op == Ostosi || i->op == Odtosi) ? 's' : 'u', "wl"[i->cls]);
		ref(i->arg[0], (i->op == Ostosi || i->op == Ostoui) ? Ks : Kd);
		fputs(");\n", out);
		break;
	case Oswtof:
	case Ouwtof:
		assign(i);
		fprintf(out, "(%s)(%s)", ctype[i->cls], i->op == Oswtof ? "int32_t" : "uint32_t");
		ref(i->arg[0], Kw);
		fputs(";\n", out);
		break;
	case Osltof:
	case Oultof:
		assign(i);
		fprintf(out, "(%s)(%s)", ctype[i->cls], i->op == Osltof ? "int64_t" : "uint64_t");
		ref(i->arg[0], Kl);
		fputs(";\n", out);
		break;
	case Ocast:
		assign(i);
		switch (i->cls) {
		case Kw: fputs("lux_stow(", out); ref(i->arg[0], Ks); break;
		case Kl: fputs("lux_dtol(", out); ref(i->arg[0], Kd); break;
		case Ks: fputs("lux_wtos(", out); ref(i->arg[0], Kw); break;
		case Kd: fputs("lux_ltod(", out); ref(i->arg[0], Kl); break;
		}
		fputs(");\n", out);
		break;
	case Oalloc4:
	case Oalloc8:
	case Oalloc16:
		assign(i);
		if (allocof[i->to.val] >= 0)
			fprintf(out, "(uint64_t)(uintptr_t)(lux_frame + %d);\n", allocof[i->to.val]);
		else {
			fputs("(uint64_t)(uintptr_t)__builtin_alloca_with_align(", out);
			ref(i->arg[0], Kl);
			fputs(", 128);\n", out);
		}
		break;
	case Oblit1:
		break;
	case Oblit0:
		/* blit0 (zdroj, cíl) a za ním blit1 s velikostí */
		fputs("\t__builtin_memmove((void *)(uintptr_t)", out);
		ref(i->arg[1], Kl);
		fputs(", (void *)(uintptr_t)", out);
		ref(i->arg[0], Kl);
		fprintf(out, ", %d);\n", abs(rsval((i+1)->arg[0])));
		break;
	default:
		if (isext(i->op))
			extend(i);
		else if (isload(i->op))
			load(i);
		else if (isstore(i->op))
			store(i);
		else if (INRANGE(i->op, Ocmpw, Ocmpd1))
			compare(i);
		else
			die("c32: unsupported operation %s", optab[i->op].name);
		break;
	}
}

/* Kopie phi pro hranu b -> s: nejdřív všechny hodnoty do pomocných
 * proměnných, pak přiřazení (paralelní kopie). */
static void
phicopies(Blk *b, Blk *s)
{
	Phi *p;
	uint n;
	int cnt;

	cnt = 0;
	for (p=s->phi; p; p=p->link) {
		for (n=0; n<p->narg; n++)
			if (p->blk[n] == b)
				break;
		if (n == p->narg)
			die("c32: missing phi argument");
		if (!cnt)
			fputs("\t{\n", out);
		fprintf(out, "\t%s p%d = ", ctype[p->cls], cnt);
		ref(p->arg[n], p->cls);
		fputs(";\n", out);
		cnt++;
	}
	if (!cnt)
		return;
	cnt = 0;
	for (p=s->phi; p; p=p->link)
		fprintf(out, "\tt%d = p%d;\n", p->to.val, cnt++);
	fputs("\t}\n", out);
}

static void
jump(Blk *b, Blk *s)
{
	phicopies(b, s);
	fprintf(out, "\tgoto L%u;\n", s->id);
}

static int
retclass(int j)
{
	switch (j) {
	case Jretl: return Kl;
	case Jrets: return Ks;
	case Jretd: return Kd;
	case Jret0: return -1;
	default: return Kw;
	}
}

static void
prototype(FILE *f, CSym *s, int names)
{
	int n;

	fprintf(f, "%s%s s%u(", s->export ? "" : "static ", s->ret < 0 ? "void" : ctype[s->ret], s->id);
	for (n=0; n<s->npar; n++)
		if (names)
			fprintf(f, "%s%s a%d", n ? ", " : "", ptype(cpars[s->par + n]), n);
		else
			fprintf(f, "%s%s", n ? ", " : "", ptype(cpars[s->par + n]));
	if (!s->npar)
		fputs("void", f);
	fputs(")", f);
}

void
c_emitfn(Fn *f, FILE *o)
{
	Blk *b;
	Ins *i, *args;
	CSym *s;
	uint n;
	int t, npar, off, sz, ret, k;

	(void)o;
	fn = f;
	if (!body) {
		if (!c_outpath)
			die("c32: output file required (-o)");
		if (strlen(c_outpath) + 6 >= sizeof bodypath)
			die("c32: output path too long");
		strcpy(bodypath, c_outpath);
		strcpy(bodypath + strlen(bodypath), ".body");
		body = fopen(bodypath, "w");
		if (!body)
			die("c32: cannot open %s", bodypath);
	}
	out = body;
	if (fn->vararg)
		die("c32: variadic functions are not supported");
	for (n=0; n<fn->nblk; n++)
		fn->rpo[n]->id = n;
	allocof = alloc(fn->ntmp * sizeof allocof[0]);
	for (t=0; t<fn->ntmp; t++)
		allocof[t] = -1;
	off = 0;
	ret = -1;
	s = csym(intern(fn->name));
	s->func = 1;
	s->export = fn->lnk.export;
	s->par = ncpars;
	s->npar = 0;
	if (!cpars)
		cpars = vnew(0, sizeof cpars[0], PHeap);
	for (n=0; n<fn->nblk; n++) {
		b = fn->rpo[n];
		if (isret(b->jmp.type) && b->jmp.type != Jret0)
			ret = retclass(b->jmp.type);
		for (i=b->ins; i<&b->ins[b->nins]; i++) {
			if (ispar(i->op)) {
				if (!INRANGE(i->op, Opar, Oparuh))
					die("c32: aggregate parameters are not supported");
				vgrow(&cpars, ++ncpars);
				cpars[ncpars-1] = isparbh(i->op) ? Kw : i->cls;
				s->npar++;
			}
			if (isalloc(i->op) && rtype(i->arg[0]) == RCon) {
				off = (off + 15) & ~15;
				allocof[i->to.val] = off;
				sz = fn->con[i->arg[0].val].bits.i;
				off += (sz + 15) & ~15;
			}
		}
	}
	s->ret = ret;
	framesize = off;
	fputs("\n", out);
	prototype(out, s, 1);
	fputs("\n{\n", out);
	for (t=Tmp0; t<fn->ntmp; t++)
		fprintf(out, "\t%s t%d = 0;\n", ctype[tcls(t)], t);
	if (framesize)
		fprintf(out, "\t_Alignas(16) unsigned char lux_frame[%d];\n", framesize);
	npar = 0;
	for (i=fn->start->ins; i<&fn->start->ins[fn->start->nins]; i++)
		if (ispar(i->op)) {
			k = isparbh(i->op) ? Kw : i->cls;
			fprintf(out, "\tt%d = ", i->to.val);
			if (isparbh(i->op)) {
				static char *sub[] = {"int8_t", "uint8_t", "int16_t", "uint16_t"};
				fprintf(out, "(uint32_t)(%s)a%d;\n", sub[i->op - Oparsb], npar);
			} else
				fprintf(out, "a%d;\n", npar);
			(void)k;
			npar++;
		}
	for (n=0; n<fn->nblk; n++) {
		b = fn->rpo[n];
		fprintf(out, "L%u:;\n", n);
		args = 0;
		for (i=b->ins; i<&b->ins[b->nins]; i++) {
			if (ispar(i->op))
				continue;
			if (isarg(i->op)) {
				if (!args)
					args = i;
				continue;
			}
			if (i->op == Ocall) {
				call(i, args ? args : i, args ? (int)(i - args) : 0);
				args = 0;
				continue;
			}
			emitins(i);
		}
		switch (b->jmp.type) {
		case Jhlt:
			fputs("\t__builtin_trap();\n", out);
			break;
		case Jret0:
			if (ret < 0)
				fputs("\treturn;\n", out);
			else
				fprintf(out, "\treturn (%s)0;\n", ctype[ret]);
			break;
		case Jretw:
		case Jretl:
		case Jrets:
		case Jretd:
		case Jretsb:
		case Jretub:
		case Jretsh:
		case Jretuh:
			fputs("\treturn ", out);
			ref(b->jmp.arg, retclass(b->jmp.type));
			fputs(";\n", out);
			break;
		case Jjmp:
			jump(b, b->s1);
			break;
		case Jjnz:
			fputs("\tif (", out);
			if (rtype(b->jmp.arg) == RTmp)
				ref(b->jmp.arg, tcls(b->jmp.arg.val));
			else
				ref(b->jmp.arg, Kl);
			fputs(" != 0) {\n", out);
			jump(b, b->s1);
			fputs("\t} else {\n", out);
			jump(b, b->s2);
			fputs("\t}\n", out);
			break;
		default:
			die("c32: unsupported jump %d", b->jmp.type);
		}
	}
	fputs("}\n", out);
}

static char *prelude =
	"/* LuxIR c32 */\n"
	"#include <stdint.h>\n"
	"_Static_assert(sizeof(void *) == 4, \"LuxIR c32 output needs a 32-bit target\");\n"
	"#define LUX_P(a) ((void *)(uintptr_t)(a))\n"
	"static inline uint32_t lux_ldw(uint64_t a) { uint32_t v; __builtin_memcpy(&v, LUX_P(a), 4); return v; }\n"
	"static inline uint64_t lux_ldl(uint64_t a) { uint64_t v; __builtin_memcpy(&v, LUX_P(a), 8); return v; }\n"
	"static inline float lux_lds(uint64_t a) { float v; __builtin_memcpy(&v, LUX_P(a), 4); return v; }\n"
	"static inline double lux_ldd(uint64_t a) { double v; __builtin_memcpy(&v, LUX_P(a), 8); return v; }\n"
	"static inline int8_t lux_ldsb(uint64_t a) { int8_t v; __builtin_memcpy(&v, LUX_P(a), 1); return v; }\n"
	"static inline uint8_t lux_ldub(uint64_t a) { uint8_t v; __builtin_memcpy(&v, LUX_P(a), 1); return v; }\n"
	"static inline int16_t lux_ldsh(uint64_t a) { int16_t v; __builtin_memcpy(&v, LUX_P(a), 2); return v; }\n"
	"static inline uint16_t lux_lduh(uint64_t a) { uint16_t v; __builtin_memcpy(&v, LUX_P(a), 2); return v; }\n"
	"static inline int32_t lux_ldsw(uint64_t a) { int32_t v; __builtin_memcpy(&v, LUX_P(a), 4); return v; }\n"
	"static inline uint32_t lux_lduw(uint64_t a) { uint32_t v; __builtin_memcpy(&v, LUX_P(a), 4); return v; }\n"
	"static inline void lux_stb(uint64_t a, uint32_t v) { uint8_t x = v; __builtin_memcpy(LUX_P(a), &x, 1); }\n"
	"static inline void lux_sth(uint64_t a, uint32_t v) { uint16_t x = v; __builtin_memcpy(LUX_P(a), &x, 2); }\n"
	"static inline void lux_stw(uint64_t a, uint32_t v) { __builtin_memcpy(LUX_P(a), &v, 4); }\n"
	"static inline void lux_stl(uint64_t a, uint64_t v) { __builtin_memcpy(LUX_P(a), &v, 8); }\n"
	"static inline void lux_sts(uint64_t a, float v) { __builtin_memcpy(LUX_P(a), &v, 4); }\n"
	"static inline void lux_std(uint64_t a, double v) { __builtin_memcpy(LUX_P(a), &v, 8); }\n"
	"static inline uint32_t lux_stow(float v) { uint32_t x; __builtin_memcpy(&x, &v, 4); return x; }\n"
	"static inline float lux_wtos(uint32_t v) { float x; __builtin_memcpy(&x, &v, 4); return x; }\n"
	"static inline uint64_t lux_dtol(double v) { uint64_t x; __builtin_memcpy(&x, &v, 8); return x; }\n"
	"static inline double lux_ltod(uint64_t v) { double x; __builtin_memcpy(&x, &v, 8); return x; }\n"
	/* dělení s chováním RISC-V: nulou -> všechny jedničky / dělenec,
	 * INT_MIN / -1 -> INT_MIN / 0 */
	"static inline uint32_t lux_divw(uint32_t a, uint32_t b) { int32_t x = a, y = b; if (!y) return UINT32_MAX; if (x == INT32_MIN && y == -1) return a; return (uint32_t)(x / y); }\n"
	"static inline uint64_t lux_divl(uint64_t a, uint64_t b) { int64_t x = a, y = b; if (!y) return UINT64_MAX; if (x == INT64_MIN && y == -1) return a; return (uint64_t)(x / y); }\n"
	"static inline uint32_t lux_remw(uint32_t a, uint32_t b) { int32_t x = a, y = b; if (!y) return a; if (y == -1) return 0; return (uint32_t)(x % y); }\n"
	"static inline uint64_t lux_reml(uint64_t a, uint64_t b) { int64_t x = a, y = b; if (!y) return a; if (y == -1) return 0; return (uint64_t)(x % y); }\n"
	"static inline uint32_t lux_udivw(uint32_t a, uint32_t b) { return b ? a / b : UINT32_MAX; }\n"
	"static inline uint64_t lux_udivl(uint64_t a, uint64_t b) { return b ? a / b : UINT64_MAX; }\n"
	"static inline uint32_t lux_uremw(uint32_t a, uint32_t b) { return b ? a % b : a; }\n"
	"static inline uint64_t lux_ureml(uint64_t a, uint64_t b) { return b ? a % b : a; }\n"
	/* nasycené převody (NaN -> 0) */
	"static inline uint32_t lux_dtosw(double v) { if (v != v) return 0; if (v <= -2147483648.0) return (uint32_t)INT32_MIN; if (v >= 2147483647.0) return INT32_MAX; return (uint32_t)(int32_t)v; }\n"
	"static inline uint64_t lux_dtosl(double v) { if (v != v) return 0; if (v <= -9223372036854775808.0) return (uint64_t)INT64_MIN; if (v >= 9223372036854775808.0) return INT64_MAX; return (uint64_t)(int64_t)v; }\n"
	"static inline uint32_t lux_dtouw(double v) { if (!(v > 0)) return 0; if (v >= 4294967295.0) return UINT32_MAX; return (uint32_t)v; }\n"
	"static inline uint64_t lux_dtoul(double v) { if (!(v > 0)) return 0; if (v >= 18446744073709551616.0) return UINT64_MAX; return (uint64_t)v; }\n"
	"static inline uint32_t lux_stosw(float v) { return lux_dtosw(v); }\n"
	"static inline uint64_t lux_stosl(float v) { return lux_dtosl(v); }\n"
	"static inline uint32_t lux_stouw(float v) { return lux_dtouw(v); }\n"
	"static inline uint64_t lux_stoul(float v) { return lux_dtoul(v); }\n";

static void
quotename(FILE *f, uint32_t id)
{
	char *s;

	fputc('"', f);
	for (s=str(id); *s; s++)
		if (*s == '"' || *s == '\\')
			fprintf(f, "\\%c", *s);
		else
			fputc(*s, f);
	fputc('"', f);
}

static void
datadecl(FILE *f, CObj *o, int def)
{
	fprintf(f, "%s%sstruct d%u s%u", o->export ? (def ? "" : "extern ") : "static ",
		o->thread ? "_Thread_local " : "", o->id, o->id);
}

void
c_emitfin(FILE *f)
{
	uint n, m;
	CSym *s;
	CObj *o;
	CField *d;
	int c;
	char buf[4096];
	size_t got;

	fputs(prelude, f);
	/* funkce definované v modulu a cizí symboly */
	for (n=0; n<ncsyms; n++) {
		s = &csyms[n];
		if (s->func) {
			prototype(f, s, 0);
			fputs(" __asm__(", f);
			quotename(f, s->id);
			fputs(");\n", f);
		} else if (!s->data && s->used) {
			fprintf(f, "extern %schar s%u[] __asm__(", s->thread ? "_Thread_local " : "", s->id);
			quotename(f, s->id);
			fputs(");\n", f);
		}
	}
	/* typy a deklarace dat */
	for (n=0; n<nobjs; n++) {
		o = &objs[n];
		fprintf(f, "struct __attribute__((packed, aligned(%d))) d%u {", o->align, o->id);
		if (!o->nfield)
			fputs(" uint8_t empty[1];", f);
		for (m=0; m<o->nfield; m++) {
			d = &fields[o->field + m];
			if (d->kind != FRef)
				fprintf(f, " uint8_t f%u[%"PRIu64"];", m, d->len);
			else if (d->size == 8)
				fprintf(f, " uintptr_t f%u; uint32_t h%u;", m, m);
			else
				fprintf(f, " uintptr_t f%u;", m);
		}
		fputs(" };\n", f);
		datadecl(f, o, 0);
		fputs(" __asm__(", f);
		quotename(f, o->id);
		fputs(")", f);
		if (o->sec)
			fprintf(f, " __attribute__((section(%s)))", o->sec);
		fputs(";\n", f);
	}
	/* data */
	for (n=0; n<nobjs; n++) {
		o = &objs[n];
		datadecl(f, o, 1);
		fputs(" = {", f);
		if (!o->nfield)
			fputs(" {0}", f);
		for (m=0; m<o->nfield; m++) {
			d = &fields[o->field + m];
			fputs(m ? ", " : " ", f);
			if (d->kind == FZero)
				fputs("{0}", f);
			else if (d->kind == FBytes) {
				fputs("{", f);
				for (c=0; c<(int)d->len; c++)
					fprintf(f, "%s%u", c ? "," : "", cbytes[d->start + c]);
				fputs("}", f);
			} else {
				fprintf(f, "(uintptr_t)&s%u", d->sym);
				if (d->add)
					fprintf(f, " + (uintptr_t)%"PRId64, d->add);
				if (d->size == 8)
					fputs(", 0", f);
			}
		}
		fputs(" };\n", f);
	}
	/* těla funkcí */
	if (body) {
		fclose(body);
		body = fopen(bodypath, "r");
		if (!body)
			die("c32: cannot reopen %s", bodypath);
		while ((got = fread(buf, 1, sizeof buf - 1, body)) > 0) {
			buf[got] = 0;
			fputs(buf, f);
		}
		fclose(body);
		luxir_unlink(bodypath);
		body = 0;
	}
}

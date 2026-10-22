#include "as.h"

/* Jádro vestavěného assembleru: čtení zdroje, direktivy, symboly, sekce,
 * výrazy, rozvržení s relaxací skoků a zápis objektu ELF (viz as.h). */

AArch *A;
ASec *asecs;
uint nasecs;
ASymbol *asyms;
uint nasyms;
int acursec;
int aline;

static char *apath;
static char *src;

/* .macro jméno [parametry] … .endm: tělo se při volání rozvine
 * s dosazením \parametr (GNU as) */
typedef struct AMacro AMacro;
struct AMacro {
	char *name;
	char *body;      /* řádky těla oddělené \n */
	char *param[8];
	int nparam;
};
static AMacro *macros;
static uint nmacros;
static int macrodepth;
static AItem *cur;      /* otevřená datová položka aktuální sekce */
static int *locnum;     /* číselná návěští: počet definic každého čísla */
static int nlocnum;
static char *filename;  /* .file */
int acoff;              /* objekt COFF (Windows) místo ELF */
static int *elfsym;     /* index symbolu v tabulce ELF */

void
aerr(char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s:%d: ", apath, aline);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

/* Varování jako GNU as: vypíše se, překlad pokračuje. */
void
awarn(char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s:%d: Warning: ", apath, aline);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* Pole s kapacitou v mocninách dvou: zvětší na n prvků. */
static void *
grow(void *p, uint n, size_t esz)
{
	size_t cap, m;

	/* kapacita 8, pak mocniny dvou: realokace při prvním prvku a vždy,
	 * když n-1 je mocnina dvou >= 8 */
	m = n - 1;
	if (n != 1 && (m < 8 || (m & (m - 1)) != 0))
		return p;
	cap = n == 1 ? 8 : 2 * m;
	p = realloc(p, cap * esz);
	if (!p)
		die("out of memory");
	return p;
}

#define GROW(v, n) ((v) = grow((v), (n), sizeof (v)[0]))

static char *
dupstr(char *s)
{
	char *d;

	d = emalloc(strlen(s) + 1);
	strcpy(d, s);
	return d;
}

/* pomůcky místo isxdigit/strstr (nejsou v knihovně generované LSL verze) */
static int
hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static char *
findstr(char *s, char *needle)
{
	size_t n;

	n = strlen(needle);
	for (; *s; s++)
		if (strncmp(s, needle, n) == 0)
			return s;
	return 0;
}

/* ---- sekce a symboly ---- */

static int
findsec(char *name)
{
	uint n;

	for (n=0; n<nasecs; n++)
		if (strcmp(asecs[n].name, name) == 0)
			return n;
	return -1;
}

static int
newsec(char *name, int flags, int type)
{
	ASec *s;

	GROW(asecs, nasecs + 1);
	s = &asecs[nasecs];
	memset(s, 0, sizeof *s);
	s->name = dupstr(name);
	s->flags = flags;
	s->type = type;
	s->align = 1;
	s->symidx = -1;
	return nasecs++;
}

static void
usesec(int idx)
{
	acursec = idx;
	cur = 0;
}

static int afile;  /* pořadí vstupního souboru (místní symboly zvlášť) */

/* místní jméno (nejde do tabulky symbolů): .L…, v COFF pro x86-64 každé
 * jméno začínající L (GNU as pro pe-x86-64) */
static int
localname(char *name)
{
	if (name[0] == '.' && name[1] == 'L')
		return 1;
	return acoff && A->machine == 62 && name[0] == 'L';
}

int
asymidx(char *name)
{
	uint n;
	ASymbol *s;
	static char *buf;
	static size_t cap;
	size_t need;

	/* místní symboly .L… platí v jednom souboru jako v GNU as */
	if (afile > 0 && localname(name) && name[0] == '.') {
		need = strlen(name) + 16;
		if (need > cap) {
			cap = 2 * need;
			buf = realloc(buf, cap);
			if (!buf)
				die("out of memory");
		}
		sprintf(buf, ".L#f%d#%s", afile, name + 2);
		name = buf;
	}
	for (n=0; n<nasyms; n++)
		if (strcmp(asyms[n].name, name) == 0)
			return n;
	GROW(asyms, nasyms + 1);
	s = &asyms[nasyms];
	memset(s, 0, sizeof *s);
	s->name = dupstr(name);
	s->sec = -1;
	s->sizesub = -1;
	s->local = localname(name);
	return nasyms++;
}

static AItem *
newitem(int kind)
{
	ASec *s;
	AItem *i;

	if (acursec < 0)
		aerr("no section");
	s = &asecs[acursec];
	GROW(s->item, s->nitem + 1);
	i = &s->item[s->nitem++];
	memset(i, 0, sizeof *i);
	i->kind = kind;
	i->line = aline;
	cur = kind == IData ? i : 0;
	return i;
}

static AItem *
dataitem(void)
{
	if (!cur)
		newitem(IData);
	return cur;
}

uchar *
aemit(uint n)
{
	AItem *i;

	i = dataitem();
	if (asecs[acursec].flags & SNobits) {
		i->size += n;
		return 0;
	}
	while (i->size + n > i->cap) {
		i->cap = i->cap ? 2 * i->cap : 32;
		i->data = realloc(i->data, i->cap);
		if (!i->data)
			die("out of memory");
	}
	memset(i->data + i->size, 0, n);
	i->size += n;
	return i->data + i->size - n;
}

void
afixenc(uint off, int size, int kind, int64_t pcoff, int enc, AExpr *e)
{
	AItem *i;
	AFix *f;

	i = dataitem();
	GROW(i->fix, i->nfix + 1);
	f = &i->fix[i->nfix++];
	f->off = off;
	f->size = size;
	f->kind = kind;
	f->pcoff = pcoff;
	f->enc = enc;
	f->relsym = -1;
	f->e = *e;
}

void
afix(uint off, int size, int kind, int64_t pcoff, AExpr *e)
{
	afixenc(off, size, kind, pcoff, 0, e);
}

/* Oprava, jejíž relokace míří na jiný symbol s addendem 0 (RISC-V
 * %pcrel_lo na návěští u auipc); vyřešení používá e jako obvykle. */
void
afixrel(uint off, int size, int kind, int64_t pcoff, int enc, AExpr *e, int relsym)
{
	AItem *i;

	afixenc(off, size, kind, pcoff, enc, e);
	i = dataitem();
	i->fix[i->nfix - 1].relsym = relsym;
}

void
ajump(int jop, AExpr *e)
{
	AItem *i;

	if (e->sym < 0)
		aerr("jump to a constant");
	i = newitem(IJump);
	i->jop = jop;
	i->target = *e;
}

/* Symbol na aktuální pozici (začátek další položky). */
static int
herelabel(char *name)
{
	ASymbol *s;
	int idx;

	if (acursec < 0)
		aerr("label outside a section");
	idx = asymidx(name);
	s = &asyms[idx];
	if (s->defined)
		aerr("symbol %s is already defined", name);
	cur = 0;
	s->defined = 1;
	s->sec = acursec;
	s->item = asecs[acursec].nitem;
	return s - asyms;
}

/* Anonymní místní návěští na aktuální pozici, v ELF pojmenované „.L0 “
 * jako v GNU as pro RISC-V (symbol relokace %pcrel_lo). */
int
apcrellabel(void)
{
	static int npc;
	char name[32];

	sprintf(name, ".L#pc%d", npc++);
	return herelabel(name);
}

static int
dot(void)
{
	char name[64];
	int idx;

	sprintf(name, ".L#dot%d.%u", acursec, asecs[acursec].nitem);
	idx = asymidx(name);
	if (asyms[idx].defined)
		return idx;
	return herelabel(name);
}

/* ---- lexikální pomůcky ---- */

void
aspace(char **ps)
{
	char *p;

	p = *ps;
	while (*p == ' ' || *p == '\t')
		p++;
	*ps = p;
}

int
identchar(int c)
{
	return isalnum(c) || c == '_' || c == '.' || c == '$' || c == '@';
}

/* Slovo (identifikátor nebo řetězec v uvozovkách bez nich) ve statickém
 * bufferu. */
char *
aword(char **ps)
{
	static char buf[4096];
	char *p;
	uint n;

	aspace(ps);
	p = *ps;
	n = 0;
	if (*p == '"') {
		p++;
		while (*p && *p != '"') {
			if (*p == '\\' && p[1])
				p++;
			if (n + 1 < sizeof buf)
				buf[n++] = *p;
			p++;
		}
		if (*p == '"')
			p++;
	} else
		while (identchar((uchar)*p)) {
			if (n + 1 < sizeof buf)
				buf[n++] = *p;
			p++;
		}
	buf[n] = 0;
	*ps = p;
	return buf;
}

int
anumber(char **ps, int64_t *v)
{
	char *p, *end;
	uint64_t u;
	int neg;

	p = *ps;
	neg = 0;
	if (*p == '-') {
		neg = 1;
		p++;
	}
	if (!isdigit((uchar)*p))
		return 0;
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		u = strtoull(p + 2, &end, 16);
	else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B'))
		u = strtoull(p + 2, &end, 2);
	else if (p[0] == '0' && isdigit((uchar)p[1]))
		u = strtoull(p + 1, &end, 8);
	else
		u = strtoull(p, &end, 10);
	*v = neg ? -(int64_t)u : (int64_t)u;
	*ps = end;
	return 1;
}

/* Číselné návěští: jméno instance čísla num; forward = další definice. */
static char *
numlabel(int num, int forward)
{
	static char buf[64];
	int inst;

	inst = num < nlocnum ? locnum[num] : 0;
	if (forward)
		inst++;
	sprintf(buf, ".L#%d#%d", num, inst);
	return buf;
}

static int exprlevel(char **ps, AExpr *e, int level);

/* Člen výrazu: číslo, (výraz), symbol (přípona @plt, @gotpcrel … se
 * zahodí; GOT se tu nepoužívá), číselné návěští 1f/1b, '.', unární - ~ +.
 * Symbol smí být ve výrazu nejvýše jeden a jen s kladným znaménkem. */
static int
exprprim(char **ps, AExpr *e)
{
	char *p, *q, *w, *at, op;
	int64_t v;

	p = *ps;
	aspace(&p);
	e->sym = -1;
	e->add = 0;
	if (*p == '-' || *p == '~' || *p == '+') {
		op = *p++;
		if (!exprprim(&p, e))
			return 0;
		if (op != '+') {
			if (e->sym >= 0)
				aerr("invalid expression");
			e->add = op == '-' ? -e->add : ~e->add;
		}
		*ps = p;
		return 1;
	}
	if (*p == '(') {
		p++;
		if (!exprlevel(&p, e, 0))
			aerr("expression expected");
		aspace(&p);
		if (*p != ')')
			aerr("')' expected");
		p++;
	} else if (isdigit((uchar)*p)) {
		q = p;
		anumber(&q, &v);
		if ((*q == 'f' || *q == 'b') && !identchar((uchar)q[1])) {
			e->sym = asymidx(numlabel((int)v, *q == 'f'));
			q++;
		} else
			e->add = v;
		p = q;
	} else if (*p == '.' && !identchar((uchar)p[1])) {
		e->sym = dot();
		p++;
	} else if (identchar((uchar)*p) || *p == '"') {
		w = aword(&p);
		at = strchr(w, '@');
		if (at)
			*at = 0;
		e->sym = asymidx(w);
		if (asyms[e->sym].isabs) {
			/* konstanta z .equ */
			e->add = asyms[e->sym].absval;
			e->sym = -1;
		}
	} else
		return 0;
	*ps = p;
	return 1;
}

/* .equ/.set jméno, konstantní výraz */
static void
dequ(char *p)
{
	char *w;
	AExpr e;
	ASymbol *y;
	int idx;

	aspace(&p);
	w = dupstr(aword(&p));   /* aword má statický buffer, výraz ho přepíše */
	aspace(&p);
	if (*p != ',')
		aerr("',' expected");
	p++;
	if (!aexpr(&p, &e))
		aerr("expression expected");
	if (e.sym >= 0)
		aerr("only constant equates are supported");
	idx = asymidx(w);   /* dřív než ukazatel: asymidx může realokovat */
	y = &asyms[idx];
	if (y->defined)
		aerr("symbol %s is already defined", w);
	free(w);
	y->isabs = 1;
	y->absval = e.add;
}

/* Binární operátory s prioritou GNU as: 2 (* / % << >>), 1 (| & ^),
 * 0 (+ -); symbol smí vystupovat jen u + a -. */
static int
exprlevel(char **ps, AExpr *e, int level)
{
	char *p, op;
	AExpr r;
	int shift, lv;

	if (level > 2)
		return exprprim(ps, e);
	if (!exprlevel(ps, e, level + 1))
		return 0;
	p = *ps;
	for (;;) {
		aspace(&p);
		op = *p;
		shift = (op == '<' && p[1] == '<') || (op == '>' && p[1] == '>');
		if (op == '+' || op == '-')
			lv = 0;
		else if (op == '|' || op == '&' || op == '^')
			lv = 1;
		else if (op == '*' || op == '/' || op == '%' || shift)
			lv = 2;
		else
			break;
		if (lv != level)
			break;
		p += shift ? 2 : 1;
		if (!exprlevel(&p, &r, level + 1))
			aerr("expression expected");
		if (op == '+') {
			if (e->sym >= 0 && r.sym >= 0)
				aerr("invalid expression");
			if (r.sym >= 0)
				e->sym = r.sym;
			e->add += r.add;
		} else if (op == '-') {
			if (r.sym >= 0)
				aerr("invalid expression");
			e->add -= r.add;
		} else {
			if (e->sym >= 0 || r.sym >= 0)
				aerr("invalid expression");
			switch (op) {
			case '|': e->add |= r.add; break;
			case '&': e->add &= r.add; break;
			case '^': e->add ^= r.add; break;
			case '*': e->add *= r.add; break;
			case '/':
				if (r.add == 0)
					aerr("division by zero");
				e->add /= r.add;
				break;
			case '%':
				if (r.add == 0)
					aerr("division by zero");
				e->add %= r.add;
				break;
			case '<': e->add = (int64_t)((uint64_t)e->add << (r.add & 63)); break;
			case '>': e->add = (int64_t)((uint64_t)e->add >> (r.add & 63)); break;
			}
		}
		*ps = p;
	}
	return 1;
}

/* Výraz: sym + konstanta; vrací 0, nezačíná-li tu žádný výraz. */
int
aexpr(char **ps, AExpr *e)
{
	e->sym = -1;
	e->add = 0;
	return exprlevel(ps, e, 0);
}

/* ---- direktivy ---- */

static void
emitnum(int64_t v, int size)
{
	uchar *b;
	int n;

	b = aemit(size);
	if (!b)
		return;
	for (n=0; n<size; n++)
		b[n] = (v >> (8 * n)) & 0xff;
}

static void
dataexpr(char *p, int size)
{
	AExpr e;
	int64_t v, lim;
	uint off;

	for (;;) {
		if (!aexpr(&p, &e))
			aerr("expression expected");
		if (e.sym < 0) {
			v = e.add;
			lim = (int64_t)1 << (8 * size - 1);
			if (size < 8 && (v < -lim || v >= 2 * lim))
				aerr("value %"PRId64" does not fit in %d bytes", v, size);
			emitnum(v, size);
		} else {
			off = dataitem()->size;
			aemit(size);
			afix(off, size, FAbs, 0, &e);
		}
		aspace(&p);
		if (*p != ',')
			break;
		p++;
	}
}

static void
dascii(char *p, int zero)
{
	int c;
	uchar *b;

	for (;;) {
		aspace(&p);
		if (*p != '"')
			aerr("string expected");
		p++;
		while (*p && *p != '"') {
			c = (uchar)*p++;
			if (c == '\\') {
				c = (uchar)*p++;
				switch (c) {
				case 'n': c = '\n'; break;
				case 't': c = '\t'; break;
				case 'r': c = '\r'; break;
				case 'b': c = '\b'; break;
				case 'f': c = '\f'; break;
				case '0': case '1': case '2': case '3':
				case '4': case '5': case '6': case '7':
					c -= '0';
					if (*p >= '0' && *p <= '7') {
						c = c * 8 + (*p++ - '0');
						if (*p >= '0' && *p <= '7')
							c = c * 8 + (*p++ - '0');
					}
					break;
				case 'x':
					c = 0;
					while (hexval((uchar)*p) >= 0) {
						c = c * 16 + hexval((uchar)*p);
						p++;
					}
					break;
				default:
					break;
				}
			}
			b = aemit(1);
			if (b)
				*b = c;
		}
		if (*p == '"')
			p++;
		if (zero) {
			b = aemit(1);
			if (b)
				*b = 0;
		}
		aspace(&p);
		if (*p != ',')
			break;
		p++;
	}
}

static void
align(uint al, int fillgiven, int fill)
{
	AItem *i;

	if (fillgiven && fill != 0)
		aerr("alignment fill other than zero is not supported");
	if (al <= 1)
		return;
	i = newitem(IAlign);
	i->align = al;
	i->nops = !fillgiven && (asecs[acursec].flags & SExec);
	if (al > (uint)asecs[acursec].align)
		asecs[acursec].align = al;
}

static void
dsection(char *p)
{
	char name[256], *w;
	int flags, type, idx;
	size_t n;

	aspace(&p);
	if (*p == '"')
		w = aword(&p);
	else {
		/* jméno po čárku nebo mezeru (může obsahovat '-') */
		w = p;
		while (*p && *p != ',' && *p != ' ' && *p != '\t')
			p++;
		n = p - w;
		if (n >= sizeof name)
			aerr("section name too long");
		memcpy(name, w, n);
		name[n] = 0;
		w = name;
	}
	if (strlen(w) >= sizeof name)
		aerr("section name too long");
	if (w != name)
		strcpy(name, w);
	flags = -1;
	type = 1; /* SHT_PROGBITS */
	aspace(&p);
	if (acoff) {
		/* COFF (GNU as pro PE): příznaky jen z řetězce, bez něj data pro
		 * čtení i zápis; x kód, b neinicializovaná data, r jen pro čtení,
		 * d a w data (d s r jen pro čtení) */
		flags = SAlloc | SWrite;
		if (*p == ',') {
			int hasx, hasb, hasr, hasw, hasd;
			p++;
			aspace(&p);
			if (*p != '"')
				aerr("section flags expected");
			w = aword(&p);
			hasx = hasb = hasr = hasw = hasd = 0;
			for (; *w; w++)
				switch (*w) {
				case 'x': hasx = 1; break;
				case 'b': hasb = 1; break;
				case 'r': hasr = 1; break;
				case 'w': hasw = 1; break;
				case 'd': hasd = 1; break;
				default: aerr("unknown section flag %c", *w);
				}
			flags = SAlloc;
			if (hasx)
				flags |= SExec;
			if (hasb) {
				flags |= SNobits;
				type = 8;
			}
			if (hasw || ((hasd || hasb) && !hasr))
				flags |= SWrite;
		}
		idx = findsec(name);
		if (idx < 0) {
			idx = newsec(name, flags, type);
			/* výchozí zarovnání: GNU as 16 u .text*, .data* a .rdata*,
			 * jinak 4; LLVM 1 */
			if (A->machine == 183)
				asecs[idx].align = 1;
			else if (strncmp(name, ".text", 5) == 0 || strncmp(name, ".data", 5) == 0 || strncmp(name, ".rdata", 6) == 0)
				asecs[idx].align = 16;
			else
				asecs[idx].align = 4;
		}
		usesec(idx);
		return;
	}
	if (*p == ',') {
		p++;
		aspace(&p);
		if (*p != '"')
			aerr("section flags expected");
		w = aword(&p);
		flags = 0;
		for (; *w; w++)
			switch (*w) {
			case 'a': flags |= SAlloc; break;
			case 'w': flags |= SWrite; break;
			case 'x': flags |= SExec; break;
			case 'T': flags |= STls; break;
			default: aerr("unknown section flag %c", *w);
			}
		aspace(&p);
		if (*p == ',') {
			p++;
			aspace(&p);
			if (*p == '@' || *p == '%')
				p++;
			w = aword(&p);
			if (strcmp(w, "nobits") == 0) {
				type = 8;
				flags |= SNobits;
			} else if (strcmp(w, "note") == 0)
				type = 7;
			else if (strcmp(w, "progbits") != 0)
				aerr("unknown section type %s", w);
		}
	}
	idx = findsec(name);
	if (idx < 0) {
		if (flags < 0) {
			/* příznaky podle jména */
			if (strncmp(name, ".text", 5) == 0)
				flags = SAlloc | SExec;
			else if (strncmp(name, ".rodata", 7) == 0)
				flags = SAlloc;
			else if (strncmp(name, ".tbss", 5) == 0) {
				flags = SAlloc | SWrite | SNobits | STls;
				type = 8;
			} else if (strncmp(name, ".bss", 4) == 0) {
				flags = SAlloc | SWrite | SNobits;
				type = 8;
			} else if (strncmp(name, ".tdata", 6) == 0)
				flags = SAlloc | SWrite | STls;
			else if (strcmp(name, ".note.GNU-stack") == 0)
				flags = 0;
			else
				flags = SAlloc | SWrite;
		}
		idx = newsec(name, flags, type);
	}
	usesec(idx);
}

static void
dsym(char *p, int weak)
{
	char *w;
	ASymbol *s;
	int idx;

	for (;;) {
		w = aword(&p);
		if (!*w)
			aerr("symbol expected");
		idx = asymidx(w);
		s = &asyms[idx];
		if (weak)
			s->weak = 1;
		else
			s->global = 1;
		aspace(&p);
		if (*p != ',')
			break;
		p++;
	}
}

static void
dtype(char *p)
{
	char *w;
	ASymbol *s;
	int idx;

	w = aword(&p);
	idx = asymidx(w);
	s = &asyms[idx];
	aspace(&p);
	if (*p == ',')
		p++;
	aspace(&p);
	if (*p == '@' || *p == '%')
		p++;
	w = aword(&p);
	if (strcmp(w, "function") == 0)
		s->type = TFunc;
	else if (strcmp(w, "object") == 0)
		s->type = TObject;
	else
		aerr("unknown symbol type %s", w);
}

/* .size sym, výraz | .size sym, .-sym */
static void
dsize(char *p)
{
	char *w;
	ASymbol *s;
	AExpr e;
	int sym, sub, here;

	w = aword(&p);
	sym = asymidx(w);
	aspace(&p);
	if (*p != ',')
		aerr("',' expected");
	p++;
	aspace(&p);
	if (*p == '.' && !identchar((uchar)p[1])) {
		p++;
		aspace(&p);
		if (*p == '-') {
			p++;
			w = aword(&p);
			sub = asymidx(w);
			here = dot();
			s = &asyms[sym];
			s->hassize = 1;
			s->size.sym = here;
			s->size.add = 0;
			s->sizesub = sub;
			return;
		}
		aerr(".size: unsupported expression");
	}
	if (!aexpr(&p, &e))
		aerr("expression expected");
	s = &asyms[sym];
	s->hassize = 1;
	s->size = e;
}

static void
dfill(char *p)
{
	int64_t count, size, value, n;

	if (!anumber(&p, &count))
		aerr("count expected");
	size = 1;
	value = 0;
	aspace(&p);
	if (*p == ',') {
		p++;
		aspace(&p);
		if (!anumber(&p, &size))
			aerr("size expected");
		aspace(&p);
		if (*p == ',') {
			p++;
			aspace(&p);
			if (!anumber(&p, &value))
				aerr("value expected");
		}
	}
	if (size < 1 || size > 8)
		aerr("invalid fill size");
	for (n=0; n<count; n++)
		emitnum(value, size);
}

/* .comm symbol, velikost[, zarovnání]: common symbol (linker ho umístí) */
static void
dcomm(char *p)
{
	char *w;
	ASymbol *s;
	int64_t size, al;
	int sym;

	w = aword(&p);
	sym = asymidx(w);
	aspace(&p);
	if (*p != ',')
		aerr("',' expected");
	p++;
	aspace(&p);
	if (!anumber(&p, &size))
		aerr("size expected");
	al = 1;
	aspace(&p);
	if (*p == ',') {
		p++;
		aspace(&p);
		anumber(&p, &al);
	}
	s = &asyms[sym];
	if (s->defined)
		aerr("symbol %s is already defined", w);
	s->global = 1;
	s->type = TObject;
	s->hassize = 1;
	s->size.sym = -1;
	s->size.add = size;
	s->common = al > 0 ? al : 1;
}

static void
dalign(char *d, char *p)
{
	int64_t v, fill;
	int fillgiven;

	if (!anumber(&p, &v))
		aerr("alignment expected");
	if (strcmp(d, ".balign") != 0) {
		if (v > 30)
			aerr("alignment too large");
		v = (int64_t)1 << v;
	}
	fillgiven = 0;
	fill = 0;
	aspace(&p);
	if (*p == ',') {
		p++;
		aspace(&p);
		if (anumber(&p, &fill))
			fillgiven = 1;
		aspace(&p);
		if (*p == ',')
			aerr("alignment with a maximum skip is not supported");
	}
	align(v, fillgiven, fill);
}

static void
directive(char *d, char *p)
{
	int64_t v;

	aspace(&p);
	if (strcmp(d, ".text") == 0)
		usesec(findsec(".text"));
	else if (strcmp(d, ".data") == 0)
		usesec(findsec(".data"));
	else if (strcmp(d, ".bss") == 0)
		usesec(findsec(".bss"));
	else if (strcmp(d, ".section") == 0)
		dsection(p);
	else if (strcmp(d, ".globl") == 0 || strcmp(d, ".global") == 0)
		dsym(p, 0);
	else if (strcmp(d, ".weak") == 0)
		dsym(p, 1);
	else if (strcmp(d, ".type") == 0)
		dtype(p);
	else if (strcmp(d, ".size") == 0)
		dsize(p);
	else if (strcmp(d, ".byte") == 0)
		dataexpr(p, 1);
	else if (strcmp(d, ".short") == 0 || strcmp(d, ".2byte") == 0 || strcmp(d, ".hword") == 0 || strcmp(d, ".half") == 0)
		dataexpr(p, 2);
	else if (strcmp(d, ".word") == 0)
		dataexpr(p, A->wordsize ? A->wordsize : 2);
	else if (strcmp(d, ".int") == 0 || strcmp(d, ".long") == 0 || strcmp(d, ".4byte") == 0)
		dataexpr(p, 4);
	else if (strcmp(d, ".quad") == 0 || strcmp(d, ".8byte") == 0 || strcmp(d, ".xword") == 0 || strcmp(d, ".dword") == 0)
		dataexpr(p, 8);
	else if (strcmp(d, ".ascii") == 0)
		dascii(p, 0);
	else if (strcmp(d, ".asciz") == 0 || strcmp(d, ".string") == 0)
		dascii(p, 1);
	else if (strcmp(d, ".zero") == 0 || strcmp(d, ".skip") == 0 || strcmp(d, ".space") == 0) {
		AExpr ce;
		if (!aexpr(&p, &ce) || ce.sym >= 0 || ce.add < 0)
			aerr("count expected");
		v = ce.add;
		aemit(v);
	} else if (strcmp(d, ".fill") == 0)
		dfill(p);
	else if (strcmp(d, ".balign") == 0 || strcmp(d, ".p2align") == 0 || strcmp(d, ".align") == 0)
		dalign(d, p);
	else if (strcmp(d, ".equ") == 0 || strcmp(d, ".set") == 0)
		dequ(p);
	else if (strcmp(d, ".file") == 0) {
		if (!filename)
			filename = dupstr(aword(&p));
	} else if (strcmp(d, ".comm") == 0)
		dcomm(p);
	else if (strcmp(d, ".syntax") == 0 || strcmp(d, ".arm") == 0 || strcmp(d, ".fpu") == 0
	|| strcmp(d, ".arch") == 0 || strcmp(d, ".cpu") == 0 || strcmp(d, ".ident") == 0
	|| strcmp(d, ".subsections_via_symbols") == 0
	|| strcmp(d, ".option") == 0 || strcmp(d, ".attribute") == 0)
		; /* .option: bez relaxace a komprese (jako -mno-relax, rv64g) */
	else
		aerr("unknown directive %s", d);
}

/* ---- řádky ---- */

/* Odstraní komentáře (# nebo // do konce řádku, blokový v řádku)
 * a koncové mezery; respektuje řetězce. */
static void
stripcomment(char *line)
{
	char *p, *q;
	int instr;

	instr = 0;
	for (p=line; *p; p++) {
		if (instr) {
			if (*p == '\\' && p[1])
				p++;
			else if (*p == '"')
				instr = 0;
			continue;
		}
		if (*p == '"')
			instr = 1;
		else if ((A->comment && *p == A->comment) || (*p == '/' && p[1] == '/')) {
			*p = 0;
			break;
		} else if (*p == '/' && p[1] == '*') {
			q = findstr(p + 2, "*/");
			if (!q)
				aerr("unterminated comment");
			memmove(p, q + 2, strlen(q + 2) + 1);
			p--;
		}
	}
	q = line + strlen(line);
	while (q > line && (q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\r'))
		*--q = 0;
}

static int expandmacro(char *name, char *args);

static void
statement(char *p)
{
	char *w, *q, d[64];
	int64_t v;
	uint n;

	aspace(&p);
	if (!*p)
		return;
	/* návěští: slovo nebo číslo následované ':' */
	q = p;
	if (isdigit((uchar)*q)) {
		anumber(&q, &v);
		aspace(&q);
		if (*q == ':') {
			if (v < 0 || v > 100000)
				aerr("invalid numeric label");
			if (v >= nlocnum) {
				locnum = realloc(locnum, (v + 1) * sizeof locnum[0]);
				if (!locnum)
					die("out of memory");
				while (nlocnum <= v)
					locnum[nlocnum++] = 0;
			}
			locnum[v]++;
			herelabel(numlabel((int)v, 0));
			statement(q + 1);
			return;
		}
	} else if (identchar((uchar)*q) || *q == '"') {
		w = aword(&q);
		aspace(&q);
		if (*q == ':') {
			herelabel(w);
			statement(q + 1);
			return;
		}
	}
	if (*p == '.') {
		q = p;
		while (identchar((uchar)*q))
			q++;
		n = q - p;
		if (n >= sizeof d)
			aerr("directive too long");
		memcpy(d, p, n);
		d[n] = 0;
		directive(d, q);
		return;
	}
	/* instrukce: mnemonika a operandy */
	q = p;
	while (*q && *q != ' ' && *q != '\t')
		q++;
	if (*q)
		*q++ = 0;
	aspace(&q);
	if (nmacros && expandmacro(p, q))
		return;
	/* LLVM v COFF zarovnání sekce instrukcemi nezvyšuje */
	if (A->minalign > asecs[acursec].align && !acoff)
		asecs[acursec].align = A->minalign;
	if (!A->insn(p, q))
		aerr("unknown instruction %s", p);
}

static void
readfile(char *path)
{
	FILE *f;
	size_t cap, n, got;

	f = fopen(path, "rb");
	if (!f)
		aerr("cannot open %s", path);
	cap = 1 << 16;
	src = emalloc(cap);
	n = 0;
	for (;;) {
		if (n + 4096 > cap) {
			cap *= 2;
			src = realloc(src, cap);
			if (!src)
				die("out of memory");
		}
		got = fread(src + n, 1, cap - n - 1, f);
		if (got == 0)
			break;
		n += got;
	}
	fclose(f);
	src[n] = 0;
}

/* Řádek bez komentáře: příkazy oddělené ';' mimo řetězce. */
static void
procline(char *line)
{
	char *s, *t;
	int instr;

	s = line;
	instr = 0;
	for (t=line; *t; t++) {
		if (instr && *t == '\\' && t[1])
			t++;
		else if (*t == '"')
			instr = !instr;
		else if (*t == ';' && !instr) {
			*t = 0;
			statement(s);
			s = t + 1;
		}
	}
	statement(s);
}

/* Volání makra: argumenty oddělené čárkami, v těle se nahradí
 * \parametr; každý řádek těla se zpracuje jako běžný řádek. */
static int
expandmacro(char *name, char *args)
{
	AMacro *m;
	char *arg[8], *p, *q, *b, *e, *out, *o;
	uint k;
	int narg, n, len, found, save;
	size_t cap;

	for (k=0; k<nmacros; k++)
		if (strcmp(macros[k].name, name) == 0)
			break;
	if (k == nmacros)
		return 0;
	m = &macros[k];
	if (++macrodepth > 20)
		aerr("macro expansion too deep");
	narg = 0;
	p = args;
	for (;;) {
		aspace(&p);
		if (!*p)
			break;
		q = p;
		while (*q && *q != ',')
			q++;
		if (narg >= 8)
			aerr("too many macro arguments");
		arg[narg++] = p;
		if (*q == ',') {
			*q = 0;
			p = q + 1;
		} else
			p = q;
	}
	save = aline;
	b = m->body;
	while (*b) {
		e = strchr(b, '\n');
		len = e ? (int)(e - b) : (int)strlen(b);
		cap = 2 * len + 64;
		out = emalloc(cap);
		o = out;
		for (n=0; n<len; ) {
			if (b[n] == '\\') {
				if (b[n+1] == '(' && b[n+2] == ')') {
					n += 3;
					continue;
				}
				found = 0;
				for (k=0; k<(uint)m->nparam; k++) {
					int pl = strlen(m->param[k]);
					if (strncmp(b + n + 1, m->param[k], pl) == 0 && !identchar((uchar)b[n+1+pl])) {
						char *a = (int)k < narg ? arg[k] : "";
						if ((size_t)(o - out) + strlen(a) + len + 8 > cap) {
							size_t off = o - out;
							cap = 2 * (cap + strlen(a));
							out = realloc(out, cap);
							if (!out)
								die("out of memory");
							o = out + off;
						}
						strcpy(o, a);
						o += strlen(a);
						n += 1 + pl;
						found = 1;
						break;
					}
				}
				if (found)
					continue;
			}
			*o++ = b[n++];
		}
		*o = 0;
		stripcomment(out);
		procline(out);
		free(out);
		aline = save;
		b = e ? e + 1 : b + len;
	}
	macrodepth--;
	return 1;
}

static void
aparse(void)
{
	static char *line;
	static size_t cap;
	char *p, *e, *s, *t, *reptstart;
	size_t n;
	int reptn, reptline, skip;

	p = src;
	aline = 0;
	reptstart = 0;
	reptn = 0;
	reptline = 0;
	skip = 0;
	while (*p) {
		aline++;
		e = strchr(p, '\n');
		n = e ? (size_t)(e - p) : strlen(p);
		/* řádek do pracovní kopie: zdroj zůstává pro .rept */
		if (n + 1 > cap) {
			cap = 2 * (n + 1);
			line = realloc(line, cap);
			if (!line)
				die("out of memory");
		}
		memcpy(line, p, n);
		line[n] = 0;
		p = e ? e + 1 : p + n;
		stripcomment(line);
		s = line;
		aspace(&s);
		/* .rept N … .endr: blok se zpracuje N-krát (jen pro testy) */
		if (strncmp(s, ".rept", 5) == 0 && !identchar((uchar)s[5])) {
			int64_t v;
			s += 5;
			aspace(&s);
			if (!anumber(&s, &v) || v < 0)
				aerr("count expected");
			reptn = v;
			reptstart = p;
			reptline = aline;
			skip = reptn == 0;
			continue;
		}
		if (strncmp(s, ".endr", 5) == 0 && !identchar((uchar)s[5])) {
			if (!reptstart)
				aerr(".endr without .rept");
			if (--reptn > 0) {
				p = reptstart;
				aline = reptline;
			} else {
				reptstart = 0;
				skip = 0;
			}
			continue;
		}
		if (skip)
			continue;
		/* .macro: tělo až po .endm se uloží bez zpracování */
		if (strncmp(s, ".macro", 6) == 0 && !identchar((uchar)s[6])) {
			char *body, *bstart;
			size_t blen;
			AMacro *m;
			s += 6;
			GROW(macros, nmacros + 1);
			m = &macros[nmacros++];
			memset(m, 0, sizeof *m);
			aspace(&s);
			m->name = dupstr(aword(&s));
			for (;;) {
				aspace(&s);
				if (*s == ',')
					s++;
				aspace(&s);
				if (!identchar((uchar)*s))
					break;
				if (m->nparam >= 8)
					aerr("too many macro parameters");
				m->param[m->nparam++] = dupstr(aword(&s));
			}
			bstart = p;
			for (;;) {
				if (!*p)
					aerr(".macro without .endm");
				aline++;
				e = strchr(p, '\n');
				n = e ? (size_t)(e - p) : strlen(p);
				t = p;
				while (*t == ' ' || *t == '\t')
					t++;
				if (strncmp(t, ".endm", 5) == 0 && !identchar((uchar)t[5])) {
					blen = p - bstart;
					p = e ? e + 1 : p + n;
					break;
				}
				p = e ? e + 1 : p + n;
			}
			body = emalloc(blen + 1);
			memcpy(body, bstart, blen);
			body[blen] = 0;
			m->body = body;
			continue;
		}
		procline(line);
	}
}

/* ---- rozvržení ---- */

/* Posun položky yi od začátku jejího fragmentu GNU as: fragment začíná
 * za každým relaxovatelným skokem a zarovnáním (první průchod relaxace
 * RISC-V, kdy GNU as adresy dopředných cílů ještě nezná). */
static int64_t
fragoff(ASec *s, uint m, uint yi)
{
	uint k, b;
	int64_t v;

	b = m;
	for (k=m+1; k<yi && k<s->nitem; k++)
		if (s->item[k].kind != IData)
			b = k;
	v = 0;
	for (k=b+1; k<yi && k<s->nitem; k++)
		v += s->item[k].size;
	return v;
}

/* Hodnota výrazu, je-li symbol definovaný v sekci sec. */
static int
exprin(AExpr *e, int sec, int64_t *v)
{
	ASymbol *s;

	if (e->sym < 0) {
		*v = e->add;
		return 1;
	}
	s = &asyms[e->sym];
	if (!s->defined || s->sec != sec)
		return 0;
	*v = s->val + e->add;
	return 1;
}

/* Adresa symbolu během průchodu relaxací: položky před aktuální (m) už
 * mají nové posuny, položky za ní staré plus dosavadní nárůst stretch
 * (jako relax_segment v GNU as). */
/* Kolik bajtů kladného nárůstu průchodu mohou pohltit zarovnání mezi
 * položkami a a b (každé nejvýše align-1; GNU as nepřičítá nárůst
 * k dopřednému cíli za zarovnáním, aby skok nepřecenil svůj dosah). */
static int64_t
absorb(ASec *s, uint a, uint b)
{
	uint m;
	int64_t n;

	n = 0;
	for (m=a+1; m<b && m<s->nitem; m++)
		if (s->item[m].kind == IAlign)
			n += s->item[m].align - 1;
	return n;
}

/* Adresa symbolu během průchodu relaxací: položky před aktuální (m) už
 * mají nové posuny, položky za ní staré plus odhad nárůstu stretch (jako
 * relax_frag v GNU as). */
static int64_t
symaddr(ASymbol *y, int sec, uint m, int64_t stretch)
{
	ASec *s;
	int64_t v;

	s = &asecs[sec];
	if (y->item < s->nitem)
		v = s->item[y->item].off;
	else
		v = s->size;
	if (y->item > m) {
		if (stretch > 0) {
			stretch -= absorb(s, m, y->item);
			if (stretch < 0)
				stretch = 0;
		}
		v += stretch;
	}
	return v;
}

/* Počáteční rozvržení (všechny skoky krátké), pak průchody jako GNU as:
 * položky v pořadí, zarovnání podle nové adresy, skok se prodlouží, když
 * se cíl nedosáhne; vrací nárůst průchodu (0 = hotovo). */
static int64_t
layout(int initial)
{
	ASec *s;
	AItem *i;
	ASymbol *y;
	uint n, m;
	uint64_t off;
	int64_t stretch, total, t, disp, size;

	total = 0;
	for (n=0; n<nasecs; n++) {
		s = &asecs[n];
		stretch = 0;
		off = 0;
		for (m=0; m<s->nitem; m++) {
			i = &s->item[m];
			if (initial)
				i->off = off;
			else
				i->off += stretch;
			size = i->size;
			switch (i->kind) {
			case IAlign:
				size = (int64_t)(((i->off + i->align - 1) & -(uint64_t)i->align) - i->off);
				break;
			case IJump:
				y = &asyms[i->target.sym];
				if (A->relaxmode == 1) {
					/* forma znovu v každém průchodu: cíl před skokem má
					 * adresu tohoto průchodu, cíl za ním adresu minulého,
					 * v prvním průchodu jen posun ve svém fragmentu */
					if (!y->defined || y->sec != (int)n || y->weak)
						i->jlong = 1;
					else {
						if (y->item <= m)
							t = symaddr(y, n, m, 0);
						else if (initial)
							t = fragoff(s, m, y->item);
						else
							t = y->item < s->nitem ? (int64_t)s->item[y->item].off : (int64_t)s->size;
						t += i->target.add;
						i->jlong = !A->jfits(i, t - (int64_t)i->off);
					}
					size = A->jsize(i);
					break;
				}
				if (!i->jlong && !initial) {
					size = A->jsize(i);
					if (!y->defined || y->sec != (int)n || y->weak)
						i->jlong = 1;
					else {
						t = symaddr(y, n, m, stretch) + i->target.add;
						disp = t - (int64_t)(i->off + size);
						if (!A->jfits(i, disp)) {
							i->jlong = 1;
						}
					}
				}
				size = A->jsize(i);
				break;
			}
			if (!initial)
				stretch += size - (int64_t)i->size;
			i->size = size;
			off = i->off + size;
		}
		s->size = off;
		total += stretch < 0 ? -stretch : stretch;
	}
	for (n=0; n<nasyms; n++) {
		y = &asyms[n];
		if (!y->defined)
			continue;
		s = &asecs[y->sec];
		y->val = y->item < s->nitem ? s->item[y->item].off : s->size;
	}
	return total;
}

/* ---- výstup ELF ---- */

typedef struct Out Out;
struct Out {
	uchar *b;
	uint64_t n, cap;
};

static void
oput(Out *o, void *p, uint64_t n)
{
	while (o->n + n > o->cap) {
		o->cap = o->cap ? 2 * o->cap : 65536;
		o->b = realloc(o->b, o->cap);
		if (!o->b)
			die("out of memory");
	}
	memcpy(o->b + o->n, p, n);
	o->n += n;
}

static void
putval(uchar *p, int64_t v, int size)
{
	int n;

	for (n=0; n<size; n++)
		p[n] = (v >> (8 * n)) & 0xff;
}

static void
onum(Out *o, uint64_t v, int size)
{
	uchar b[8];

	putval(b, v, size);
	oput(o, b, size);
}

static void
ozero(Out *o, uint64_t n)
{
	static uchar z[64];

	while (n > sizeof z) {
		oput(o, z, sizeof z);
		n -= sizeof z;
	}
	oput(o, z, n);
}

static void
opad(Out *o, uint64_t al)
{
	ozero(o, ((o->n + al - 1) & -al) - o->n);
}

typedef struct Rel Rel;
struct Rel {
	uint64_t off;
	int sym;      /* index symbolu (asyms), je-li sec < 0 */
	int sec;      /* symbol sekce sec (>= 0) */
	int type;
	int64_t add;
};

static Rel *rels;
static uint nrels;

/* Oprava v obsahu sekce: vyřešit, nebo zapsat relokaci. jump: skok nebo
 * volání (relativní odkaz na globální symbol téže sekce se u skoku
 * vyřeší, u volání zůstane relokace jako v GNU as). */
static void
resolvefix(int secidx, uchar *buf, uint64_t pos, AFix *f, int jump)
{
	ASymbol *y;
	int64_t v, a;
	int type, es;

	y = f->e.sym >= 0 ? &asyms[f->e.sym] : 0;
	if (!y) {
		if (f->kind != FAbs)
			aerr("pc-relative reference to a constant");
		putval(buf + pos, f->e.add, f->size);
		return;
	}
	if (f->kind != FAbs && y->defined && y->sec == secidx
	&& (!A->localonly || y->local)
	&& ((jump && A->resolveglobal) || (!y->global && !y->weak))) {
		/* S + A - P, A = add + pcoff, P = pozice pole */
		v = (int64_t)y->val + f->e.add + f->pcoff - (int64_t)pos;
		if (f->enc) {
			A->fixenc(buf + pos, f, v);
			return;
		}
		if (f->size == 1 && (v < -128 || v > 127))
			aerr("jump out of range");
		putval(buf + pos, v, f->size);
		return;
	}
	type = A->reloc(f);
	if (!type)
		aerr("cannot relocate a reference to %s", y->name);
	a = f->e.add;
	if (f->kind != FAbs)
		a += f->pcoff;
	/* předběžná hodnota do pole s relokací (RISC-V): S + A - P u skoků
	 * (S = posun symbolu v jeho sekci, 0 u nedefinovaného), jen A
	 * u absolutních %hi/%lo */
	if (A->tentative && f->enc) {
		v = f->e.add;
		if (f->kind != FAbs)
			v += (y->defined ? (int64_t)y->val : 0) + f->pcoff - (int64_t)pos;
		A->tentative(buf + pos, f, v);
	}
	es = -1;
	if (f->relsym >= 0) {
		/* %pcrel_lo: symbol .L0 u auipc, addend 0 */
		y = &asyms[f->relsym];
		es = f->relsym;
		a = 0;
	} else if (A->keeplocalsym == 2 || (A->symreloc && A->symreloc(f))
	|| !(y->defined && !y->global && !y->weak && (y->local || !A->keeplocalsym || y->type != TFunc)))
		es = f->e.sym;
	/* jinak symbol sekce; arm64 GNU as nechává relokace na místní funkce
	 * na symbolu, RISC-V na každém symbolu včetně .L */
	GROW(rels, nrels + 1);
	rels[nrels].off = pos;
	if (es >= 0) {
		rels[nrels].sym = es;
		rels[nrels].sec = -1;
		if (y->local)
			y->keep = 1;
	} else {
		rels[nrels].sym = 0;
		rels[nrels].sec = y->sec;
		a += y->val;
	}
	rels[nrels].type = type;
	if (A->elf64)
		rels[nrels].add = a;
	else {
		/* REL: addend v poli (ARM podle kódování instrukce) */
		if (A->relenc && f->enc)
			A->relenc(buf + pos, f, a);
		else
			putval(buf + pos, a, f->size);
		rels[nrels].add = 0;
	}
	nrels++;
}

static int64_t
symsize(ASymbol *y)
{
	int64_t sz;

	if (!y->hassize)
		return 0;
	sz = y->size.add;
	if (y->size.sym >= 0)
		sz += asyms[y->size.sym].val;
	if (y->sizesub >= 0)
		sz -= asyms[y->sizesub].val;
	return sz;
}

/* Záznam symbolu (ELF64: name, info, other, shndx, value, size; ELF32:
 * name, value, size, info, other, shndx). */
static void
osym(Out *o, uint64_t name, int info, int shndx, uint64_t value, uint64_t size)
{
	if (A->elf64) {
		onum(o, name, 4);
		onum(o, info, 1);
		onum(o, 0, 1);
		onum(o, shndx, 2);
		onum(o, value, 8);
		onum(o, size, 8);
	} else {
		onum(o, name, 4);
		onum(o, value, 4);
		onum(o, size, 4);
		onum(o, info, 1);
		onum(o, 0, 1);
		onum(o, shndx, 2);
	}
}

static void
osh(Out *o, uint64_t name, int type, uint64_t flags, uint64_t off, uint64_t size, int link, int info, uint64_t align, uint64_t ent)
{
	int w;

	w = A->elf64 ? 8 : 4;
	onum(o, name, 4);
	onum(o, type, 4);
	onum(o, flags, w);
	onum(o, 0, w);
	onum(o, off, w);
	onum(o, size, w);
	onum(o, link, 4);
	onum(o, info, 4);
	onum(o, align, w);
	onum(o, ent, w);
}

/* Jméno symbolu v ELF: vnitřní jména místních návěští (.L#f<k>#jméno
 * po souborech, .L#n#i číselná, .L#pc<k> u auipc) dostanou podobu GNU as:
 * .Ljméno, .Ln^Bi a „.L0 “. */
static char *
symwname(ASymbol *y)
{
	static char buf[64];
	char *p;
	int num, inst;

	p = y->name;
	if (strncmp(p, ".L#f", 4) == 0) {
		p = strchr(p + 4, '#');
		if (!p)
			return y->name;
		p++;
		if (*p != '#') {
			if (strlen(p) + 3 > sizeof buf)
				return p;
			sprintf(buf, ".L%s", p);
			return buf;
		}
	}
	if (strncmp(p, ".L#pc", 5) == 0)
		return ".L0 ";
	if (strncmp(p, ".L#", 3) == 0 && isdigit((uchar)p[3])) {
		/* .L#n#i: číselné návěští n, instance i */
		num = 0;
		for (p+=3; isdigit((uchar)*p); p++)
			num = 10 * num + (*p - '0');
		if (*p == '#' && isdigit((uchar)p[1])) {
			inst = 0;
			for (p++; isdigit((uchar)*p); p++)
				inst = 10 * inst + (*p - '0');
			if (*p == 0) {
				sprintf(buf, ".L%d^B%d", num, inst);
				return buf;
			}
		}
	}
	return y->name;
}

static void
writeobj(char *out)
{
	Out o, strtab, shstr, symtab;
	ASec *s;
	ASymbol *y;
	AItem *i;
	AFix fx;
	FILE *f;
	uchar *buf;
	uint n, m, k, nsyms, firstglobal, nsh;
	uint64_t pos, pos2, strpos, shstrpos, shoff, fl;
	Rel **secrels;
	char *nm;
	int64_t disp, t;
	int resolved, w, symtabidx, strtabidx, shstridx, symsz, info;
	uint64_t *secoff, *reloff, *relsz, *secname, *relname;
	uint *relcnt;
	char *stdnames[3];

	memset(&o, 0, sizeof o);
	memset(&strtab, 0, sizeof strtab);
	memset(&shstr, 0, sizeof shstr);
	memset(&symtab, 0, sizeof symtab);
	w = A->elf64 ? 8 : 4;
	symsz = A->elf64 ? 24 : 16;
	stdnames[0] = ".symtab";
	stdnames[1] = ".strtab";
	stdnames[2] = ".shstrtab";

	/* indexy sekcí v ELF (relokační sekce až po zjištění, které jsou) */
	nsh = 1;
	for (n=0; n<nasecs; n++) {
		asecs[n].elfidx = nsh++;
		asecs[n].relaidx = 0;
	}

	/* obsah sekcí; relokace si každá sekce schová, symboly dostanou
	 * indexy až podle toho, na které z nich relokace odkazují */
	elfsym = emalloc((nasyms + 1) * sizeof elfsym[0]);
	for (n=0; n<nasyms; n++)
		elfsym[n] = -1;
	secrels = emalloc((nasecs + 1) * sizeof secrels[0]);
	secoff = emalloc((nasecs + 1) * sizeof secoff[0]);
	reloff = emalloc((nasecs + 1) * sizeof reloff[0]);
	relsz = emalloc((nasecs + 1) * sizeof relsz[0]);
	relcnt = emalloc((nasecs + 1) * sizeof relcnt[0]);
	ozero(&o, A->elf64 ? 64 : 52);
	for (n=0; n<nasecs; n++) {
		s = &asecs[n];
		nrels = 0;
		relcnt[n] = 0;
		reloff[n] = 0;
		relsz[n] = 0;
		opad(&o, s->align);
		secoff[n] = o.n;
		if (s->flags & SNobits)
			continue;
		buf = emalloc(s->size + 1);
		memset(buf, 0, s->size + 1);
		for (m=0; m<s->nitem; m++) {
			i = &s->item[m];
			aline = i->line;
			switch (i->kind) {
			case IData:
				if (i->size)
					memcpy(buf + i->off, i->data, i->size);
				for (k=0; k<i->nfix; k++)
					resolvefix(n, buf, i->off + i->fix[k].off, &i->fix[k], 0);
				break;
			case IAlign:
				if (i->nops && i->size)
					A->nops(buf + i->off, i->size);
				break;
			case IJump:
				y = &asyms[i->target.sym];
				resolved = exprin(&i->target, n, &t);
				disp = resolved ? (int64_t)t - (int64_t)(i->off + i->size) : 0;
				A->jemit(i, buf + i->off, disp, resolved);
				/* relokace: cíl jinde, nebo globální/jiný než .L symbol,
				 * který se podle architektury nevyřeší (RISC-V dostane
				 * relokaci i s vyplněnou předběžnou hodnotou) */
				if (!resolved || (!A->resolveglobal && (y->global || y->weak))
				|| (A->localonly && !y->local)) {
					memset(&fx, 0, sizeof fx);
					fx.size = 4;
					fx.kind = FBranch;
					/* x86 počítá od konce instrukce, RISC-V od začátku */
					fx.pcoff = A->relaxmode == 1 ? 0 : -4;
					fx.enc = A->jenc ? A->jenc(i) : 0;
					fx.relsym = -1;
					fx.e = i->target;
					resolvefix(n, buf, i->off + i->size - 4, &fx, 1);
				}
				break;
			}
		}
		oput(&o, buf, s->size);
		free(buf);
		relcnt[n] = nrels;
		secrels[n] = rels;
		rels = 0;
		nrels = 0;
	}

	/* tabulka symbolů: 0, soubor, symboly sekcí, místní, globální */
	nsyms = 1;
	ozero(&strtab, 1);
	ozero(&symtab, symsz);
	if (filename) {
		nsyms++;
		osym(&symtab, strtab.n, 4, 0xfff1, 0, 0); /* STT_FILE, SHN_ABS */
		oput(&strtab, filename, strlen(filename) + 1);
	}
	for (n=0; n<nasecs; n++) {
		asecs[n].symidx = nsyms++;
		osym(&symtab, 0, 3, asecs[n].elfidx, 0, 0); /* STT_SECTION */
	}
	firstglobal = nsyms;
	for (k=0; k<2; k++) {
		if (k == 1)
			firstglobal = nsyms;
		for (n=0; n<nasyms; n++) {
			y = &asyms[n];
			if (y->local && !y->keep)
				continue;
			if (y->isabs) {
				/* .equ: místní absolutní symbol (GNU as ho zapisuje) */
				if (k != 0)
					continue;
				elfsym[n] = nsyms++;
				osym(&symtab, strtab.n, 0, 0xfff1, y->absval, 0);
				nm = symwname(y);
				oput(&strtab, nm, strlen(nm) + 1);
				continue;
			}
			if ((uint)(y->global || y->weak || !y->defined) != k)
				continue;
			elfsym[n] = nsyms++;
			info = (y->weak ? 2 : y->global || !y->defined ? 1 : 0) << 4;
			info |= y->type == TFunc ? 2 : y->type == TObject ? 1 : 0;
			if (y->common)
				osym(&symtab, strtab.n, info, 0xfff2, y->common, symsize(y));
			else
				osym(&symtab, strtab.n, info, y->defined ? asecs[y->sec].elfidx : 0,
					y->defined ? y->val : 0, symsize(y));
			nm = symwname(y);
			oput(&strtab, nm, strlen(nm) + 1);
		}
	}

	/* relokace */
	for (n=0; n<nasecs; n++) {
		if (!relcnt[n])
			continue;
		rels = secrels[n];
		opad(&o, w);
		reloff[n] = o.n;
		for (k=0; k<relcnt[n]; k++) {
			if (rels[k].sec >= 0)
				m = asecs[rels[k].sec].symidx;
			else {
				if (elfsym[rels[k].sym] < 0)
					die("as: relocation symbol without a table entry");
				m = elfsym[rels[k].sym];
			}
			if (A->elf64) {
				onum(&o, rels[k].off, 8);
				onum(&o, ((uint64_t)m << 32) | (uint32_t)rels[k].type, 8);
				onum(&o, rels[k].add, 8);
			} else {
				onum(&o, rels[k].off, 4);
				onum(&o, ((uint32_t)m << 8) | (uint32_t)rels[k].type, 4);
			}
		}
		relsz[n] = o.n - reloff[n];
		free(rels);
		rels = 0;
	}

	/* konečné indexy sekcí: relokační sekce za svou sekcí */
	nsh = 1;
	for (n=0; n<nasecs; n++) {
		asecs[n].elfidx = nsh++;
		if (relcnt[n])
			asecs[n].relaidx = nsh++;
	}
	symtabidx = nsh++;
	strtabidx = nsh++;
	shstridx = nsh++;
	for (n=0; n<nasecs; n++)
		putval(symtab.b + asecs[n].symidx * symsz + (A->elf64 ? 6 : 14), asecs[n].elfidx, 2);
	for (n=0; n<nasyms; n++) {
		y = &asyms[n];
		if ((!y->local || y->keep) && y->defined)
			putval(symtab.b + elfsym[n] * symsz + (A->elf64 ? 6 : 14), asecs[y->sec].elfidx, 2);
	}

	/* .symtab, .strtab, .shstrtab a hlavičky sekcí */
	opad(&o, w);
	pos = o.n;
	oput(&o, symtab.b, symtab.n);
	strpos = o.n;
	oput(&o, strtab.b, strtab.n);
	ozero(&shstr, 1);
	secname = emalloc((nasecs + 1) * sizeof secname[0]);
	relname = emalloc((nasecs + 1) * sizeof relname[0]);
	for (n=0; n<nasecs; n++) {
		secname[n] = shstr.n;
		oput(&shstr, asecs[n].name, strlen(asecs[n].name) + 1);
		if (relcnt[n]) {
			relname[n] = shstr.n;
			oput(&shstr, A->elf64 ? ".rela" : ".rel", A->elf64 ? 5 : 4);
			oput(&shstr, asecs[n].name, strlen(asecs[n].name) + 1);
		}
	}
	shstrpos = o.n;
	pos2 = shstr.n;
	for (k=0; k<3; k++)
		oput(&shstr, stdnames[k], strlen(stdnames[k]) + 1);
	oput(&o, shstr.b, shstr.n);
	opad(&o, w);
	shoff = o.n;
	osh(&o, 0, 0, 0, 0, 0, 0, 0, 0, 0);
	for (n=0; n<nasecs; n++) {
		s = &asecs[n];
		fl = 0;
		if (s->flags & SWrite)
			fl |= 1;
		if (s->flags & SAlloc)
			fl |= 2;
		if (s->flags & SExec)
			fl |= 4;
		if (s->flags & STls)
			fl |= 0x400;
		osh(&o, secname[n], s->type, fl, secoff[n], s->size, 0, 0, s->align, 0);
		if (relcnt[n])
			osh(&o, relname[n], A->elf64 ? 4 : 9, 0x40, reloff[n], relsz[n], symtabidx, s->elfidx, w, A->elf64 ? 24 : 8);
	}
	osh(&o, pos2, 2, 0, pos, symtab.n, strtabidx, firstglobal, w, symsz);
	osh(&o, pos2 + 8, 3, 0, strpos, strtab.n, 0, 0, 1, 0);
	osh(&o, pos2 + 16, 3, 0, shstrpos, shstr.n, 0, 0, 1, 0);

	/* hlavička ELF */
	memcpy(o.b, "\177ELF", 4);
	o.b[4] = A->elf64 ? 2 : 1;
	o.b[5] = 1;
	o.b[6] = 1;
	putval(o.b + 16, 1, 2); /* ET_REL */
	putval(o.b + 18, A->machine, 2);
	putval(o.b + 20, 1, 4);
	putval(o.b + (A->elf64 ? 48 : 36), A->eflags, 4);
	if (A->elf64) {
		putval(o.b + 40, shoff, 8);
		putval(o.b + 52, 64, 2);
		putval(o.b + 58, 64, 2);
		putval(o.b + 60, nsh, 2);
		putval(o.b + 62, shstridx, 2);
	} else {
		putval(o.b + 32, shoff, 4);
		putval(o.b + 40, 52, 2);
		putval(o.b + 46, 40, 2);
		putval(o.b + 48, nsh, 2);
		putval(o.b + 50, shstridx, 2);
	}
	f = fopen(out, "wb");
	if (!f)
		aerr("cannot create %s", out);
	for (pos=0; pos<o.n; pos++)
		fputc(o.b[pos], f);
	fclose(f);
}

/* ---- COFF (Windows) ----
 *
 * Dvě podoby podle toho, čí výstup je vzorem:
 *
 * x86-64 jako GNU as pro PE (pe-x86-64): hlavička s LINE_NUMS_STRIPPED
 * (bez relokací i RELOCS_STRIPPED), sekce doplněné na své zarovnání (kód
 * bajty 0x90, data nulami) s délkou bez výplně v pomocném záznamu,
 * relokace proti definovanému symbolu vždy přes symbol jeho sekce
 * s posunem v poli, odkaz relativní k PC na symbol téže sekce se vyřeší.
 * Symboly: .file (bez direktivy „fake“), místní (C_LABEL v .text, jinde
 * C_STAT), symboly sekcí, definované globální, nedefinované; tabulka
 * řetězců v pořadí použití.
 *
 * arm64 jako LLVM (clang --target=aarch64-windows-gnu): hlavička bez
 * příznaků, sekce bez výplně, pomocný záznam s CRC obsahu a číslem
 * sekce, relokace na dočasná návěští .L přes symbol sekce, jinak na
 * symbol sám, addend v poli instrukce; odkaz relativní k PC na symbol
 * téže sekce se vyřeší i u globálních (na rozdíl od ELF). Symboly: sekce, ostatní v pořadí
 * vzniku, .file až na konci (dlouhé jméno ve více pomocných záznamech);
 * tabulka řetězců seřazená jako StringTableBuilder (sestupně podle
 * obráceného řetězce, přípona předchozího se sdílí). */

static int
coffllvm(void)
{
	return A->machine == 183;
}

static int
coffmach(void)
{
	return A->machine == 62 ? 0x8664 : 0xaa64;
}

/* typ relokace; *a: hodnota pole (x86-64: REL32 počítá od konce pole) */
static int
coffrel(AFix *f, int64_t *a)
{
	if (A->coffrel)
		return A->coffrel(f);
	if (f->kind == FAbs) {
		if (f->size == 8)
			return 1;               /* ADDR64 */
		if (f->size == 4)
			return 2;               /* ADDR32 */
		return 0;
	}
	if (f->size != 4)
		return 0;
	/* REL32: S + A - (P + 4), hodnota pole = A + 4 */
	*a += f->pcoff + 4;
	return 4;
}

static void
coffresolvefix(int secidx, uchar *buf, uint64_t pos, AFix *f)
{
	ASymbol *y;
	int64_t v, a;
	int type, llvm;

	llvm = coffllvm();
	y = f->e.sym >= 0 ? &asyms[f->e.sym] : 0;
	if (!y) {
		if (f->kind != FAbs)
			aerr("pc-relative reference to a constant");
		putval(buf + pos, f->e.add, f->size);
		return;
	}
	if (f->kind != FAbs && y->defined && y->sec == secidx) {
		v = (int64_t)y->val + f->e.add + f->pcoff - (int64_t)pos;
		if (f->enc) {
			A->fixenc(buf + pos, f, v);
			return;
		}
		if (f->size == 1 && (v < -128 || v > 127))
			aerr("jump out of range");
		putval(buf + pos, v, f->size);
		return;
	}
	a = f->e.add;
	type = coffrel(f, &a);
	if (!type)
		aerr("cannot relocate a reference to %s", y->name);
	GROW(rels, nrels + 1);
	rels[nrels].off = pos;
	if (y->defined && (!llvm || y->local)) {
		rels[nrels].sym = 0;
		rels[nrels].sec = y->sec;
		a += y->val;
	} else {
		rels[nrels].sym = f->e.sym;
		rels[nrels].sec = -1;
	}
	rels[nrels].type = type;
	rels[nrels].add = 0;
	if (f->enc && A->coffenc)
		A->coffenc(buf + pos, f, a);
	else
		putval(buf + pos, a, f->size);
	nrels++;
}

/* tabulka řetězců: jména delší než 8 bajtů a jejich posuny */
static char **cstr;
static uint64_t *cstroff;
static uint ncstr;
static uint64_t cstrsize;

static void
coffaddstr(char *name)
{
	uint n;

	if (strlen(name) <= 8)
		return;
	for (n=0; n<ncstr; n++)
		if (strcmp(cstr[n], name) == 0)
			return;
	GROW(cstr, ncstr + 1);
	GROW(cstroff, ncstr + 1);
	cstr[ncstr] = name;
	cstroff[ncstr] = 0;
	ncstr++;
}

/* pořadí LLVM: sestupně podle obráceného řetězce */
static int
cstrcmp(const void *pa, const void *pb)
{
	char *a, *b;
	size_t i, j;
	int ca, cb;

	a = *(char **)pa;
	b = *(char **)pb;
	i = strlen(a);
	j = strlen(b);
	while (i > 0 && j > 0) {
		ca = (uchar)a[--i];
		cb = (uchar)b[--j];
		if (ca != cb)
			return ca > cb ? -1 : 1;
	}
	if (i == j)
		return 0;
	return i > 0 ? -1 : 1;
}

static void
cstrlayout(Out *strs)
{
	uint n;
	char *prev;
	size_t lp, ln;

	if (coffllvm())
		qsort(cstr, ncstr, sizeof cstr[0], cstrcmp);
	prev = 0;
	lp = 0;
	cstrsize = 4;
	for (n=0; n<ncstr; n++) {
		ln = strlen(cstr[n]);
		if (coffllvm() && prev && lp >= ln && strcmp(prev + lp - ln, cstr[n]) == 0) {
			cstroff[n] = cstrsize - ln - 1;
			continue;
		}
		cstroff[n] = cstrsize;
		oput(strs, cstr[n], ln + 1);
		cstrsize += ln + 1;
		prev = cstr[n];
		lp = ln;
	}
}

static Out *cstrout;  /* GNU: řetězce se připojují při každém použití */

static uint64_t
cstrfind(char *name)
{
	uint n;
	uint64_t off;

	if (!coffllvm()) {
		off = cstrsize;
		oput(cstrout, name, strlen(name) + 1);
		cstrsize += strlen(name) + 1;
		return off;
	}
	for (n=0; n<ncstr; n++)
		if (strcmp(cstr[n], name) == 0)
			return cstroff[n];
	die("as: COFF string missing");
	return 0;
}

/* jméno do záznamu (8 bajtů): krátké přímo, dlouhé jako posun v tabulce
 * řetězců (symbol: nuly a posun; sekce: "/posun") */
static void
coffname(Out *o, char *name, int sec)
{
	char b[24];
	size_t n;

	memset(b, 0, sizeof b);
	n = strlen(name);
	if (n <= 8) {
		memcpy(b, name, n);
		oput(o, b, 8);
		return;
	}
	if (!sec) {
		onum(o, 0, 4);
		onum(o, cstrfind(name), 4);
		return;
	}
	sprintf(b, "/%d", (int)cstrfind(name));
	oput(o, b, 8);
}

static void
coffsym(Out *o, char *name, uint64_t value, int sec, int cls, int naux)
{
	coffname(o, name, 0);
	onum(o, value, 4);
	onum(o, sec, 2);
	onum(o, 0, 2);
	onum(o, cls, 1);
	onum(o, naux, 1);
}

static int
coffalign(uint64_t al)
{
	int k;

	for (k=0; ((uint64_t)1 << k) < al; k++)
		;
	return k + 1;
}

static uint32_t crctab[256];

/* CRC-32 (polynom 0xedb88320) od 0 bez závěrečné inverze (JamCRC LLVM) */
static uint32_t
coffcrc(uchar *p, uint64_t n)
{
	uint32_t c, k;
	uint64_t i;
	int j;

	if (!crctab[1])
		for (k=0; k<256; k++) {
			c = k;
			for (j=0; j<8; j++)
				c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
			crctab[k] = c;
		}
	c = 0;
	for (i=0; i<n; i++)
		c = crctab[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return c;
}

/* symbol do tabulky COFF (místní i globální, ne .L) */
static int
coffkeep(ASymbol *y)
{
	if (y->local)
		return 0;
	if (y->weak)
		aerr("weak symbols are not supported in COFF");
	if (y->common)
		aerr("common symbols are not supported in COFF");
	return 1;
}

static void
coffsymof(Out *syms, ASymbol *y, int llvm)
{
	if (y->isabs)
		coffsym(syms, y->name, y->absval, -1, 3, 0);
	else if (!y->defined)
		coffsym(syms, y->name, 0, 0, 2, 0);
	else if (y->global)
		coffsym(syms, y->name, y->val, asecs[y->sec].elfidx, 2, 0);
	else
		coffsym(syms, y->name, y->val, asecs[y->sec].elfidx,
			!llvm && strcmp(asecs[y->sec].name, ".text") == 0 ? 6 : 3, 0);
}

static void
coffrelocs(Out *o, Rel *r, uint cnt, int *secsym)
{
	uint k;

	for (k=0; k<cnt; k++) {
		onum(o, r[k].off, 4);
		if (r[k].sec >= 0)
			onum(o, secsym[r[k].sec], 4);
		else {
			if (elfsym[r[k].sym] < 0)
				die("as: relocation symbol without a table entry");
			onum(o, elfsym[r[k].sym], 4);
		}
		onum(o, r[k].type, 2);
	}
}

static void
writecoff(char *out)
{
	Out o, hdr, strs, syms;
	ASec *s;
	ASymbol *y;
	AItem *i;
	AFix fx;
	FILE *f;
	uchar **bufs;
	uint64_t *psize, *dataoff, *reloff, pos;
	Rel **secrels;
	uint *relcnt;
	uint n, m, k, nsyms, fl, total;
	int64_t disp, t;
	int resolved, *secsym, naux, al, llvm;
	char *fname;

	llvm = coffllvm();
	memset(&o, 0, sizeof o);
	memset(&hdr, 0, sizeof hdr);
	memset(&strs, 0, sizeof strs);
	memset(&syms, 0, sizeof syms);
	bufs = emalloc((nasecs + 1) * sizeof bufs[0]);
	psize = emalloc((nasecs + 1) * sizeof psize[0]);
	dataoff = emalloc((nasecs + 1) * sizeof dataoff[0]);
	reloff = emalloc((nasecs + 1) * sizeof reloff[0]);
	secrels = emalloc((nasecs + 1) * sizeof secrels[0]);
	relcnt = emalloc((nasecs + 1) * sizeof relcnt[0]);
	secsym = emalloc((nasecs + 1) * sizeof secsym[0]);
	elfsym = emalloc((nasyms + 1) * sizeof elfsym[0]);
	for (n=0; n<nasyms; n++)
		elfsym[n] = -1;
	ncstr = 0;

	/* obsah sekcí a relokace */
	for (n=0; n<nasecs; n++) {
		s = &asecs[n];
		psize[n] = llvm ? s->size : (s->size + s->align - 1) & -(uint64_t)s->align;
		nrels = 0;
		bufs[n] = 0;
		if (!(s->flags & SNobits)) {
			bufs[n] = emalloc(psize[n] + 1);
			memset(bufs[n], 0, psize[n] + 1);
			if (s->flags & SExec)
				memset(bufs[n] + s->size, 0x90, psize[n] - s->size);
			for (m=0; m<s->nitem; m++) {
				i = &s->item[m];
				aline = i->line;
				switch (i->kind) {
				case IData:
					if (i->size)
						memcpy(bufs[n] + i->off, i->data, i->size);
					for (k=0; k<i->nfix; k++)
						coffresolvefix(n, bufs[n], i->off + i->fix[k].off, &i->fix[k]);
					break;
				case IAlign:
					if (i->nops && i->size)
						A->nops(bufs[n] + i->off, i->size);
					break;
				case IJump:
					y = &asyms[i->target.sym];
					resolved = exprin(&i->target, n, &t);
					disp = resolved ? (int64_t)t - (int64_t)(i->off + i->size) : 0;
					A->jemit(i, bufs[n] + i->off, disp, resolved);
					if (!resolved) {
						memset(&fx, 0, sizeof fx);
						fx.size = 4;
						fx.kind = FBranch;
						fx.pcoff = -4;
						fx.enc = A->jenc ? A->jenc(i) : 0;
						fx.relsym = -1;
						fx.e = i->target;
						coffresolvefix(n, bufs[n], i->off + i->size - 4, &fx);
					}
					break;
				}
			}
		}
		secrels[n] = rels;
		relcnt[n] = nrels;
		rels = 0;
		nrels = 0;
	}

	/* tabulka řetězců: LLVM předem seřazená a bez opakování, GNU podle
	 * použití (záhlaví sekcí, .file, symboly) */
	fname = filename ? filename : llvm ? 0 : "fake";
	cstrout = &strs;
	cstrsize = 4;
	if (llvm) {
		for (n=0; n<nasecs; n++)
			coffaddstr(asecs[n].name);
		for (n=0; n<nasyms; n++)
			if (coffkeep(&asyms[n]))
				coffaddstr(asyms[n].name);
		cstrlayout(&strs);
	}

	/* záhlaví sekcí */
	for (n=0; n<nasecs; n++) {
		s = &asecs[n];
		coffname(&hdr, s->name, 1);
		onum(&hdr, 0, 4);
		onum(&hdr, 0, 4);
		onum(&hdr, psize[n], 4);
		onum(&hdr, 0, 4);  /* posun dat */
		onum(&hdr, 0, 4);  /* posun relokací */
		onum(&hdr, 0, 4);
		onum(&hdr, relcnt[n], 2);
		onum(&hdr, 0, 2);
		al = coffalign(s->align);
		if (s->flags & SExec)
			fl = 0x60000020;
		else if (s->flags & SNobits)
			fl = 0x40000080;
		else
			fl = 0x40000040;
		if (s->flags & SWrite)
			fl |= 0x80000000u;
		onum(&hdr, fl | ((uint)al << 20), 4);
	}

	/* symboly */
	nsyms = 0;
	if (!llvm) {
		/* jméno souboru v pomocném záznamu, delší než 18 bajtů v tabulce
		 * řetězců (nuly a posun) */
		coffsym(&syms, ".file", 0, -2, 103, 1);
		if (strlen(fname) > 18) {
			onum(&syms, 0, 4);
			onum(&syms, cstrfind(fname), 4);
			ozero(&syms, 10);
		} else {
			char aux[19];
			memset(aux, 0, sizeof aux);
			memcpy(aux, fname, strlen(fname));
			oput(&syms, aux, 18);
		}
		nsyms = 2;
		for (n=0; n<nasyms; n++) {
			y = &asyms[n];
			if (y->global || !y->defined || !coffkeep(y))
				continue;
			elfsym[n] = nsyms++;
			coffsymof(&syms, y, 0);
		}
	}
	for (n=0; n<nasecs; n++) {
		s = &asecs[n];
		secsym[n] = nsyms;
		nsyms += 2;
		coffsym(&syms, s->name, 0, n + 1, 3, 1);
		onum(&syms, s->size, 4);
		onum(&syms, relcnt[n], 2);
		onum(&syms, 0, 2);
		onum(&syms, llvm && bufs[n] ? coffcrc(bufs[n], s->size) : 0, 4);
		onum(&syms, llvm ? n + 1 : 0, 2);
		ozero(&syms, 4);
	}
	if (!llvm) {
		for (k=0; k<2; k++)
			for (n=0; n<nasyms; n++) {
				y = &asyms[n];
				if (!(y->global || !y->defined) || (uint)!y->defined != k || !coffkeep(y))
					continue;
				elfsym[n] = nsyms++;
				coffsymof(&syms, y, 0);
			}
	} else {
		for (n=0; n<nasyms; n++) {
			y = &asyms[n];
			if (!coffkeep(y))
				continue;
			elfsym[n] = nsyms++;
			coffsymof(&syms, y, 1);
		}
		if (fname) {
			/* .file: jméno v pomocných záznamech po 18 bajtech */
			char *aux;
			naux = (strlen(fname) + 17) / 18;
			coffsym(&syms, ".file", 0, -2, 103, naux);
			aux = emalloc(18 * naux + 1);
			memset(aux, 0, 18 * naux + 1);
			memcpy(aux, fname, strlen(fname));
			oput(&syms, aux, 18 * naux);
			free(aux);
			nsyms += 1 + naux;
		}
	}

	/* rozložení souboru: GNU obsah všech sekcí a pak relokace, LLVM za
	 * obsahem každé sekce její relokace (posun dat i u prázdné sekce) */
	pos = 20 + 40 * (uint64_t)nasecs;
	for (n=0; n<nasecs; n++) {
		dataoff[n] = 0;
		reloff[n] = 0;
		if (bufs[n] && (psize[n] || llvm)) {
			dataoff[n] = pos;
			pos += psize[n];
		}
		if (llvm && relcnt[n]) {
			reloff[n] = pos;
			pos += 10 * (uint64_t)relcnt[n];
		}
	}
	for (n=0; n<nasecs && !llvm; n++)
		if (relcnt[n]) {
			reloff[n] = pos;
			pos += 10 * (uint64_t)relcnt[n];
		}
	total = 0;
	for (n=0; n<nasecs; n++)
		total += relcnt[n];
	onum(&o, coffmach(), 2);
	onum(&o, nasecs, 2);
	onum(&o, 0, 4);
	onum(&o, pos, 4);
	onum(&o, nsyms, 4);
	onum(&o, 0, 2);
	/* GNU as: LINE_NUMS_STRIPPED, bez relokací i RELOCS_STRIPPED */
	onum(&o, llvm ? 0 : total ? 4 : 5, 2);
	for (n=0; n<nasecs; n++) {
		putval(hdr.b + 40 * n + 20, dataoff[n], 4);
		putval(hdr.b + 40 * n + 24, reloff[n], 4);
	}
	oput(&o, hdr.b, hdr.n);
	for (n=0; n<nasecs; n++) {
		if (dataoff[n])
			oput(&o, bufs[n], psize[n]);
		if (llvm)
			coffrelocs(&o, secrels[n], relcnt[n], secsym);
	}
	for (n=0; n<nasecs && !llvm; n++)
		coffrelocs(&o, secrels[n], relcnt[n], secsym);
	oput(&o, syms.b, syms.n);
	onum(&o, cstrsize, 4);
	oput(&o, strs.b, strs.n);
	f = fopen(out, "wb");
	if (!f)
		aerr("cannot create %s", out);
	for (pos=0; pos<o.n; pos++)
		fputc(o.b[pos], f);
	fclose(f);
}

int
assemble(char **paths, int npaths, char *out, AArch *arch)
{
	uint n;
	int rounds, k;

	A = arch;
	nasecs = 0;
	nasyms = 0;
	acursec = -1;
	cur = 0;
	nlocnum = 0;
	filename = 0;
	nmacros = 0;
	macrodepth = 0;
	newsec(".text", SAlloc | SExec, 1);
	if (A->textalign > 1)
		asecs[0].align = A->textalign;
	newsec(".data", SAlloc | SWrite, 1);
	newsec(".bss", SAlloc | SWrite | SNobits, 8);
	if (acoff)
		/* výchozí sekce: GNU as pro PE na 16, LLVM na 4 */
		for (n=0; n<3; n++)
			asecs[n].align = A->machine == 183 ? 4 : 16;
	usesec(0);
	/* více vstupů: jeden objekt, jako by šly za sebou (sekce, symboly
	 * a číselná návěští se sdílejí) */
	for (k=0; k<npaths; k++) {
		apath = paths[k];
		afile = k;
		readfile(apath);
		aparse();
		usesec(0);
	}
	afile = 0;
	aline = 0;
	for (n=0; n<nasyms; n++)
		if (!asyms[n].defined && asyms[n].local && !asyms[n].isabs)
			aerr("undefined local symbol %s", asyms[n].name);
	if (A->padcode)
		for (n=0; n<nasecs; n++)
			if ((asecs[n].flags & (SExec | SNobits)) == SExec && asecs[n].nitem) {
				usesec(n);
				if (A->padcode == 1)
					align(asecs[n].align, 0, 0);
				else
					align(4, 1, 0);
			}
	rounds = 0;
	layout(1);
	while (layout(0))
		if (++rounds > 1000)
			aerr("jump relaxation does not converge");
	if (acoff) {
		for (n=0; n<nasecs; n++)
			asecs[n].elfidx = n + 1;
		writecoff(out);
	} else
		writeobj(out);
	return 0;
}

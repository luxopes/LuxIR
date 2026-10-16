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
static AItem *cur;      /* otevřená datová položka aktuální sekce */
static int *locnum;     /* číselná návěští: počet definic každého čísla */
static int nlocnum;
static char *filename;  /* .file */
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

int
asymidx(char *name)
{
	uint n;
	ASymbol *s;

	for (n=0; n<nasyms; n++)
		if (strcmp(asyms[n].name, name) == 0)
			return n;
	GROW(asyms, nasyms + 1);
	s = &asyms[nasyms];
	memset(s, 0, sizeof *s);
	s->name = dupstr(name);
	s->sec = -1;
	s->sizesub = -1;
	s->local = name[0] == '.' && name[1] == 'L';
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
afix(uint off, int size, int kind, int64_t pcoff, AExpr *e)
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
	f->e = *e;
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

static int
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

/* Výraz: [-]člen {+|- člen}; člen je číslo, (číslo), symbol, číselné
 * návěští 1f/1b nebo '.'; nejvýše jeden symbol s kladným znaménkem
 * (přípona @plt, @gotpcrel … se zahodí; GOT se tu nepoužívá). */
int
aexpr(char **ps, AExpr *e)
{
	char *p, *q, *w, *at;
	int64_t v;
	int sign, any;

	e->sym = -1;
	e->add = 0;
	any = 0;
	p = *ps;
	for (;;) {
		aspace(&p);
		sign = 1;
		if (*p == '+' || *p == '-') {
			if (*p == '-')
				sign = -1;
			p++;
			aspace(&p);
		}
		if (*p == '(') {
			p++;
			aspace(&p);
			if (!anumber(&p, &v))
				aerr("number expected");
			aspace(&p);
			if (*p != ')')
				aerr("')' expected");
			p++;
			e->add += sign * v;
		} else if (isdigit((uchar)*p)) {
			q = p;
			anumber(&q, &v);
			if ((*q == 'f' || *q == 'b') && !identchar((uchar)q[1])) {
				if (e->sym >= 0 || sign < 0)
					aerr("invalid expression");
				e->sym = asymidx(numlabel((int)v, *q == 'f'));
				q++;
			} else
				e->add += sign * v;
			p = q;
		} else if (*p == '.' && !identchar((uchar)p[1])) {
			if (e->sym >= 0 || sign < 0)
				aerr("invalid expression");
			e->sym = dot();
			p++;
		} else if (identchar((uchar)*p) || *p == '"') {
			if (e->sym >= 0 || sign < 0)
				aerr("invalid expression");
			w = aword(&p);
			at = strchr(w, '@');
			if (at)
				*at = 0;
			e->sym = asymidx(w);
		} else
			break;
		any = 1;
		aspace(&p);
		if (*p != '+' && *p != '-')
			break;
	}
	*ps = p;
	return any;
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
	else if (strcmp(d, ".short") == 0 || strcmp(d, ".word") == 0 || strcmp(d, ".2byte") == 0 || strcmp(d, ".hword") == 0)
		dataexpr(p, 2);
	else if (strcmp(d, ".int") == 0 || strcmp(d, ".long") == 0 || strcmp(d, ".4byte") == 0)
		dataexpr(p, 4);
	else if (strcmp(d, ".quad") == 0 || strcmp(d, ".8byte") == 0)
		dataexpr(p, 8);
	else if (strcmp(d, ".ascii") == 0)
		dascii(p, 0);
	else if (strcmp(d, ".asciz") == 0 || strcmp(d, ".string") == 0)
		dascii(p, 1);
	else if (strcmp(d, ".zero") == 0 || strcmp(d, ".skip") == 0 || strcmp(d, ".space") == 0) {
		if (!anumber(&p, &v))
			aerr("count expected");
		aemit(v);
	} else if (strcmp(d, ".fill") == 0)
		dfill(p);
	else if (strcmp(d, ".balign") == 0 || strcmp(d, ".p2align") == 0 || strcmp(d, ".align") == 0)
		dalign(d, p);
	else if (strcmp(d, ".file") == 0) {
		if (!filename)
			filename = dupstr(aword(&p));
	} else if (strcmp(d, ".comm") == 0)
		dcomm(p);
	else if (strcmp(d, ".syntax") == 0 || strcmp(d, ".arm") == 0 || strcmp(d, ".fpu") == 0
	|| strcmp(d, ".arch") == 0 || strcmp(d, ".cpu") == 0 || strcmp(d, ".ident") == 0
	|| strcmp(d, ".subsections_via_symbols") == 0)
		;
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
		else if (*p == '#' || (*p == '/' && p[1] == '/')) {
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

static void
aparse(void)
{
	static char *line;
	static size_t cap;
	char *p, *e, *s, *t, *reptstart;
	size_t n;
	int instr, reptn, reptline, skip;

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
		/* více příkazů na řádku odděluje ';' mimo řetězce */
		s = line;
		instr = 0;
		for (t=line; *t; t++) {
			if (*t == '"')
				instr = !instr;
			else if (*t == ';' && !instr) {
				*t = 0;
				statement(s);
				s = t + 1;
			}
		}
		statement(s);
	}
}

/* ---- rozvržení ---- */

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
				if (!i->jlong && !initial) {
					size = A->jsize(i);
					y = &asyms[i->target.sym];
					if (!y->defined || y->sec != (int)n)
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
	int sym;      /* index v tabulce symbolů ELF */
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
	&& (jump || (!y->global && !y->weak))) {
		/* S + A - P, A = add + pcoff, P = pozice pole */
		v = (int64_t)y->val + f->e.add + f->pcoff - (int64_t)pos;
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
	if (y->defined && !y->global && !y->weak) {
		es = asecs[y->sec].symidx;
		a += y->val;
	} else
		es = elfsym[f->e.sym];
	GROW(rels, nrels + 1);
	rels[nrels].off = pos;
	rels[nrels].sym = es;
	rels[nrels].type = type;
	if (A->elf64)
		rels[nrels].add = a;
	else {
		/* REL: addend v poli */
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

	/* tabulka symbolů: 0, soubor, symboly sekcí, místní, globální */
	elfsym = emalloc((nasyms + 1) * sizeof elfsym[0]);
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
			if (y->local)
				continue;
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
			oput(&strtab, y->name, strlen(y->name) + 1);
		}
	}

	/* obsah sekcí a relokace */
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
				resolved = exprin(&i->target, n, &t);
				disp = resolved ? (int64_t)t - (int64_t)(i->off + i->size) : 0;
				A->jemit(i, buf + i->off, disp, resolved);
				if (!resolved) {
					memset(&fx, 0, sizeof fx);
					fx.size = 4;
					fx.kind = FBranch;
					fx.pcoff = -4;
					fx.e = i->target;
					resolvefix(n, buf, i->off + i->size - 4, &fx, 1);
				}
				break;
			}
		}
		oput(&o, buf, s->size);
		free(buf);
		relcnt[n] = nrels;
		if (nrels) {
			opad(&o, w);
			reloff[n] = o.n;
			for (k=0; k<nrels; k++)
				if (A->elf64) {
					onum(&o, rels[k].off, 8);
					onum(&o, ((uint64_t)(uint32_t)rels[k].sym << 32) | (uint32_t)rels[k].type, 8);
					onum(&o, rels[k].add, 8);
				} else {
					onum(&o, rels[k].off, 4);
					onum(&o, ((uint32_t)rels[k].sym << 8) | (uint32_t)rels[k].type, 4);
				}
			relsz[n] = o.n - reloff[n];
		}
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
		if (!y->local && y->defined)
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

int
assemble(char *path, char *out, AArch *arch)
{
	uint n;
	int rounds;

	A = arch;
	apath = path;
	nasecs = 0;
	nasyms = 0;
	acursec = -1;
	cur = 0;
	nlocnum = 0;
	filename = 0;
	newsec(".text", SAlloc | SExec, 1);
	newsec(".data", SAlloc | SWrite, 1);
	newsec(".bss", SAlloc | SWrite | SNobits, 8);
	usesec(0);
	readfile(path);
	aparse();
	aline = 0;
	for (n=0; n<nasyms; n++)
		if (!asyms[n].defined && asyms[n].local)
			aerr("undefined local symbol %s", asyms[n].name);
	rounds = 0;
	layout(1);
	while (layout(0))
		if (++rounds > 1000)
			aerr("jump relaxation does not converge");
	writeobj(out);
	return 0;
}

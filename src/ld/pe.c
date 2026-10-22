#include "ld.h"

/* Vestavěný linker pro Windows (luxir -l -t amd64_win|arm64_win):
 * konzolový program PE32+ z objektů COFF (výstup luxir -a nebo GNU as)
 * bez MinGW a LLVM.
 *
 * Dělá to, co LSL žádalo po ld (MinGW) a lld: -e _start --subsystem
 * console --dynamicbase --high-entropy-va --nxcompat --stack 8 MiB
 * --gc-sections -s. Vstupní sekce se podle jména před '$' (a příznaků)
 * slévají do .text, .rdata, .data a .bss; nepoužité sekce (bez cesty
 * relokací od vstupního bodu) se vypustí. Importy popisují soubory .def
 * mezi vstupy (LIBRARY dll, EXPORTS jména funkcí): volání nedefinované
 * funkce z .def jde přes thunk v .text (x86-64 jmp *IAT, arm64 adrp+ldr+br)
 * a __imp_jméno je přímo položka IAT; tabulky importů jsou v .idata.
 * Absolutní adresy (ADDR64) dostanou bázové relokace v .reloc, takže
 * obraz smí loader posunout (ASLR); 32bitové absolutní adresy jsou chyba
 * (báze nad 4 GiB). Tabulka symbolů se nezapisuje, časové razítko je 0:
 * výstup je deterministický. */

typedef struct PRel PRel;
typedef struct PSec PSec;
typedef struct PSym PSym;
typedef struct PObj PObj;
typedef struct PGlob PGlob;
typedef struct PImp PImp;

struct PRel {
	uint32_t off;
	uint32_t sym;
	int type;
};

struct PSec {
	char *name;
	uint32_t ch;
	uint32_t size;
	uchar *data;       /* 0 u neinicializovaných dat */
	PRel *rel;
	uint nrel;
	uint align;
	int out;           /* výstupní sekce nebo -1 */
	int keep;
	uint32_t rva;
};

struct PSym {
	char *name;
	uint32_t value;
	int sec;           /* 1.., 0 nedefinovaný, -1 absolutní, -2 ladicí */
	int cls;
	int glob;          /* index v pegs nebo -1 */
};

struct PObj {
	char *path;
	uchar *data;
	PSec *sec;
	uint nsec;
	PSym *sym;
	uint nsym;
};

struct PGlob {
	char *name;
	int obj;           /* definice: objekt a symbol, -1 nedefinovaný */
	int sym;
	int imp;           /* import z .def nebo -1 */
};

struct PImp {
	char *name;
	int dll;
	int used;          /* volání přes thunk (1), přímo IAT (__imp_, 2) */
	uint32_t thunk;    /* RVA thunku */
	uint32_t iat;      /* RVA položky IAT */
	uint32_t hint;     /* RVA hint/name */
};

/* výstupní sekce */
enum { PText, PRdata, PData, PBss, PIdata, PReloc, NPOut };

static char *pename[NPOut] = {".text", ".rdata", ".data", ".bss", ".idata", ".reloc"};
static uint32_t pech[NPOut] = {
	0x60000020, /* kód, čtení, spouštění */
	0x40000040, /* data jen pro čtení */
	0xc0000040, /* data */
	0xc0000080, /* neinicializovaná data */
	0xc0000040, /* importy (loader zapisuje IAT) */
	0x42000040, /* bázové relokace, zahoditelné */
};

static PObj *peobjs;
static uint penobjs;
static PGlob *pegs;
static uint pengs;
static uint *peghash;      /* index+1 do pegs */
static uint pehashcap;
static char **pedlls;
static uint pendlls;
static PImp *peimps;
static uint penimps;
static int pemach;          /* 0x8664 nebo 0xaa64 */
static char *peentry;
static uint64_t pebase = 0x140000000;
static uint32_t pesize[NPOut], perva[NPOut], peoff[NPOut], peraw[NPOut];
static uchar *pedata[NPOut];
static uint32_t *perelocs;  /* RVA polí ADDR64 */
static uint penrelocs, perelcap;
static uint32_t pethunksz;
static uint pendll;         /* použité DLL */
static uint32_t peilt, peiat, peiatsz, pedllnames;

static void
peerr(char *fmt, ...)
{
	va_list ap;

	fputs("luxir: link: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void *
pegrow(void *p, uint n, size_t esz)
{
	p = realloc(p, (n + 1) * esz);
	if (!p)
		die("out of memory");
	return p;
}

static uint64_t
perd(uchar *p, int n)
{
	uint64_t v;
	int i;

	v = 0;
	for (i=n-1; i>=0; i--)
		v = (v << 8) | p[i];
	return v;
}

static void
pewr(uchar *p, uint64_t v, int n)
{
	int i;

	for (i=0; i<n; i++)
		p[i] = (v >> (8 * i)) & 0xff;
}

static uint32_t
peal(uint32_t v, uint32_t a)
{
	return (v + a - 1) & -a;
}

static uchar *
peread(char *path, uint64_t *len)
{
	FILE *f;
	uchar *d;
	long cap, n, got;

	f = fopen(path, "rb");
	if (!f)
		peerr("cannot open %s", path);
	cap = 1 << 16;
	d = emalloc(cap);
	n = 0;
	for (;;) {
		if (n + 4096 > cap) {
			cap *= 2;
			d = realloc(d, cap);
			if (!d)
				die("out of memory");
		}
		got = fread(d + n, 1, cap - n - 1, f);
		if (got == 0)
			break;
		n += got;
	}
	fclose(f);
	d[n] = 0;
	*len = n;
	return d;
}

/* ---- globální jména ---- */

static uint
pehashs(char *s)
{
	uint h;

	h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (uchar)*s) * 16777619u;
	return h;
}

static int
pefind(char *name, int add)
{
	uint i, k, cap;
	uint *old;

	if (!pehashcap || (add && 2 * (pengs + 1) > pehashcap)) {
		old = peghash;
		cap = pehashcap;
		pehashcap = cap ? 2 * cap : 1024;
		peghash = emalloc(pehashcap * sizeof peghash[0]);
		memset(peghash, 0, pehashcap * sizeof peghash[0]);
		for (k=0; k<pengs; k++) {
			i = pehashs(pegs[k].name) & (pehashcap - 1);
			while (peghash[i])
				i = (i + 1) & (pehashcap - 1);
			peghash[i] = k + 1;
		}
		free(old);
	}
	i = pehashs(name) & (pehashcap - 1);
	while (peghash[i]) {
		if (strcmp(pegs[peghash[i] - 1].name, name) == 0)
			return peghash[i] - 1;
		i = (i + 1) & (pehashcap - 1);
	}
	if (!add)
		return -1;
	pegs = pegrow(pegs, pengs + 1, sizeof pegs[0]);
	pegs[pengs].name = name;
	pegs[pengs].obj = -1;
	pegs[pengs].sym = -1;
	pegs[pengs].imp = -1;
	peghash[i] = pengs + 1;
	return pengs++;
}

/* ---- vstupy ---- */

static char *
pename8(PObj *o, uchar *p, uchar *strtab, uint64_t strsz)
{
	char *s;
	uint64_t off;

	if (perd(p, 4) == 0) {
		off = perd(p + 4, 4);
		if (off >= strsz)
			peerr("%s: bad symbol name", o->path);
		return (char *)strtab + off;
	}
	s = emalloc(9);
	memcpy(s, p, 8);
	s[8] = 0;
	return s;
}

static void
pereadobj(char *path, uchar *d, uint64_t len)
{
	PObj *o;
	PSec *s;
	PSym *y;
	uchar *h, *strtab, *e;
	uint64_t symptr, nsym, strsz, ptr, rptr, n, k, off;
	uint nsec, optsz, naux;
	char *name;

	if (len < 20 || perd(d, 2) != (uint)pemach)
		peerr("%s: not a COFF object for the target", path);
	penobjs++;
	peobjs = pegrow(peobjs, penobjs, sizeof peobjs[0]);
	o = &peobjs[penobjs - 1];
	memset(o, 0, sizeof *o);
	o->path = path;
	o->data = d;
	nsec = perd(d + 2, 2);
	symptr = perd(d + 8, 4);
	nsym = perd(d + 12, 4);
	optsz = perd(d + 16, 2);
	if (symptr + nsym * 18 + 4 > len || 20 + optsz + 40 * (uint64_t)nsec > len)
		peerr("%s: truncated object", path);
	strtab = d + symptr + nsym * 18;
	strsz = perd(strtab, 4);
	if (strsz < 4 || symptr + nsym * 18 + strsz > len)
		peerr("%s: bad string table", path);
	o->nsec = nsec;
	o->sec = emalloc((nsec + 1) * sizeof o->sec[0]);
	memset(o->sec, 0, (nsec + 1) * sizeof o->sec[0]);
	for (n=0; n<nsec; n++) {
		h = d + 20 + optsz + 40 * n;
		s = &o->sec[n];
		if (h[0] == '/') {
			off = 0;
			for (k=1; k<8 && h[k] >= '0' && h[k] <= '9'; k++)
				off = 10 * off + (h[k] - '0');
			if (off >= strsz)
				peerr("%s: bad section name", path);
			s->name = (char *)strtab + off;
		} else {
			name = emalloc(9);
			memcpy(name, h, 8);
			name[8] = 0;
			s->name = name;
		}
		s->size = perd(h + 16, 4);
		ptr = perd(h + 20, 4);
		rptr = perd(h + 24, 4);
		s->nrel = perd(h + 32, 2);
		s->ch = perd(h + 36, 4);
		s->align = (s->ch >> 20) & 15 ? 1u << (((s->ch >> 20) & 15) - 1) : 16;
		s->out = -1;
		if (s->ch & 0x80)
			s->data = 0;
		else {
			if (ptr + s->size > len)
				peerr("%s: section %s out of file", path, s->name);
			s->data = s->size ? d + ptr : 0;
		}
		if (s->nrel) {
			if (rptr + 10 * (uint64_t)s->nrel > len)
				peerr("%s: relocations out of file", path);
			s->rel = emalloc(s->nrel * sizeof s->rel[0]);
			for (k=0; k<s->nrel; k++) {
				e = d + rptr + 10 * k;
				s->rel[k].off = perd(e, 4);
				s->rel[k].sym = perd(e + 4, 4);
				s->rel[k].type = perd(e + 8, 2);
				if (s->rel[k].off + 4 > s->size || s->rel[k].sym >= nsym)
					peerr("%s: bad relocation in %s", path, s->name);
			}
		}
	}
	o->nsym = nsym;
	o->sym = emalloc((nsym + 1) * sizeof o->sym[0]);
	memset(o->sym, 0, (nsym + 1) * sizeof o->sym[0]);
	for (n=0; n<nsym; n+=1+naux) {
		e = d + symptr + 18 * n;
		y = &o->sym[n];
		y->name = pename8(o, e, strtab, strsz);
		y->value = perd(e + 8, 4);
		y->sec = (int16_t)perd(e + 12, 2);
		y->cls = e[16];
		y->glob = -1;
		naux = e[17];
		if (y->sec > (int)nsec)
			peerr("%s: bad symbol section", path);
		if (y->cls == 105)
			peerr("%s: weak external %s is not supported", path, y->name);
		if (y->cls == 2) {
			int g = pefind(y->name, 1);
			y->glob = g;
			if (y->sec == 0 && y->value)
				peerr("%s: common symbol %s is not supported", path, y->name);
			if (y->sec != 0) {
				if (pegs[g].obj >= 0)
					peerr("duplicate symbol %s (%s and %s)", y->name, peobjs[pegs[g].obj].path, path);
				pegs[g].obj = penobjs - 1;
				pegs[g].sym = n;
			}
		}
	}
}

/* .def: LIBRARY dll, EXPORTS a jména (komentáře ';', zbytek řádku za
 * jménem se ignoruje) */
static void
pereaddef(char *path, uchar *d)
{
	char *p, *w, *line;
	int dll, exports;
	size_t n;

	dll = -1;
	exports = 0;
	p = (char *)d;
	while (*p) {
		line = p;
		while (*p && *p != '\n')
			p++;
		if (*p)
			*p++ = 0;
		w = strchr(line, ';');
		if (w)
			*w = 0;
		while (*line == ' ' || *line == '\t' || *line == '\r')
			line++;
		n = strlen(line);
		while (n && (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == '\r'))
			line[--n] = 0;
		if (!n)
			continue;
		if (strncmp(line, "LIBRARY", 7) == 0 && (line[7] == ' ' || line[7] == '\t')) {
			w = line + 7;
			while (*w == ' ' || *w == '\t')
				w++;
			if (*w == '"') {
				w++;
				n = strlen(w);
				if (n && w[n - 1] == '"')
					w[n - 1] = 0;
			}
			pedlls = pegrow(pedlls, pendlls + 1, sizeof pedlls[0]);
			pedlls[pendlls] = w;
			dll = pendlls++;
			exports = 0;
			continue;
		}
		if (strcmp(line, "EXPORTS") == 0) {
			if (dll < 0)
				peerr("%s: EXPORTS without LIBRARY", path);
			exports = 1;
			continue;
		}
		if (!exports)
			peerr("%s: unexpected line %s", path, line);
		w = line;
		while (*w && *w != ' ' && *w != '\t' && *w != '@' && *w != '=')
			w++;
		*w = 0;
		peimps = pegrow(peimps, penimps + 1, sizeof peimps[0]);
		peimps[penimps].name = line;
		peimps[penimps].dll = dll;
		peimps[penimps].used = 0;
		penimps++;
	}
}

static void
peimports(void)
{
	uint n;
	int g;
	char *nm;

	for (n=0; n<penimps; n++) {
		g = pefind(peimps[n].name, 0);
		if (g >= 0 && pegs[g].obj < 0 && pegs[g].imp < 0)
			pegs[g].imp = n;
		nm = emalloc(strlen(peimps[n].name) + 7);
		sprintf(nm, "__imp_%s", peimps[n].name);
		g = pefind(nm, 0);
		if (g >= 0 && pegs[g].obj < 0 && pegs[g].imp < 0)
			pegs[g].imp = n;
	}
}

/* ---- výběr sekcí a gc ---- */

static int
peoutsec(PSec *s)
{
	char *n;

	n = s->name;
	if (s->ch & 0x800)          /* LNK_REMOVE */
		return -1;
	if (strcmp(n, ".drectve") == 0 || strncmp(n, ".debug", 6) == 0)
		return -1;
	if (s->ch & 0x20)
		return PText;
	if (s->ch & 0x80)
		return PBss;
	if (!(s->ch & 0x40))
		return -1;
	if (!(s->ch & 0x80000000u))
		return PRdata;
	return PData;
}

static void
pemark(PObj *o, PSec *s)
{
	uint k;
	PSym *y;
	PGlob *g;
	PObj *to;

	if (s->keep)
		return;
	s->keep = 1;
	for (k=0; k<s->nrel; k++) {
		y = &o->sym[s->rel[k].sym];
		to = o;
		if (y->glob >= 0) {
			g = &pegs[y->glob];
			if (g->obj < 0) {
				if (g->imp < 0)
					peerr("undefined symbol %s (%s)", y->name, o->path);
				if (strncmp(g->name, "__imp_", 6) == 0)
					peimps[g->imp].used |= 2;
				else
					peimps[g->imp].used |= 1;
				continue;
			}
			to = &peobjs[g->obj];
			y = &to->sym[g->sym];
		}
		if (y->sec > 0 && to->sec[y->sec - 1].out >= 0)
			pemark(to, &to->sec[y->sec - 1]);
		else if (y->sec == 0)
			peerr("undefined symbol %s (%s)", y->name, o->path);
	}
}

static void
pegc(void)
{
	uint n, m;
	int g;
	PGlob *e;
	PObj *o;

	for (n=0; n<penobjs; n++)
		for (m=0; m<peobjs[n].nsec; m++)
			peobjs[n].sec[m].out = peoutsec(&peobjs[n].sec[m]);
	g = pefind(peentry, 0);
	if (g < 0 || pegs[g].obj < 0)
		peerr("entry symbol %s is not defined", peentry);
	e = &pegs[g];
	o = &peobjs[e->obj];
	if (o->sym[e->sym].sec <= 0)
		peerr("entry symbol %s has no section", peentry);
	pemark(o, &o->sec[o->sym[e->sym].sec - 1]);
}

/* ---- rozvržení ---- */

static uint32_t
pethunk(void)
{
	return pemach == 0x8664 ? 8 : 12;
}

static void
pelayout(void)
{
	uint n, m, k, d, ndll, nslots, slot;
	uint32_t rva, sz, names;
	PSec *s;

	/* sekce objektů: velikosti výstupních sekcí */
	for (k=0; k<PIdata; k++) {
		sz = 0;
		for (n=0; n<penobjs; n++)
			for (m=0; m<peobjs[n].nsec; m++) {
				s = &peobjs[n].sec[m];
				if (!s->keep || s->out != (int)k)
					continue;
				sz = peal(sz, s->align);
				s->rva = sz;   /* zatím posun ve výstupní sekci */
				sz += s->size;
			}
		if (k == PText) {
			/* thunky importů na konci kódu */
			pethunksz = 0;
			sz = peal(sz, 16);
			for (n=0; n<penimps; n++)
				if (peimps[n].used & 1) {
					peimps[n].thunk = sz;
					sz += pethunk();
					pethunksz += pethunk();
				}
		}
		pesize[k] = sz;
	}
	/* .idata: deskriptory, ILT, IAT (po DLL, každá končí nulovou
	 * položkou), hint/name funkcí, jména DLL */
	ndll = 0;
	nslots = 0;
	for (d=0; d<pendlls; d++) {
		int any = 0;
		for (n=0; n<penimps; n++)
			if (peimps[n].used && peimps[n].dll == (int)d) {
				nslots++;
				any = 1;
			}
		if (any) {
			nslots++;
			ndll++;
		}
	}
	pendll = ndll;
	sz = 0;
	if (ndll) {
		sz = 20 * (ndll + 1) + 2 * 8 * nslots;
		for (n=0; n<penimps; n++)
			if (peimps[n].used)
				sz += peal(2 + strlen(peimps[n].name) + 1, 2);
		for (d=0; d<pendlls; d++)
			sz += strlen(pedlls[d]) + 1;
	}
	pesize[PIdata] = sz;
	/* RVA a posuny v souboru */
	rva = 0x1000;
	for (k=0; k<PReloc; k++) {
		perva[k] = 0;
		if (!pesize[k])
			continue;
		perva[k] = rva;
		rva = peal(rva + pesize[k], 0x1000);
	}
	perva[PReloc] = rva;
	for (n=0; n<penobjs; n++)
		for (m=0; m<peobjs[n].nsec; m++) {
			s = &peobjs[n].sec[m];
			if (s->keep && s->out >= 0)
				s->rva += perva[s->out];
		}
	for (n=0; n<penimps; n++)
		if (peimps[n].used & 1)
			peimps[n].thunk += perva[PText];
	/* IAT, ILT a jména: RVA položek */
	if (ndll) {
		peilt = perva[PIdata] + 20 * (ndll + 1);
		peiat = peilt + 8 * nslots;
		peiatsz = 8 * nslots;
		names = peiat + 8 * nslots;
		slot = 0;
		for (d=0; d<pendlls; d++) {
			int any = 0;
			for (n=0; n<penimps; n++)
				if (peimps[n].used && peimps[n].dll == (int)d) {
					peimps[n].iat = peiat + 8 * slot;
					peimps[n].hint = names;
					names += peal(2 + strlen(peimps[n].name) + 1, 2);
					slot++;
					any = 1;
				}
			slot += any;
		}
		pedllnames = names;
	}
}

/* ---- relokace ---- */

static uint32_t
pesymrva(PObj *o, uint si, int *isimp)
{
	PSym *y;
	PGlob *g;
	PObj *to;

	*isimp = 0;
	y = &o->sym[si];
	to = o;
	if (y->glob >= 0) {
		g = &pegs[y->glob];
		if (g->obj < 0) {
			*isimp = 1;
			if (strncmp(g->name, "__imp_", 6) == 0)
				return peimps[g->imp].iat;
			return peimps[g->imp].thunk;
		}
		to = &peobjs[g->obj];
		y = &to->sym[g->sym];
	}
	if (y->sec == -1)
		peerr("%s: absolute symbol %s is not supported", o->path, y->name);
	if (y->sec <= 0)
		peerr("%s: undefined symbol %s", o->path, y->name);
	if (!to->sec[y->sec - 1].keep)
		peerr("%s: relocation against discarded section %s", o->path, to->sec[y->sec - 1].name);
	return to->sec[y->sec - 1].rva + y->value;
}

static void
pebasereloc(uint32_t rva)
{
	if (penrelocs == perelcap) {
		perelcap = perelcap ? 2 * perelcap : 256;
		perelocs = realloc(perelocs, perelcap * sizeof perelocs[0]);
		if (!perelocs)
			die("out of memory");
	}
	perelocs[penrelocs++] = rva;
}

static void
perelocate(void)
{
	uint n, m, k;
	PObj *o;
	PSec *s;
	PRel *r;
	uchar *f;
	uint32_t S, P, ins;
	int64_t A, v;
	int imp;

	for (n=0; n<penobjs; n++) {
		o = &peobjs[n];
		for (m=0; m<o->nsec; m++) {
			s = &o->sec[m];
			if (!s->keep || s->out < 0 || !s->data)
				continue;
			f = pedata[s->out] + (s->rva - perva[s->out]);
			memcpy(f, s->data, s->size);
			for (k=0; k<s->nrel; k++) {
				r = &s->rel[k];
				S = pesymrva(o, r->sym, &imp);
				P = s->rva + r->off;
				f = pedata[s->out] + (P - perva[s->out]);
				if (pemach == 0x8664)
					switch (r->type) {
					case 1: /* ADDR64 */
						if (r->off + 8 > s->size)
							peerr("%s: bad ADDR64 relocation", o->path);
						pewr(f, pebase + S + perd(f, 8), 8);
						pebasereloc(P);
						break;
					case 3: /* ADDR32NB */
						pewr(f, S + (int32_t)perd(f, 4), 4);
						break;
					case 4: case 5: case 6: case 7: case 8: case 9: /* REL32, REL32_1..5 */
						v = (int64_t)S + (int32_t)perd(f, 4) - ((int64_t)P + 4 + (r->type - 4));
						if (v < INT32_MIN || v > INT32_MAX)
							peerr("%s: relocation out of range", o->path);
						pewr(f, v, 4);
						break;
					case 2:
						peerr("%s: 32-bit absolute address in a relocatable image (%s)", o->path, o->sym[r->sym].name);
						break;
					default:
						peerr("%s: unsupported relocation type %d", o->path, r->type);
					}
				else {
					ins = perd(f, 4);
					switch (r->type) {
					case 0xe: /* ADDR64 */
						if (r->off + 8 > s->size)
							peerr("%s: bad ADDR64 relocation", o->path);
						pewr(f, pebase + S + perd(f, 8), 8);
						pebasereloc(P);
						break;
					case 2: /* ADDR32NB */
						pewr(f, S + (int32_t)ins, 4);
						break;
					case 0x11: /* REL32 */
						pewr(f, (int64_t)S + (int32_t)ins - (int64_t)P, 4);
						break;
					case 3: /* BRANCH26: S + A - P, A v imm26 */
						A = (int64_t)(((int32_t)(ins << 6)) >> 6) << 2;
						v = (int64_t)S + A - P;
						if ((v & 3) || v < -(1 << 27) || v >= (1 << 27))
							peerr("%s: branch out of range", o->path);
						pewr(f, (ins & 0xfc000000) | ((v >> 2) & 0x3ffffff), 4);
						break;
					case 0xf: /* BRANCH19 */
						A = (int64_t)(((int32_t)(ins << 8)) >> 13) << 2;
						v = (int64_t)S + A - P;
						if ((v & 3) || v < -(1 << 20) || v >= (1 << 20))
							peerr("%s: branch out of range", o->path);
						pewr(f, (ins & 0xff00001f) | (((v >> 2) & 0x7ffff) << 5), 4);
						break;
					case 0x10: /* BRANCH14 */
						A = (int64_t)(((int32_t)(ins << 13)) >> 18) << 2;
						v = (int64_t)S + A - P;
						if ((v & 3) || v < -(1 << 15) || v >= (1 << 15))
							peerr("%s: branch out of range", o->path);
						pewr(f, (ins & 0xfff8001f) | (((v >> 2) & 0x3fff) << 5), 4);
						break;
					case 4: /* PAGEBASE_REL21: adrp, A v imm21 (bajty) */
					case 5: /* REL21: adr */
						A = ((ins >> 29) & 3) | (((ins >> 5) & 0x7ffff) << 2);
						A = (A << 43) >> 43;
						if (r->type == 4)
							v = (((int64_t)S + A) >> 12) - ((int64_t)P >> 12);
						else
							v = (int64_t)S + A - P;
						if (v < -(1 << 20) || v >= (1 << 20))
							peerr("%s: adrp out of range", o->path);
						pewr(f, (ins & 0x9f00001f) | ((v & 3) << 29) | (((v >> 2) & 0x7ffff) << 5), 4);
						break;
					case 6: /* PAGEOFFSET_12A: add, A v imm12 */
						v = (S + ((ins >> 10) & 0xfff)) & 0xfff;
						pewr(f, (ins & ~(0xfffu << 10)) | (v << 10), 4);
						break;
					case 7: { /* PAGEOFFSET_12L: ldr/str, imm12 škálované velikostí */
						int sh = ins >> 30;
						if ((ins & 0x04800000) == 0x04800000)
							sh = 4;  /* 128bitový vektor */
						v = (((ins >> 10) & 0xfff) << sh) + S;
						v &= 0xfff;
						if (v & ((1 << sh) - 1))
							peerr("%s: misaligned page offset", o->path);
						pewr(f, (ins & ~(0xfffu << 10)) | ((uint32_t)(v >> sh) << 10), 4);
						break;
					}
					default:
						peerr("%s: unsupported relocation type %d", o->path, r->type);
					}
				}
				(void)imp;
			}
		}
	}
}

static void
pethunks(void)
{
	uint n;
	uchar *f;
	int64_t v;
	uint32_t P, T;

	for (n=0; n<penimps; n++) {
		if (!(peimps[n].used & 1))
			continue;
		P = peimps[n].thunk;
		T = peimps[n].iat;
		f = pedata[PText] + (P - perva[PText]);
		if (pemach == 0x8664) {
			/* jmp *IAT(%rip); nop; nop */
			f[0] = 0xff;
			f[1] = 0x25;
			pewr(f + 2, (int64_t)T - (P + 6), 4);
			f[6] = 0x90;
			f[7] = 0x90;
		} else {
			/* adrp x16, IAT; ldr x16, [x16, :lo12:IAT]; br x16 */
			v = ((int64_t)T >> 12) - ((int64_t)P >> 12);
			pewr(f, 0x90000010 | ((v & 3) << 29) | (((v >> 2) & 0x7ffff) << 5), 4);
			pewr(f + 4, 0xf9400210 | (((T & 0xfff) >> 3) << 10), 4);
			pewr(f + 8, 0xd61f0200, 4);
		}
	}
}

static void
peidata(void)
{
	uint n, d, nd;
	uchar *b;
	uint32_t base, first, name;

	if (!pesize[PIdata])
		return;
	b = pedata[PIdata];
	base = perva[PIdata];
	name = pedllnames;
	nd = 0;
	for (d=0; d<pendlls; d++) {
		first = 0;
		for (n=0; n<penimps; n++)
			if (peimps[n].used && peimps[n].dll == (int)d) {
				if (!first)
					first = peimps[n].iat;
				/* hint 0 a jméno; ILT a IAT ukazují na ně */
				memcpy(b + (peimps[n].hint - base) + 2, peimps[n].name, strlen(peimps[n].name));
				pewr(b + (peimps[n].iat - base), peimps[n].hint, 8);
				pewr(b + (peimps[n].iat - peiatsz - base), peimps[n].hint, 8);
			}
		if (!first)
			continue;
		memcpy(b + (name - base), pedlls[d], strlen(pedlls[d]));
		/* deskriptor: ILT, čas, forwarder, jméno, IAT */
		pewr(b + 20 * nd, first - peiatsz, 4);
		pewr(b + 20 * nd + 12, name, 4);
		pewr(b + 20 * nd + 16, first, 4);
		name += strlen(pedlls[d]) + 1;
		nd++;
	}
}

/* .reloc: bloky po stránkách, položky DIR64 (typ 10) */
static void
pereloc(void)
{
	uint n, k, cnt;
	uint32_t page, *v;
	uchar *b;
	uint64_t sz;

	pesize[PReloc] = 0;
	if (!penrelocs)
		return;
	/* seřadit (vkládáním po stránkách stačí: relokace jsou skoro seřazené) */
	v = perelocs;
	for (n=1; n<penrelocs; n++) {
		uint32_t x = v[n];
		k = n;
		while (k > 0 && v[k - 1] > x) {
			v[k] = v[k - 1];
			k--;
		}
		v[k] = x;
	}
	sz = 0;
	for (n=0; n<penrelocs; n=k) {
		page = v[n] & ~0xfffu;
		for (k=n; k<penrelocs && (v[k] & ~0xfffu) == page; k++)
			;
		cnt = k - n;
		sz += 8 + 2 * (cnt + (cnt & 1));
	}
	pedata[PReloc] = emalloc(sz + 1);
	memset(pedata[PReloc], 0, sz + 1);
	b = pedata[PReloc];
	for (n=0; n<penrelocs; n=k) {
		page = v[n] & ~0xfffu;
		for (k=n; k<penrelocs && (v[k] & ~0xfffu) == page; k++)
			;
		cnt = k - n;
		pewr(b, page, 4);
		pewr(b + 4, 8 + 2 * (cnt + (cnt & 1)), 4);
		b += 8;
		for (; n<k; n++) {
			pewr(b, (10 << 12) | (v[n] & 0xfff), 2);
			b += 2;
		}
		if (cnt & 1)
			b += 2;
	}
	pesize[PReloc] = sz;
}

/* ---- výstup ---- */

static uchar pestub[64] = {
	0x0e, 0x1f, 0xba, 0x0e, 0x00, 0xb4, 0x09, 0xcd, 0x21, 0xb8, 0x01, 0x4c, 0xcd, 0x21, 'T', 'h',
	'i', 's', ' ', 'p', 'r', 'o', 'g', 'r', 'a', 'm', ' ', 'c', 'a', 'n', 'n', 'o',
	't', ' ', 'b', 'e', ' ', 'r', 'u', 'n', ' ', 'i', 'n', ' ', 'D', 'O', 'S', ' ',
	'm', 'o', 'd', 'e', '.', 0x0d, 0x0d, 0x0a, '$', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void
pewrite(char *out)
{
	uchar *buf, *h, *p;
	uint64_t total;
	uint32_t hsize, off, entry, code, idata, udata, imagesz;
	uint nsec, k;
	int g;
	FILE *f;
	PGlob *e;
	PObj *o;

	nsec = 0;
	for (k=0; k<NPOut; k++)
		if (pesize[k])
			nsec++;
	hsize = peal(0x80 + 4 + 20 + 240 + 40 * nsec, 0x200);
	off = hsize;
	code = idata = udata = 0;
	imagesz = 0x1000;
	for (k=0; k<NPOut; k++) {
		peoff[k] = 0;
		peraw[k] = 0;
		if (!pesize[k])
			continue;
		if (k != PBss) {
			peraw[k] = peal(pesize[k], 0x200);
			peoff[k] = off;
			off += peraw[k];
		}
		if (k == PText)
			code += peraw[k];
		else if (k == PBss)
			udata += peal(pesize[k], 0x200);
		else
			idata += peraw[k];
		imagesz = peal(perva[k] + pesize[k], 0x1000);
	}
	total = off;
	buf = emalloc(total + 1);
	memset(buf, 0, total + 1);
	for (k=0; k<NPOut; k++)
		if (peraw[k] && pedata[k])
			memcpy(buf + peoff[k], pedata[k], pesize[k]);
	g = pefind(peentry, 0);
	e = &pegs[g];
	o = &peobjs[e->obj];
	entry = o->sec[o->sym[e->sym].sec - 1].rva + o->sym[e->sym].value;
	/* DOS hlavička a stub */
	buf[0] = 'M';
	buf[1] = 'Z';
	pewr(buf + 2, 0x90, 2);
	pewr(buf + 4, 3, 2);
	pewr(buf + 8, 4, 2);
	pewr(buf + 12, 0xffff, 2);
	pewr(buf + 16, 0xb8, 2);
	pewr(buf + 24, 0x40, 2);
	pewr(buf + 60, 0x80, 4);
	memcpy(buf + 64, pestub, 64);
	h = buf + 0x80;
	h[0] = 'P';
	h[1] = 'E';
	/* hlavička COFF */
	p = h + 4;
	pewr(p, pemach, 2);
	pewr(p + 2, nsec, 2);
	pewr(p + 16, 240, 2);
	/* EXECUTABLE_IMAGE, LARGE_ADDRESS_AWARE */
	pewr(p + 18, 0x22, 2);
	/* volitelná hlavička PE32+ */
	p = h + 24;
	pewr(p, 0x20b, 2);
	p[2] = 14;                      /* verze linkeru */
	pewr(p + 4, code, 4);
	pewr(p + 8, idata, 4);
	pewr(p + 12, udata, 4);
	pewr(p + 16, entry, 4);
	pewr(p + 20, perva[PText], 4);
	pewr(p + 24, pebase, 8);
	pewr(p + 32, 0x1000, 4);
	pewr(p + 36, 0x200, 4);
	pewr(p + 40, 6, 2);             /* OS 6.0 */
	pewr(p + 48, 6, 2);             /* subsystém 6.0 */
	pewr(p + 56, imagesz, 4);
	pewr(p + 60, hsize, 4);
	pewr(p + 68, 3, 2);             /* konzole */
	/* DYNAMIC_BASE, HIGH_ENTROPY_VA, NX_COMPAT, TERMINAL_SERVER_AWARE */
	pewr(p + 70, 0x8160, 2);
	pewr(p + 72, 8 << 20, 8);       /* zásobník: vyhrazený i přidělený 8 MiB */
	pewr(p + 80, 8 << 20, 8);
	pewr(p + 88, 1 << 20, 8);
	pewr(p + 96, 0x1000, 8);
	pewr(p + 108, 16, 4);
	if (pesize[PIdata]) {
		pewr(p + 112 + 8 * 1, perva[PIdata], 4);
		pewr(p + 112 + 8 * 1 + 4, 20 * (pendll + 1), 4);
		pewr(p + 112 + 8 * 12, peiat, 4);
		pewr(p + 112 + 8 * 12 + 4, peiatsz, 4);
	}
	if (pesize[PReloc]) {
		pewr(p + 112 + 8 * 5, perva[PReloc], 4);
		pewr(p + 112 + 8 * 5 + 4, pesize[PReloc], 4);
	}
	/* hlavičky sekcí */
	p = h + 24 + 240;
	for (k=0; k<NPOut; k++) {
		if (!pesize[k])
			continue;
		memcpy(p, pename[k], strlen(pename[k]));
		pewr(p + 8, pesize[k], 4);
		pewr(p + 12, perva[k], 4);
		pewr(p + 16, peraw[k], 4);
		pewr(p + 20, peoff[k], 4);
		pewr(p + 36, pech[k], 4);
		p += 40;
	}
	f = fopen(out, "wb");
	if (!f)
		peerr("cannot create %s", out);
	for (total=0; total<off; total++)
		fputc(buf[total], f);
	fclose(f);
	free(buf);
	luxir_chmod_exe(out);
}

int
linkpe(char **inputs, int ninputs, char *out, int mach, char *entry)
{
	int n, k;
	uint64_t len;
	uchar *d;
	size_t l;

	pemach = mach;
	peentry = entry ? entry : "_start";
	for (n=0; n<ninputs; n++) {
		d = peread(inputs[n], &len);
		l = strlen(inputs[n]);
		if (l > 4 && strcmp(inputs[n] + l - 4, ".def") == 0)
			pereaddef(inputs[n], d);
		else
			pereadobj(inputs[n], d, len);
	}
	peimports();
	pegc();
	pelayout();
	for (k=0; k<PReloc; k++) {
		pedata[k] = 0;
		if (pesize[k] && k != PBss) {
			pedata[k] = emalloc(pesize[k] + 1);
			memset(pedata[k], 0, pesize[k] + 1);
			if (k == PText)
				memset(pedata[k], 0xcc, pesize[k]);
		}
	}
	perelocate();
	pethunks();
	peidata();
	pereloc();
	pewrite(out);
	return 0;
}

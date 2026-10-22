#include "ld.h"

/* Vestavěný linker pro macOS (luxir -l -t amd64_apple|arm64_apple):
 * spustitelný Mach-O (MH_EXECUTE, PIE) z objektů Mach-O (výstup luxir -a
 * nebo clang) a textového stubu knihovny (.tbd) bez ld64 a LLVM.
 *
 * Dělá to, co LSL žádalo po ld64.lld (-dead_strip -x, libSystem.tbd,
 * platform_version macos 11.0): vstupní sekce s .subsections_via_symbols
 * se dělí na atomy po symbolech, do výstupu jdou jen atomy dosažitelné
 * relokacemi od _main. Sekce se slévají podle jména do segmentů __TEXT
 * a __DATA (zerofill na konec), volání funkcí knihovny jde přes stub
 * v __TEXT,__stubs a ukazatel v __DATA,__got, který dyld naváže při
 * načtení (bind bez líného navazování); absolutní ukazatele v datech
 * dostanou rebase. Příkazy: segmenty __PAGEZERO, __TEXT, __DATA,
 * __LINKEDIT, LC_DYLD_INFO_ONLY, LC_SYMTAB a LC_DYSYMTAB (importy pro
 * nepřímou tabulku), LC_LOAD_DYLINKER, LC_UUID (z obsahu), LC_BUILD_VERSION
 * (macOS 11.0), LC_MAIN, LC_LOAD_DYLIB a LC_CODE_SIGNATURE s ad-hoc
 * podpisem (CodeDirectory SHA-256, linker-signed; na arm64 povinný).
 * Výstup je deterministický. */

typedef struct MSec MSec;
typedef struct MSym MSym;
typedef struct MRel MRel;
typedef struct MObj MObj;
typedef struct MAtom MAtom;
typedef struct MOut MOut;
typedef struct MGlob MGlob;
typedef struct MImp MImp;

struct MRel {
	uint32_t off;      /* posun v sekci */
	int sym;           /* index symbolu objektu, nebo -1 */
	int sec;           /* extern = 0: sekce objektu (od 0), jinak -1 */
	int type;
	int pcrel, len;
	int64_t addend;    /* arm64: z relokace ADDEND */
};

struct MSec {
	char name[34];     /* "SEG,sect" */
	uint64_t addr, size;
	uint32_t align, flags;
	uchar *data;       /* 0 u zerofill */
	MRel *rel;
	uint nrel;
	int out;           /* výstupní sekce */
	uint atom0, natom; /* atomy sekce v moatoms */
};

struct MSym {
	char *name;
	int type, sect;    /* n_type, n_sect (od 1, 0 žádná) */
	uint64_t value;
	int glob;          /* index v mogs (externí), jinak -1 */
};

struct MObj {
	char *path;
	uchar *data;
	uint64_t len;
	MSec *sec;
	uint nsec;
	MSym *sym;
	uint nsym;
	int subs;          /* MH_SUBSECTIONS_VIA_SYMBOLS */
};

struct MAtom {
	int obj, sec;
	uint64_t start, end;  /* adresy v objektu */
	int live;
	uint64_t out;         /* adresa ve výstupu */
};

struct MOut {
	char name[34];
	int seg;           /* 1 __TEXT, 2 __DATA */
	uint32_t flags, align;
	uint64_t addr, size, off;
	uchar *data;       /* 0 u zerofill */
};

struct MGlob {
	char *name;
	int obj, sym;      /* definice, -1 nedefinovaný */
	int imp;           /* import z .tbd, -1 ne */
};

struct MImp {
	char *name;
	int used;
	uint64_t stub, got; /* adresy */
	int symidx;        /* index v tabulce symbolů výstupu */
};

enum { MaxOut = 64 };

static MObj *moobjs;
static uint monobjs;
static MAtom *moatoms;
static uint monatoms, moatomcap;
static MOut moouts[MaxOut];
static int monouts;
static MGlob *mogs;
static uint mongs;
static uint *mohash;
static uint mohashcap;
static MImp *moimps;
static uint monimps;
static char **motbdsyms;
static uint montbd;
static char *modylib;
static uint32_t modylibver;
static int moarm;                 /* arm64 (jinak x86-64) */
static uint64_t mopage;
static char *moentry;
static int mostubs, mogot;        /* indexy výstupních sekcí */
static uchar *morebase, *mobind;
static uint64_t mornrebase, mornbind, morebasecap, mobindcap;

static void
moerr(char *fmt, ...)
{
	va_list ap;

	fputs("luxir: link: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static uint64_t
mord(uchar *p, int n)
{
	uint64_t v;
	int i;

	v = 0;
	for (i=n-1; i>=0; i--)
		v = (v << 8) | p[i];
	return v;
}

static void
mowr(uchar *p, uint64_t v, int n)
{
	int i;

	for (i=0; i<n; i++)
		p[i] = (v >> (8 * i)) & 0xff;
}

/* big-endian (podpis) */
static void
mowrbe(uchar *p, uint64_t v, int n)
{
	int i;

	for (i=0; i<n; i++)
		p[i] = (v >> (8 * (n - 1 - i))) & 0xff;
}

static uint64_t
moal(uint64_t v, uint64_t a)
{
	return a > 1 ? (v + a - 1) & -a : v;
}

static uchar *
moread(char *path, uint64_t *len)
{
	FILE *f;
	uchar *d;
	long cap, n, got;

	f = fopen(path, "rb");
	if (!f)
		moerr("cannot open %s", path);
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

/* ---- dynamický buffer ---- */

typedef struct MBuf MBuf;
struct MBuf {
	uchar *p;
	uint64_t n, cap;
};

static void
mobput(MBuf *b, int c)
{
	if (b->n == b->cap) {
		b->cap = b->cap ? 2 * b->cap : 256;
		b->p = realloc(b->p, b->cap);
		if (!b->p)
			die("out of memory");
	}
	b->p[b->n++] = c;
}

static void
mobstr(MBuf *b, char *s)
{
	for (; *s; s++)
		mobput(b, *s);
	mobput(b, 0);
}

static void
mouleb(MBuf *b, uint64_t v)
{
	int c;

	do {
		c = v & 0x7f;
		v >>= 7;
		if (v)
			c |= 0x80;
		mobput(b, c);
	} while (v);
}

static void
mosleb(MBuf *b, int64_t v)
{
	int c;

	for (;;) {
		c = v & 0x7f;
		v >>= 7;
		if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) {
			mobput(b, c);
			return;
		}
		mobput(b, c | 0x80);
	}
}

/* ---- globální jména ---- */

static uint
mohashs(char *s)
{
	uint h;

	h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (uchar)*s) * 16777619u;
	return h;
}

static int
mofind(char *name, int add)
{
	uint i, k, cap;
	uint *old;

	if (!mohashcap || (add && 2 * (mongs + 1) > mohashcap)) {
		old = mohash;
		cap = mohashcap;
		mohashcap = cap ? 2 * cap : 1024;
		mohash = emalloc(mohashcap * sizeof mohash[0]);
		memset(mohash, 0, mohashcap * sizeof mohash[0]);
		for (k=0; k<mongs; k++) {
			i = mohashs(mogs[k].name) & (mohashcap - 1);
			while (mohash[i])
				i = (i + 1) & (mohashcap - 1);
			mohash[i] = k + 1;
		}
		free(old);
	}
	i = mohashs(name) & (mohashcap - 1);
	while (mohash[i]) {
		if (strcmp(mogs[mohash[i] - 1].name, name) == 0)
			return mohash[i] - 1;
		i = (i + 1) & (mohashcap - 1);
	}
	if (!add)
		return -1;
	mogs = realloc(mogs, (mongs + 1) * sizeof mogs[0]);
	if (!mogs)
		die("out of memory");
	mogs[mongs].name = name;
	mogs[mongs].obj = -1;
	mogs[mongs].sym = -1;
	mogs[mongs].imp = -1;
	mohash[i] = mongs + 1;
	return mongs++;
}

/* ---- vstupy ---- */

static void
moreadobj(char *path, uchar *d, uint64_t len)
{
	MObj *o;
	MSec *s;
	MSym *y;
	uchar *c, *e, *st;
	uint ncmds, k, n, j, q, nsect, symoff, nsyms, stroff, strsize;
	uint32_t cmd, csz, info;
	uint64_t pos;
	int64_t pend;
	int haspend;

	if (len < 32 || mord(d, 4) != 0xfeedfacf)
		moerr("%s: not a 64-bit Mach-O object", path);
	if (mord(d + 4, 4) != (moarm ? 0x0100000c : 0x01000007))
		moerr("%s: wrong architecture for the target", path);
	if (mord(d + 12, 4) != 1)
		moerr("%s: not an object file", path);
	monobjs++;
	moobjs = realloc(moobjs, monobjs * sizeof moobjs[0]);
	if (!moobjs)
		die("out of memory");
	o = &moobjs[monobjs - 1];
	memset(o, 0, sizeof *o);
	o->path = path;
	o->data = d;
	o->len = len;
	o->subs = (mord(d + 24, 4) & 0x2000) != 0;
	ncmds = mord(d + 16, 4);
	pos = 32;
	symoff = nsyms = stroff = strsize = 0;
	for (k=0; k<ncmds; k++) {
		if (pos + 8 > len)
			moerr("%s: truncated load commands", path);
		c = d + pos;
		cmd = mord(c, 4);
		csz = mord(c + 4, 4);
		if (csz < 8 || pos + csz > len)
			moerr("%s: bad load command", path);
		if (cmd == 0x19) {
			nsect = mord(c + 64, 4);
			if (72 + 80 * (uint64_t)nsect > csz)
				moerr("%s: bad segment", path);
			o->sec = realloc(o->sec, (o->nsec + nsect + 1) * sizeof o->sec[0]);
			if (!o->sec)
				die("out of memory");
			for (n=0; n<nsect; n++) {
				e = c + 72 + 80 * n;
				s = &o->sec[o->nsec++];
				memset(s, 0, sizeof *s);
				memcpy(s->name, e + 16, 16);
				s->name[16] = 0;
				j = strlen(s->name);
				s->name[j++] = ',';
				memcpy(s->name + j, e, 16);
				s->name[j + 16] = 0;
				s->addr = mord(e + 32, 8);
				s->size = mord(e + 40, 8);
				s->align = 1u << mord(e + 52, 4);
				s->flags = mord(e + 64, 4);
				s->out = -1;
				if ((s->flags & 0xff) == 1)
					s->data = 0;
				else {
					if (mord(e + 48, 4) + s->size > len)
						moerr("%s: section %s out of file", path, s->name);
					s->data = d + mord(e + 48, 4);
				}
				s->nrel = mord(e + 60, 4);
				if (mord(e + 56, 4) + 8 * (uint64_t)s->nrel > len)
					moerr("%s: relocations out of file", path);
				s->rel = emalloc((s->nrel + 1) * sizeof s->rel[0]);
				/* relokace: ADDEND (arm64) patří k následující */
				haspend = 0;
				pend = 0;
				j = 0;
				for (q=0; q<s->nrel; q++) {
					uchar *r = d + mord(e + 56, 4) + 8 * q;
					MRel *m;
					info = mord(r + 4, 4);
					if (mord(r, 4) & 0x80000000u)
						moerr("%s: scattered relocations are not supported", path);
					if (moarm && (info >> 28) == 10) {
						pend = ((int64_t)(info & 0xffffff) << 40) >> 40;
						haspend = 1;
						continue;
					}
					m = &s->rel[j++];
					m->off = mord(r, 4);
					m->pcrel = (info >> 24) & 1;
					m->len = (info >> 25) & 3;
					m->type = info >> 28;
					m->addend = haspend ? pend : 0;
					haspend = 0;
					if ((info >> 27) & 1) {
						m->sym = info & 0xffffff;
						m->sec = -1;
					} else {
						m->sym = -1;
						m->sec = (info & 0xffffff) - 1;
					}
				}
				s->nrel = j;
			}
		} else if (cmd == 2) {
			symoff = mord(c + 8, 4);
			nsyms = mord(c + 12, 4);
			stroff = mord(c + 16, 4);
			strsize = mord(c + 20, 4);
		}
		pos += csz;
	}
	if (symoff + 16 * (uint64_t)nsyms > len || stroff + (uint64_t)strsize > len)
		moerr("%s: bad symbol table", path);
	st = d + stroff;
	o->nsym = nsyms;
	o->sym = emalloc((nsyms + 1) * sizeof o->sym[0]);
	for (n=0; n<nsyms; n++) {
		e = d + symoff + 16 * n;
		y = &o->sym[n];
		k = mord(e, 4);
		if (k >= strsize)
			moerr("%s: bad symbol name", path);
		y->name = (char *)st + k;
		y->type = e[4];
		y->sect = e[5];
		y->value = mord(e + 8, 8);
		y->glob = -1;
		if (y->type & 0xe0)
			continue;           /* ladicí (stab) */
		if ((y->type & 0xe) == 0xe && y->sect > (int)o->nsec)
			moerr("%s: bad symbol section", path);
		if (y->type & 1) {
			int g = mofind(y->name, 1);
			y->glob = g;
			if ((y->type & 0xe) == 0xe) {
				if (mogs[g].obj >= 0)
					moerr("duplicate symbol %s (%s and %s)", y->name, moobjs[mogs[g].obj].path, path);
				mogs[g].obj = monobjs - 1;
				mogs[g].sym = n;
			} else if ((y->type & 0xe) != 0 || y->value)
				moerr("%s: unsupported symbol %s", path, y->name);
		}
	}
	for (n=0; n<o->nsec; n++)
		for (k=0; k<o->sec[n].nrel; k++) {
			MRel *m = &o->sec[n].rel[k];
			if ((m->sym >= 0 && (uint)m->sym >= nsyms) || (m->sym < 0 && (m->sec < 0 || (uint)m->sec >= o->nsec)))
				moerr("%s: bad relocation", path);
		}
}

/* .tbd: install-name, current-version a jména v symbols: [...] cílů,
 * mezi nimiž je architektura výstupu */
static int
mocontains(char *s, char *needle)
{
	size_t n;

	n = strlen(needle);
	for (; *s; s++)
		if (strncmp(s, needle, n) == 0)
			return 1;
	return 0;
}

static void
moreadtbd(char *path, uchar *d)
{
	char *p, *w, *arch;
	int ours, insyms;
	size_t n;

	arch = moarm ? "arm64-macos" : "x86_64-macos";
	ours = 0;
	insyms = 0;
	p = (char *)d;
	while (*p) {
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')
			p++;
		if (!*p)
			break;
		if (*p == '#') {
			while (*p && *p != '\n')
				p++;
			continue;
		}
		if (strncmp(p, "install-name:", 13) == 0) {
			p += 13;
			while (*p == ' ' || *p == '\'' || *p == '"')
				p++;
			w = p;
			while (*p && *p != '\'' && *p != '"' && *p != '\n')
				p++;
			n = p - w;
			modylib = emalloc(n + 1);
			memcpy(modylib, w, n);
			modylib[n] = 0;
			continue;
		}
		if (strncmp(p, "current-version:", 16) == 0) {
			p += 16;
			while (*p == ' ')
				p++;
			modylibver = 0;
			while (*p >= '0' && *p <= '9')
				modylibver = 10 * modylibver + (*p++ - '0');
			modylibver <<= 16;
			continue;
		}
		if (strncmp(p, "- targets:", 10) == 0 || strncmp(p, "targets:", 8) == 0) {
			w = strchr(p, ']');
			if (!w)
				moerr("%s: bad targets", path);
			*w = 0;
			ours = mocontains(p, arch);
			p = w + 1;
			continue;
		}
		if (strncmp(p, "symbols:", 8) == 0) {
			p += 8;
			while (*p == ' ')
				p++;
			if (*p != '[')
				moerr("%s: symbols list expected", path);
			p++;
			insyms = 1;
			continue;
		}
		if (insyms) {
			if (*p == ']') {
				insyms = 0;
				p++;
				continue;
			}
			if (*p == '\'' || *p == '"') {
				char q = *p++;
				w = p;
				while (*p && *p != q)
					p++;
				if (!*p)
					moerr("%s: unterminated name", path);
				*p++ = 0;
			} else {
				w = p;
				while (*p && *p != ',' && *p != ']' && *p != ' ' && *p != '\n')
					p++;
				if (*p == ']') {
					n = p - w;
					w = memcpy(emalloc(n + 1), w, n);
					w[n] = 0;
				} else if (*p)
					*p++ = 0;
			}
			if (ours && *w) {
				motbdsyms = realloc(motbdsyms, (montbd + 1) * sizeof motbdsyms[0]);
				if (!motbdsyms)
					die("out of memory");
				motbdsyms[montbd++] = w;
			}
			continue;
		}
		/* ostatní řádky */
		while (*p && *p != '\n')
			p++;
	}
	if (!modylib)
		moerr("%s: install-name missing", path);
}

static int
moimpcmp(const void *a, const void *b)
{
	return strcmp(((MImp *)a)->name, ((MImp *)b)->name);
}

/* nedefinované externí symboly z .tbd jsou importy (seřazené podle jména) */
static void
moimports(void)
{
	uint n, k;

	for (n=0; n<mongs; n++) {
		if (mogs[n].obj >= 0)
			continue;
		for (k=0; k<montbd; k++)
			if (strcmp(motbdsyms[k], mogs[n].name) == 0)
				break;
		if (k == montbd)
			continue;
		moimps = realloc(moimps, (monimps + 1) * sizeof moimps[0]);
		if (!moimps)
			die("out of memory");
		memset(&moimps[monimps], 0, sizeof moimps[0]);
		moimps[monimps].name = mogs[n].name;
		monimps++;
	}
	qsort(moimps, monimps, sizeof moimps[0], moimpcmp);
	for (k=0; k<monimps; k++)
		mogs[mofind(moimps[k].name, 0)].imp = k;
}

/* ---- atomy a dead strip ---- */

static int
moatomcmp(const void *a, const void *b)
{
	uint64_t x, y;

	x = *(uint64_t *)a;
	y = *(uint64_t *)b;
	return x < y ? -1 : x > y;
}

static void
moaddatom(int obj, int sec, uint64_t start, uint64_t end)
{
	if (monatoms == moatomcap) {
		moatomcap = moatomcap ? 2 * moatomcap : 1024;
		moatoms = realloc(moatoms, moatomcap * sizeof moatoms[0]);
		if (!moatoms)
			die("out of memory");
	}
	moatoms[monatoms].obj = obj;
	moatoms[monatoms].sec = sec;
	moatoms[monatoms].start = start;
	moatoms[monatoms].end = end;
	moatoms[monatoms].live = 0;
	moatoms[monatoms].out = 0;
	monatoms++;
}

/* sekce se dělí na atomy po symbolech (s .subsections_via_symbols), ne
 * sekce literálů */
static void
moatomize(void)
{
	uint n, m, k, cnt;
	MObj *o;
	MSec *s;
	uint64_t *cut;
	int t;

	for (n=0; n<monobjs; n++) {
		o = &moobjs[n];
		for (m=0; m<o->nsec; m++) {
			s = &o->sec[m];
			s->atom0 = monatoms;
			t = s->flags & 0xff;
			cut = emalloc((o->nsym + 2) * sizeof cut[0]);
			cnt = 0;
			cut[cnt++] = s->addr;
			if (o->subs && (t == 0 || t == 1))
				for (k=0; k<o->nsym; k++)
					if (!(o->sym[k].type & 0xe0) && (o->sym[k].type & 0xe) == 0xe && o->sym[k].sect == (int)m + 1
					&& o->sym[k].value > s->addr && o->sym[k].value < s->addr + s->size)
						cut[cnt++] = o->sym[k].value;
			qsort(cut, cnt, sizeof cut[0], moatomcmp);
			for (k=0; k<cnt; k++) {
				if (k > 0 && cut[k] == cut[k - 1])
					continue;
				moaddatom(n, m, cut[k], 0);
			}
			for (k=s->atom0; k<monatoms; k++)
				moatoms[k].end = k + 1 < monatoms ? moatoms[k + 1].start : s->addr + s->size;
			s->natom = monatoms - s->atom0;
			free(cut);
		}
	}
}

/* atom sekce s adresou v objektu (poslední se začátkem <= adresa) */
static int
moatomat(int obj, int sec, uint64_t a)
{
	MSec *s;
	uint lo, hi, mid;

	s = &moobjs[obj].sec[sec];
	lo = s->atom0;
	hi = s->atom0 + s->natom;
	if (lo == hi)
		moerr("%s: reference into empty section %s", moobjs[obj].path, s->name);
	while (hi - lo > 1) {
		mid = (lo + hi) / 2;
		if (moatoms[mid].start <= a)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

/* cíl relokace: atom (>= 0), import (-2 - index), nebo chyba */
static int
motarget(int obj, int sec, MRel *r, uint64_t *addr)
{
	MObj *o;
	MSym *y;
	MGlob *g;
	int to, k;
	uint64_t P, field;

	o = &moobjs[obj];
	if (r->sym >= 0) {
		y = &o->sym[r->sym];
		to = obj;
		if (y->glob >= 0) {
			g = &mogs[y->glob];
			if (g->obj < 0) {
				if (g->imp < 0)
					moerr("undefined symbol %s (%s)", y->name, o->path);
				return -2 - g->imp;
			}
			to = g->obj;
			y = &moobjs[to].sym[g->sym];
		}
		if ((y->type & 0xe) != 0xe || y->sect <= 0)
			moerr("%s: unsupported relocation target %s", o->path, y->name);
		*addr = y->value;
		return moatomat(to, y->sect - 1, y->value);
	}
	/* x86-64 relokace na sekci: cíl je adresa v poli */
	P = o->sec[sec].addr + r->off;
	field = mord(o->sec[sec].data + r->off, r->len == 3 ? 8 : 4);
	if (r->pcrel) {
		k = r->type == 6 ? 1 : r->type == 7 ? 2 : r->type == 8 ? 4 : 0;
		*addr = P + 4 + (int64_t)(int32_t)field + k;
	} else
		*addr = field;
	return moatomat(obj, r->sec, *addr);
}

static void
momark(uint a)
{
	uint *stack, ns, cap, k;
	MAtom *t;
	MSec *s;
	uint64_t addr;
	int to;

	cap = 1024;
	stack = emalloc(cap * sizeof stack[0]);
	ns = 0;
	if (!moatoms[a].live) {
		moatoms[a].live = 1;
		stack[ns++] = a;
	}
	while (ns) {
		t = &moatoms[stack[--ns]];
		s = &moobjs[t->obj].sec[t->sec];
		for (k=0; k<s->nrel; k++) {
			uint64_t P = s->addr + s->rel[k].off;
			if (P < t->start || P >= t->end)
				continue;
			to = motarget(t->obj, t->sec, &s->rel[k], &addr);
			if (to <= -2) {
				moimps[-2 - to].used = 1;
				continue;
			}
			if (!moatoms[to].live) {
				moatoms[to].live = 1;
				if (ns == cap) {
					cap *= 2;
					stack = realloc(stack, cap * sizeof stack[0]);
					if (!stack)
						die("out of memory");
				}
				stack[ns++] = to;
			}
		}
	}
	free(stack);
}

/* ---- rozvržení ---- */

static int
mooutsec(char *name, uint32_t flags, int seg)
{
	int k;

	for (k=0; k<monouts; k++)
		if (strcmp(moouts[k].name, name) == 0)
			return k;
	if (monouts == MaxOut)
		moerr("too many output sections");
	memset(&moouts[monouts], 0, sizeof moouts[0]);
	strcpy(moouts[monouts].name, name);
	moouts[monouts].flags = flags;
	moouts[monouts].seg = seg;
	moouts[monouts].align = 1;
	return monouts++;
}

static uint64_t mosegaddr[4], mosegsize[4], mosegoff[4], mosegfsize[4];
static uint64_t mohdrsize, molinkoff;
static int *moorder;   /* výstupní sekce v pořadí rozvržení */
static int monorder;

static uint64_t
mostubsize(void)
{
	return moarm ? 12 : 6;
}

static int
moiszerofill(MOut *s)
{
	return (s->flags & 0xff) == 1;
}

static void
molayout(void)
{
	uint n, m, k, a, nused;
	MObj *o;
	MSec *s;
	MOut *q;
	uint64_t addr, off;
	int seg, i, pass;

	/* výstupní sekce: __text, __stubs, další __TEXT; __got, další __DATA,
	 * zerofill na konec */
	mooutsec("__TEXT,__text", 0x80000400, 1);
	nused = 0;
	for (k=0; k<monimps; k++)
		if (moimps[k].used)
			nused++;
	mostubs = nused ? mooutsec("__TEXT,__stubs", 0x80000408, 1) : -1;
	for (n=0; n<monobjs; n++) {
		o = &moobjs[n];
		for (m=0; m<o->nsec; m++) {
			s = &o->sec[m];
			if (strncmp(s->name, "__TEXT,", 7) == 0)
				s->out = mooutsec(s->name, s->flags, 1);
		}
	}
	mogot = nused ? mooutsec("__DATA,__got", 6, 2) : -1;
	for (pass=0; pass<2; pass++)
		for (n=0; n<monobjs; n++) {
			o = &moobjs[n];
			for (m=0; m<o->nsec; m++) {
				s = &o->sec[m];
				if (strncmp(s->name, "__TEXT,", 7) == 0)
					continue;
				if (strncmp(s->name, "__DATA,", 7) != 0)
					moerr("%s: unsupported segment of section %s", o->path, s->name);
				if (((s->flags & 0xff) == 1) == pass)
					s->out = mooutsec(s->name, s->flags, 2);
			}
		}
	/* atomy do výstupních sekcí (posun modulo zarovnání sekce zachován) */
	for (i=0; i<monouts; i++)
		moouts[i].size = 0;
	for (n=0; n<monobjs; n++) {
		o = &moobjs[n];
		for (m=0; m<o->nsec; m++) {
			s = &o->sec[m];
			q = &moouts[s->out];
			if (s->align > q->align)
				q->align = s->align;
			for (a=s->atom0; a<s->atom0+s->natom; a++) {
				MAtom *t = &moatoms[a];
				uint64_t rel = (t->start - s->addr) & (s->align - 1);
				if (!t->live)
					continue;
				q->size += (rel - q->size) & (s->align - 1);
				t->out = q->size;   /* zatím posun ve výstupní sekci */
				q->size += t->end - t->start;
			}
		}
	}
	if (mostubs >= 0) {
		moouts[mostubs].align = 4;
		moouts[mostubs].size = nused * mostubsize();
	}
	if (mogot >= 0) {
		moouts[mogot].align = 8;
		moouts[mogot].size = nused * 8;
	}
	/* příkazy: velikost hlavičky */
	monorder = 0;
	moorder = emalloc((monouts + 1) * sizeof moorder[0]);
	for (seg=1; seg<=2; seg++)
		for (pass=0; pass<2; pass++)
			for (i=0; i<monouts; i++)
				if (moouts[i].seg == seg && moouts[i].size && moiszerofill(&moouts[i]) == pass)
					moorder[monorder++] = i;
	{
		uint ntext = 0, ndata = 0, dl;
		for (i=0; i<monorder; i++) {
			if (moouts[moorder[i]].seg == 1)
				ntext++;
			else
				ndata++;
		}
		dl = moal(strlen(modylib) + 1 + 24, 8);
		mohdrsize = 32 + 72 /* __PAGEZERO */ + 72 + 80 * ntext + (ndata ? 72 + 80 * ndata : 0)
			+ 72 /* __LINKEDIT */ + 48 /* LC_DYLD_INFO_ONLY */ + 24 /* LC_SYMTAB */ + 80 /* LC_DYSYMTAB */
			+ 32 /* LC_LOAD_DYLINKER */ + 24 /* LC_UUID */ + 24 /* LC_BUILD_VERSION */ + 24 /* LC_MAIN */
			+ dl /* LC_LOAD_DYLIB */ + 16 /* LC_CODE_SIGNATURE */;
	}
	/* adresy: __TEXT od báze s hlavičkou, __DATA na další stránce */
	mosegaddr[0] = 0;
	mosegsize[0] = 0x100000000ull;
	addr = 0x100000000ull + mohdrsize;
	mosegaddr[1] = 0x100000000ull;
	mosegoff[1] = 0;
	off = mohdrsize;
	for (seg=1; seg<=2; seg++) {
		if (seg == 2) {
			addr = moal(addr, mopage);
			off = moal(off, mopage);
			mosegaddr[2] = addr;
			mosegoff[2] = off;
		}
		for (i=0; i<monorder; i++) {
			q = &moouts[moorder[i]];
			if (q->seg != seg)
				continue;
			addr = moal(addr, q->align);
			if (!moiszerofill(q))
				off = moal(off, q->align);
			q->addr = addr;
			q->off = moiszerofill(q) ? 0 : off;
			addr += q->size;
			if (!moiszerofill(q))
				off += q->size;
		}
		if (seg == 1) {
			mosegsize[1] = moal(addr - mosegaddr[1], mopage);
			mosegfsize[1] = mosegsize[1];
			addr = mosegaddr[1] + mosegsize[1];
			off = mosegfsize[1];
		} else {
			mosegsize[2] = moal(addr - mosegaddr[2], mopage);
			mosegfsize[2] = moal(off - mosegoff[2], mopage);
		}
	}
	if (!mosegsize[2]) {
		mosegaddr[2] = mosegaddr[1] + mosegsize[1];
		mosegoff[2] = mosegfsize[1];
	}
	mosegaddr[3] = mosegaddr[2] + mosegsize[2];
	mosegoff[3] = mosegoff[2] + mosegfsize[2];
	molinkoff = mosegoff[3];
	/* atomy: konečné adresy */
	for (a=0; a<monatoms; a++)
		if (moatoms[a].live) {
			MSec *ss = &moobjs[moatoms[a].obj].sec[moatoms[a].sec];
			moatoms[a].out += moouts[ss->out].addr;
		}
	k = 0;
	for (n=0; n<monimps; n++)
		if (moimps[n].used) {
			moimps[n].stub = moouts[mostubs].addr + k * mostubsize();
			moimps[n].got = moouts[mogot].addr + 8 * k;
			k++;
		}
}

/* ---- relokace ---- */

static void
moaddrebase(uint64_t addr)
{
	MBuf b;

	if (addr < mosegaddr[2] || addr >= mosegaddr[2] + mosegsize[2])
		moerr("absolute address outside __DATA (text relocation)");
	memset(&b, 0, sizeof b);
	b.p = morebase;
	b.n = mornrebase;
	b.cap = morebasecap;
	/* SET_SEGMENT_AND_OFFSET_ULEB(__DATA), DO_REBASE_IMM_TIMES(1) */
	mobput(&b, 0x22);
	mouleb(&b, addr - mosegaddr[2]);
	mobput(&b, 0x51);
	morebase = b.p;
	mornrebase = b.n;
	morebasecap = b.cap;
}

static void
moaddbind(uint64_t addr, char *name, int64_t addend)
{
	MBuf b;

	if (addr < mosegaddr[2] || addr >= mosegaddr[2] + mosegsize[2])
		moerr("imported address outside __DATA (text relocation)");
	memset(&b, 0, sizeof b);
	b.p = mobind;
	b.n = mornbind;
	b.cap = mobindcap;
	/* SET_DYLIB_ORDINAL_IMM(1), SET_SYMBOL_TRAILING_FLAGS_IMM(0) jméno,
	 * SET_TYPE_IMM(pointer), SET_SEGMENT_AND_OFFSET_ULEB(__DATA),
	 * [SET_ADDEND_SLEB], DO_BIND */
	mobput(&b, 0x11);
	mobput(&b, 0x40);
	mobstr(&b, name);
	mobput(&b, 0x51);
	mobput(&b, 0x72);
	mouleb(&b, addr - mosegaddr[2]);
	/* addend platí i pro další záznamy: nastaví se vždy */
	mobput(&b, 0x60);
	mosleb(&b, addend);
	mobput(&b, 0x90);
	mobind = b.p;
	mornbind = b.n;
	mobindcap = b.cap;
}

/* adresa cíle ve výstupu: atom + posun, import: stub */
static uint64_t
motargetout(int to, uint64_t objaddr)
{
	if (to <= -2)
		return moimps[-2 - to].stub;
	return moatoms[to].out + (objaddr - moatoms[to].start);
}

static void
morelocate(void)
{
	uint n, m, k, a;
	MObj *o;
	MSec *s;
	MAtom *t;
	MRel *r;
	uchar *f, *base;
	uint64_t P, S, objaddr;
	int64_t v, A;
	uint32_t ins;
	int to, sc;

	for (n=0; n<monobjs; n++) {
		o = &moobjs[n];
		for (m=0; m<o->nsec; m++) {
			s = &o->sec[m];
			for (a=s->atom0; a<s->atom0+s->natom; a++) {
				t = &moatoms[a];
				if (!t->live || !s->data)
					continue;
				base = moouts[s->out].data + (t->out - moouts[s->out].addr);
				memcpy(base, s->data + (t->start - s->addr), t->end - t->start);
			}
			for (k=0; k<s->nrel; k++) {
				r = &s->rel[k];
				a = moatomat(n, m, s->addr + r->off);
				t = &moatoms[a];
				if (!t->live)
					continue;
				P = t->out + (s->addr + r->off - t->start);
				f = moouts[s->out].data + (P - moouts[s->out].addr);
				to = motarget(n, m, r, &objaddr);
				S = motargetout(to, objaddr);
				if (!moarm) {
					switch (r->type) {
					case 0: /* UNSIGNED */
						if (r->len == 3) {
							A = r->sym >= 0 ? (int64_t)mord(f, 8) : 0;
							v = S + A;
							mowr(f, v, 8);
							if (to <= -2) {
								mowr(f, 0, 8);
								moaddbind(P, moimps[-2 - to].name, A);
							} else
								moaddrebase(P);
						} else
							moerr("%s: 32-bit absolute address in a position-independent executable", o->path);
						break;
					case 1: case 2: case 6: case 7: case 8: /* SIGNED, BRANCH, SIGNED_1/2/4 */
						if (r->sym >= 0)
							v = (int64_t)S + (int32_t)mord(f, 4) - (int64_t)(P + 4);
						else {
							/* na sekci: pole je posun v objektu, cíl se posune s atomem */
							int64_t Pobj = s->addr + r->off;
							v = (int32_t)mord(f, 4) + ((int64_t)S - (int64_t)objaddr) - ((int64_t)P - Pobj);
						}
						if (v < INT32_MIN || v > INT32_MAX)
							moerr("%s: relocation out of range", o->path);
						mowr(f, v, 4);
						break;
					default:
						moerr("%s: unsupported relocation type %d", o->path, r->type);
					}
					continue;
				}
				A = r->addend;
				ins = mord(f, 4);
				switch (r->type) {
				case 0: /* UNSIGNED */
					if (r->len != 3)
						moerr("%s: 32-bit absolute address in a position-independent executable", o->path);
					A = mord(f, 8);
					if (to <= -2) {
						mowr(f, 0, 8);
						moaddbind(P, moimps[-2 - to].name, A);
					} else {
						mowr(f, S + A, 8);
						moaddrebase(P);
					}
					break;
				case 2: /* BRANCH26 */
					v = (int64_t)(S + A) - (int64_t)P;
					if ((v & 3) || v < -(1 << 27) || v >= (1 << 27))
						moerr("%s: branch out of range", o->path);
					mowr(f, (ins & 0xfc000000) | ((v >> 2) & 0x3ffffff), 4);
					break;
				case 3: /* PAGE21 */
					v = (int64_t)((S + A) >> 12) - (int64_t)(P >> 12);
					if (v < -(1 << 20) || v >= (1 << 20))
						moerr("%s: adrp out of range", o->path);
					mowr(f, (ins & 0x9f00001f) | ((uint32_t)(v & 3) << 29) | ((uint32_t)((v >> 2) & 0x7ffff) << 5), 4);
					break;
				case 4: /* PAGEOFF12: add, nebo ldr/str (škálované) */
					v = (S + A) & 0xfff;
					sc = 0;
					if ((ins & 0x3b000000) == 0x39000000) {
						sc = ins >> 30;
						if ((ins & 0x04800000) == 0x04800000)
							sc = 4;
					}
					if (v & ((1 << sc) - 1))
						moerr("%s: misaligned page offset", o->path);
					mowr(f, (ins & ~(0xfffu << 10)) | ((uint32_t)(v >> sc) << 10), 4);
					break;
				default:
					moerr("%s: unsupported relocation type %d", o->path, r->type);
				}
			}
		}
	}
	/* stuby a GOT */
	k = 0;
	for (n=0; n<monimps; n++) {
		if (!moimps[n].used)
			continue;
		P = moimps[n].stub;
		S = moimps[n].got;
		f = moouts[mostubs].data + (P - moouts[mostubs].addr);
		if (moarm) {
			v = (int64_t)(S >> 12) - (int64_t)(P >> 12);
			mowr(f, 0x90000010 | ((uint32_t)(v & 3) << 29) | ((uint32_t)((v >> 2) & 0x7ffff) << 5), 4);
			mowr(f + 4, 0xf9400210 | ((uint32_t)((S & 0xfff) >> 3) << 10), 4);
			mowr(f + 8, 0xd61f0200, 4);
		} else {
			f[0] = 0xff;
			f[1] = 0x25;
			mowr(f + 2, (int64_t)S - (int64_t)(P + 6), 4);
		}
		moaddbind(S, moimps[n].name, 0);
		k++;
	}
}

/* ---- SHA-256 (podpis a UUID) ---- */

static uint32_t mosha_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t
moror(uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

static void
moshablock(uint32_t *h, uchar *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, hh, t1, t2;
	int i;

	for (i=0; i<16; i++)
		w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
	for (i=16; i<64; i++)
		w[i] = (moror(w[i - 2], 17) ^ moror(w[i - 2], 19) ^ (w[i - 2] >> 10)) + w[i - 7]
			+ (moror(w[i - 15], 7) ^ moror(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 16];
	a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
	for (i=0; i<64; i++) {
		t1 = hh + (moror(e, 6) ^ moror(e, 11) ^ moror(e, 25)) + ((e & f) ^ (~e & g)) + mosha_k[i] + w[i];
		t2 = (moror(a, 2) ^ moror(a, 13) ^ moror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void
mosha256(uchar *p, uint64_t n, uchar *out)
{
	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	uchar last[128];
	uint64_t i, r;
	int k;

	for (i=0; i+64<=n; i+=64)
		moshablock(h, p + i);
	r = n - i;
	memset(last, 0, sizeof last);
	memcpy(last, p + i, r);
	last[r] = 0x80;
	k = r < 56 ? 64 : 128;
	mowrbe(last + k - 8, n * 8, 8);
	moshablock(h, last);
	if (k == 128)
		moshablock(h, last + 64);
	for (k=0; k<8; k++)
		mowrbe(out + 4 * k, h[k], 4);
}

/* ---- výstup ---- */

static uint
molog2(uint64_t a)
{
	uint k;

	for (k=0; ((uint64_t)1 << k) < a; k++)
		;
	return k;
}

static void
mocmd(MBuf *b, uint32_t v)
{
	int i;

	for (i=0; i<4; i++)
		mobput(b, (v >> (8 * i)) & 0xff);
}

static void
mocmd64(MBuf *b, uint64_t v)
{
	mocmd(b, v);
	mocmd(b, v >> 32);
}

static void
moname16(MBuf *b, char *s)
{
	int i;
	size_t n;

	n = strlen(s);
	for (i=0; i<16; i++)
		mobput(b, i < (int)n ? s[i] : 0);
}

static void
mosegcmd(MBuf *b, char *name, int seg, int prot, int nsect)
{
	mocmd(b, 0x19);
	mocmd(b, 72 + 80 * nsect);
	moname16(b, name);
	mocmd64(b, mosegaddr[seg]);
	mocmd64(b, mosegsize[seg]);
	mocmd64(b, seg == 0 ? 0 : mosegoff[seg]);
	mocmd64(b, seg == 0 ? 0 : mosegfsize[seg]);
	mocmd(b, prot);
	mocmd(b, prot);
	mocmd(b, nsect);
	mocmd(b, 0);
}

static void
mowrite(char *out)
{
	MBuf cmds, link, exp;
	uchar *buf, hash[32];
	uint64_t total, rebaseoff, bindoff, expoff, symoff, indoff, stroff, sigoff, sigsize, nslots, codelimit, pos;
	uint n, k, nused, ntext, ndata, nsyms, strsize;
	int i, g, rebasen, bindn;
	char *ident, *slash;
	MObj *o;
	FILE *f;

	nused = 0;
	for (k=0; k<monimps; k++)
		if (moimps[k].used)
			nused++;
	/* __LINKEDIT: rebase, bind, export, symboly, nepřímé, řetězce, podpis */
	memset(&link, 0, sizeof link);
	memset(&exp, 0, sizeof exp);
	rebasen = mornrebase ? mornrebase + 2 : 0;
	rebaseoff = molinkoff;
	if (rebasen) {
		/* REBASE_OPCODE_SET_TYPE_IMM(pointer) na začátek, DONE na konec */
		mobput(&link, 0x11);
		for (pos=0; pos<mornrebase; pos++)
			mobput(&link, morebase[pos]);
		mobput(&link, 0);
		while (link.n % 8)
			mobput(&link, 0);
	}
	rebasen = link.n;
	bindoff = molinkoff + link.n;
	for (pos=0; pos<mornbind; pos++)
		mobput(&link, mobind[pos]);
	if (mornbind) {
		mobput(&link, 0);
		while (link.n % 8)
			mobput(&link, 0);
	}
	bindn = link.n - rebasen;
	/* export trie: jen __mh_execute_header na adrese 0 */
	mobput(&exp, 0);
	mobput(&exp, 1);
	mobstr(&exp, "__mh_execute_header");
	mouleb(&exp, exp.n + 1);
	mobput(&exp, 2);
	mobput(&exp, 0);
	mobput(&exp, 0);
	mobput(&exp, 0);
	while (exp.n % 8)
		mobput(&exp, 0);
	expoff = molinkoff + link.n;
	for (pos=0; pos<exp.n; pos++)
		mobput(&link, exp.p[pos]);
	/* symboly: __mh_execute_header, importy (podle jména) */
	symoff = molinkoff + link.n;
	nsyms = 1 + nused;
	strsize = 2 + strlen("__mh_execute_header") + 1;
	for (k=0; k<monimps; k++)
		if (moimps[k].used)
			strsize += strlen(moimps[k].name) + 1;
	strsize = moal(strsize, 8);
	{
		uint64_t sx = 2;
		/* n_strx, n_type, n_sect, n_desc, n_value */
		mocmd(&link, sx);
		mobput(&link, 0x0f);
		mobput(&link, 1);
		mobput(&link, 0x10);
		mobput(&link, 0);
		mocmd64(&link, 0x100000000ull);
		sx += strlen("__mh_execute_header") + 1;
		i = 1;
		for (k=0; k<monimps; k++)
			if (moimps[k].used) {
				moimps[k].symidx = i++;
				mocmd(&link, sx);
				mobput(&link, 0x01);
				mobput(&link, 0);
				mobput(&link, 0);
				mobput(&link, 1);    /* knihovna 1 */
				mocmd64(&link, 0);
				sx += strlen(moimps[k].name) + 1;
			}
	}
	indoff = molinkoff + link.n;
	/* nepřímá tabulka: __stubs a __got (stejné pořadí) */
	for (i=0; i<2; i++)
		for (k=0; k<monimps; k++)
			if (moimps[k].used)
				mocmd(&link, moimps[k].symidx);
	stroff = molinkoff + link.n;
	mobput(&link, ' ');
	mobput(&link, 0);
	mobstr(&link, "__mh_execute_header");
	for (k=0; k<monimps; k++)
		if (moimps[k].used)
			mobstr(&link, moimps[k].name);
	while (link.n < stroff - molinkoff + strsize)
		mobput(&link, 0);
	/* podpis: identifikátor podle jména výstupu */
	slash = 0;
	for (ident=out; *ident; ident++)
		if (*ident == '/')
			slash = ident;
	ident = slash ? slash + 1 : out;
	while (link.n % 16)
		mobput(&link, 0);
	sigoff = molinkoff + link.n;
	codelimit = sigoff;
	nslots = (codelimit + 4095) / 4096;
	sigsize = 12 + 8 + 88 + strlen(ident) + 1 + 32 * nslots;
	sigsize = moal(sigsize, 16);
	total = sigoff + sigsize;
	mosegfsize[3] = total - mosegoff[3];
	mosegsize[3] = moal(mosegfsize[3], mopage);

	/* příkazy */
	memset(&cmds, 0, sizeof cmds);
	ntext = ndata = 0;
	for (i=0; i<monorder; i++) {
		if (moouts[moorder[i]].seg == 1)
			ntext++;
		else
			ndata++;
	}
	mosegcmd(&cmds, "__PAGEZERO", 0, 0, 0);
	for (n=1; n<=2; n++) {
		if (n == 2 && !ndata)
			continue;
		mosegcmd(&cmds, n == 1 ? "__TEXT" : "__DATA", n, n == 1 ? 5 : 3, n == 1 ? ntext : ndata);
		for (i=0; i<monorder; i++) {
			MOut *q = &moouts[moorder[i]];
			char *c;
			if (q->seg != (int)n)
				continue;
			c = strchr(q->name, ',');
			moname16(&cmds, c + 1);
			*c = 0;
			moname16(&cmds, q->name);
			*c = ',';
			mocmd64(&cmds, q->addr);
			mocmd64(&cmds, q->size);
			mocmd(&cmds, q->off);
			mocmd(&cmds, molog2(q->align));
			mocmd(&cmds, 0);
			mocmd(&cmds, 0);
			mocmd(&cmds, q->flags);
			/* reserved1: index v nepřímé tabulce, reserved2: velikost stubu */
			mocmd(&cmds, moorder[i] == mogot ? nused : 0);
			mocmd(&cmds, moorder[i] == mostubs ? mostubsize() : 0);
			mocmd(&cmds, 0);
		}
	}
	mosegcmd(&cmds, "__LINKEDIT", 3, 1, 0);
	/* LC_DYLD_INFO_ONLY */
	mocmd(&cmds, 0x80000022);
	mocmd(&cmds, 48);
	mocmd(&cmds, rebasen ? rebaseoff : 0);
	mocmd(&cmds, rebasen);
	mocmd(&cmds, bindn ? bindoff : 0);
	mocmd(&cmds, bindn);
	mocmd(&cmds, 0);
	mocmd(&cmds, 0);
	mocmd(&cmds, 0);
	mocmd(&cmds, 0);
	mocmd(&cmds, expoff);
	mocmd(&cmds, exp.n);
	/* LC_SYMTAB */
	mocmd(&cmds, 2);
	mocmd(&cmds, 24);
	mocmd(&cmds, symoff);
	mocmd(&cmds, nsyms);
	mocmd(&cmds, stroff);
	mocmd(&cmds, strsize);
	/* LC_DYSYMTAB */
	mocmd(&cmds, 0xb);
	mocmd(&cmds, 80);
	mocmd(&cmds, 0);
	mocmd(&cmds, 0);
	mocmd(&cmds, 0);
	mocmd(&cmds, 1);
	mocmd(&cmds, 1);
	mocmd(&cmds, nused);
	for (i=0; i<6; i++)
		mocmd(&cmds, 0);
	mocmd(&cmds, nused ? indoff : 0);
	mocmd(&cmds, 2 * nused);
	for (i=0; i<4; i++)
		mocmd(&cmds, 0);
	/* LC_LOAD_DYLINKER */
	mocmd(&cmds, 0xe);
	mocmd(&cmds, 32);
	mocmd(&cmds, 12);
	{
		char dy[20];
		memset(dy, 0, sizeof dy);
		strcpy(dy, "/usr/lib/dyld");
		for (i=0; i<20; i++)
			mobput(&cmds, dy[i]);
	}
	/* LC_UUID (vyplní se z obsahu) */
	mocmd(&cmds, 0x1b);
	mocmd(&cmds, 24);
	for (i=0; i<16; i++)
		mobput(&cmds, 0);
	/* LC_BUILD_VERSION: macOS 11.0, SDK 11.0 */
	mocmd(&cmds, 0x32);
	mocmd(&cmds, 24);
	mocmd(&cmds, 1);
	mocmd(&cmds, 0xb0000);
	mocmd(&cmds, 0xb0000);
	mocmd(&cmds, 0);
	/* LC_MAIN */
	g = mofind(moentry, 0);
	if (g < 0 || mogs[g].obj < 0)
		moerr("entry symbol %s is not defined", moentry);
	o = &moobjs[mogs[g].obj];
	{
		uint64_t ea;
		int at = moatomat(mogs[g].obj, o->sym[mogs[g].sym].sect - 1, o->sym[mogs[g].sym].value);
		ea = moatoms[at].out + (o->sym[mogs[g].sym].value - moatoms[at].start);
		mocmd(&cmds, 0x80000028);
		mocmd(&cmds, 24);
		mocmd64(&cmds, ea - 0x100000000ull);
		mocmd64(&cmds, 0);
	}
	/* LC_LOAD_DYLIB */
	{
		uint dl = moal(strlen(modylib) + 1 + 24, 8);
		mocmd(&cmds, 0xc);
		mocmd(&cmds, dl);
		mocmd(&cmds, 24);
		mocmd(&cmds, 2);
		mocmd(&cmds, modylibver);
		mocmd(&cmds, 0x10000);
		for (k=24; k<dl; k++)
			mobput(&cmds, k - 24 < strlen(modylib) ? modylib[k - 24] : 0);
	}
	/* LC_CODE_SIGNATURE */
	mocmd(&cmds, 0x1d);
	mocmd(&cmds, 16);
	mocmd(&cmds, sigoff);
	mocmd(&cmds, sigsize);
	if (32 + cmds.n != mohdrsize)
		die("macho: load command size mismatch");

	/* obraz */
	buf = emalloc(total + 1);
	memset(buf, 0, total + 1);
	mowr(buf, 0xfeedfacf, 4);
	mowr(buf + 4, moarm ? 0x0100000c : 0x01000007, 4);
	mowr(buf + 8, moarm ? 0 : 3, 4);
	mowr(buf + 12, 2, 4);              /* MH_EXECUTE */
	mowr(buf + 16, 13 - (ndata ? 0 : 1), 4);
	mowr(buf + 20, cmds.n, 4);
	/* NOUNDEFS, DYLDLINK, TWOLEVEL, PIE */
	mowr(buf + 24, 0x200085, 4);
	memcpy(buf + 32, cmds.p, cmds.n);
	for (i=0; i<monouts; i++)
		if (moouts[i].data && moouts[i].size)
			memcpy(buf + moouts[i].off, moouts[i].data, moouts[i].size);
	memcpy(buf + molinkoff, link.p, link.n);
	/* UUID: SHA-256 obsahu bez podpisu (UUID nulové), verze 4 */
	mosha256(buf, sigoff, hash);
	hash[6] = (hash[6] & 0x0f) | 0x40;
	hash[8] = (hash[8] & 0x3f) | 0x80;
	for (pos=32; pos+24<=32+cmds.n; ) {
		uint32_t c = mord(buf + pos, 4), cs = mord(buf + pos + 4, 4);
		if (c == 0x1b)
			memcpy(buf + pos + 8, hash, 16);
		pos += cs;
	}
	/* ad-hoc podpis: SuperBlob s CodeDirectory (big-endian) */
	{
		uchar *p = buf + sigoff;
		uint64_t cdlen = 88 + strlen(ident) + 1 + 32 * nslots;
		mowrbe(p, 0xfade0cc0, 4);
		mowrbe(p + 4, 20 + cdlen, 4);
		mowrbe(p + 8, 1, 4);
		mowrbe(p + 12, 0, 4);          /* CSSLOT_CODEDIRECTORY */
		mowrbe(p + 16, 20, 4);
		p += 20;
		mowrbe(p, 0xfade0c02, 4);
		mowrbe(p + 4, cdlen, 4);
		mowrbe(p + 8, 0x20400, 4);
		mowrbe(p + 12, 0x20002, 4);    /* ADHOC | LINKER_SIGNED */
		mowrbe(p + 16, 88 + strlen(ident) + 1, 4);
		mowrbe(p + 20, 88, 4);
		mowrbe(p + 24, 0, 4);
		mowrbe(p + 28, nslots, 4);
		mowrbe(p + 32, codelimit, 4);
		p[36] = 32;
		p[37] = 2;                     /* SHA-256 */
		p[38] = 0;
		p[39] = 12;                    /* stránka 4 KiB */
		/* codeLimit64, execSegBase, execSegLimit, execSegFlags */
		mowrbe(p + 56, 0, 8);
		mowrbe(p + 64, mosegoff[1], 8);
		mowrbe(p + 72, mosegfsize[1], 8);
		mowrbe(p + 80, 1, 8);          /* CS_EXECSEG_MAIN_BINARY */
		memcpy(p + 88, ident, strlen(ident));
		for (pos=0; pos<nslots; pos++) {
			uint64_t a = 4096 * pos, l = codelimit - a < 4096 ? codelimit - a : 4096;
			mosha256(buf + a, l, p + 88 + strlen(ident) + 1 + 32 * pos);
		}
	}
	f = fopen(out, "wb");
	if (!f)
		moerr("cannot create %s", out);
	for (pos=0; pos<total; pos++)
		fputc(buf[pos], f);
	fclose(f);
	free(buf);
	luxir_chmod_exe(out);
}

/* mapa pro testy (luxir -l -M soubor): živé atomy (objekt, sekce,
 * začátek, konec, adresa) a importy (jméno, stub, GOT) */
static void
momap(void)
{
	char *path;
	FILE *f;
	uint a, k;

	path = ldmap;
	if (!path || !*path)
		return;
	f = fopen(path, "w");
	if (!f)
		moerr("cannot create %s", path);
	for (a=0; a<monatoms; a++)
		if (moatoms[a].live)
			fprintf(f, "A %d %d %llu %llu %llu\n", moatoms[a].obj, moatoms[a].sec,
				(unsigned long long)moatoms[a].start, (unsigned long long)moatoms[a].end,
				(unsigned long long)moatoms[a].out);
	for (k=0; k<monimps; k++)
		if (moimps[k].used)
			fprintf(f, "I %s %llu %llu\n", moimps[k].name,
				(unsigned long long)moimps[k].stub, (unsigned long long)moimps[k].got);
	fclose(f);
}

int
linkmacho(char **inputs, int ninputs, char *out, int arm, char *entry)
{
	int n, g, k;
	uint64_t len;
	uchar *d;
	size_t l;
	MObj *o;

	moarm = arm;
	mopage = arm ? 0x4000 : 0x1000;
	moentry = entry ? entry : "_main";
	for (n=0; n<ninputs; n++) {
		d = moread(inputs[n], &len);
		l = strlen(inputs[n]);
		if (l > 4 && strcmp(inputs[n] + l - 4, ".tbd") == 0)
			moreadtbd(inputs[n], d);
		else
			moreadobj(inputs[n], d, len);
	}
	if (!modylib)
		moerr("a library stub (.tbd) is required");
	/* nedefinovaný symbol mimo knihovnu je chyba až při použití (motarget) */
	moimports();
	moatomize();
	g = mofind(moentry, 0);
	if (g < 0 || mogs[g].obj < 0)
		moerr("entry symbol %s is not defined", moentry);
	o = &moobjs[mogs[g].obj];
	if ((o->sym[mogs[g].sym].type & 0xe) != 0xe)
		moerr("entry symbol %s has no section", moentry);
	momark(moatomat(mogs[g].obj, o->sym[mogs[g].sym].sect - 1, o->sym[mogs[g].sym].value));
	molayout();
	for (k=0; k<monouts; k++) {
		moouts[k].data = 0;
		if (!moiszerofill(&moouts[k]) && moouts[k].size) {
			moouts[k].data = emalloc(moouts[k].size + 1);
			memset(moouts[k].data, 0, moouts[k].size + 1);
		}
	}
	morelocate();
	mowrite(out);
	momap();
	return 0;
}

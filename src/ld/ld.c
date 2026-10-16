#include "ld.h"

/* Vestavěný linker LuxIR: statické spustitelné soubory ELF pro Linux
 * z relokovatelných objektů (výstup luxir -a nebo GNU as).
 *
 * Dělá to, co LSL dnes žádá po GNU ld: -nostdlib -static [-pie
 * --no-dynamic-linker -z text] -z noexecstack --build-id=none
 * --gc-sections -s. Vstupní sekce se podle jména slévají do .text,
 * .rodata, .data a .bss; nepoužité sekce (bez cesty relokací od vstupního
 * bodu) se vypustí; relokace se vyřeší staticky. Výstup PIE (ET_DYN bez
 * dynamické sekce: jádro zvolí náhodnou bázi) nesmí obsahovat absolutní
 * relokace, jinak by potřeboval zavaděč; výstup bez PIE (i386) je ET_EXEC
 * na pevné bázi. Segmenty: hlavičky (R), .text (RX), .rodata (R),
 * .data+.bss (RW), PT_GNU_STACK bez práva spouštění. Tabulka symbolů se
 * nezapisuje (strip), zůstávají hlavičky sekcí pro nástroje. */

typedef struct ISec ISec;
typedef struct ISym ISym;
typedef struct IRel IRel;
typedef struct IObj IObj;
typedef struct OSec OSec;

struct IRel {
	uint64_t off;
	uint32_t sym;
	uint32_t type;
	int64_t add;
};

struct ISec {
	char *name;
	uint32_t type;
	uint64_t flags;
	uint64_t size;
	uint64_t align;
	uchar *data;
	IRel *rel;
	uint nrel;
	int rela;        /* 1 RELA, 0 REL (addend v poli) */
	int out;         /* výstupní sekce nebo -1 */
	uint64_t addr;   /* adresa po rozvržení */
	int keep;        /* přežije gc */
	int info;        /* sh_info relokační sekce (index sekce) */
};

struct ISym {
	char *name;
	uint64_t value;
	uint64_t size;
	int bind;        /* 0 local, 1 global, 2 weak */
	int type;
	int shndx;       /* index sekce, 0 nedefinovaný, -1 ABS, -2 COMMON */
	int gsym;        /* index v globální tabulce nebo -1 */
};

struct IObj {
	char *path;
	uchar *data;
	uint64_t size;
	ISec *sec;
	uint nsec;
	ISym *sym;
	uint nsym;
};

struct OSec {
	char *name;
	uint64_t flags;
	int nobits;
	uint64_t addr, off, size, align;
};

typedef struct GSym GSym;
struct GSym {
	char *name;
	IObj *obj;
	int sym;         /* index v obj->sym, -1 nedefinovaný */
	int weak;
	uint64_t common; /* velikost common, 0 = ne */
	uint64_t calign;
	uint64_t value;  /* adresa po rozvržení */
	int sec;         /* výstupní sekce commonu */
};

static IObj *objs;
static uint nobjs;
static GSym *gsyms;
static uint ngsyms;
static int elf64, machine, pie;
static uint64_t base;
static char *entryname;

/* výstupní sekce: .text, .rodata, .data, .bss */
enum { OText, ORodata, OData, OBss, NOut };
static OSec osecs[NOut] = {
	{".text", 6, 0, 0, 0, 0, 1},
	{".rodata", 2, 0, 0, 0, 0, 1},
	{".data", 3, 0, 0, 0, 0, 1},
	{".bss", 3, 1, 0, 0, 0, 1},
};

static void
lerr(char *fmt, ...)
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
xgrow(void *p, uint n, size_t esz)
{
	size_t m;

	m = n - 1;
	if (n != 1 && (m < 8 || (m & (m - 1)) != 0))
		return p;
	p = realloc(p, (n == 1 ? 8 : 2 * m) * esz);
	if (!p)
		die("out of memory");
	return p;
}

#define GROW(v, n) ((v) = xgrow((v), (n), sizeof (v)[0]))

static uint64_t
rd(uchar *p, int n)
{
	uint64_t v;
	int i;

	v = 0;
	for (i=n-1; i>=0; i--)
		v = (v << 8) | p[i];
	return v;
}

static void
wr(uchar *p, uint64_t v, int n)
{
	int i;

	for (i=0; i<n; i++)
		p[i] = (v >> (8 * i)) & 0xff;
}

static uint64_t
alignup(uint64_t v, uint64_t a)
{
	return a > 1 ? (v + a - 1) & -a : v;
}

/* ---- čtení objektů ---- */

static void
readobj(char *path)
{
	FILE *f;
	IObj *o;
	ISec *s;
	ISym *y;
	uchar *d, *sh, *st;
	uint64_t shoff, shsize, size, symoff, symsize, stroff, roff, rsize, rent;
	uint n, m, shnum, shstr, nsym, link, nrel;
	int cls, shent, syment;
	long len, cap, got;

	f = fopen(path, "rb");
	if (!f)
		lerr("cannot open %s", path);
	cap = 1 << 16;
	d = emalloc(cap);
	len = 0;
	for (;;) {
		if (len + 4096 > cap) {
			cap *= 2;
			d = realloc(d, cap);
			if (!d)
				die("out of memory");
		}
		got = fread(d + len, 1, cap - len, f);
		if (got == 0)
			break;
		len += got;
	}
	fclose(f);
	if (len < 64)
		lerr("%s: not an ELF object", path);
	if (memcmp(d, "\177ELF", 4) != 0 || d[5] != 1)
		lerr("%s: not a little-endian ELF file", path);
	cls = d[4];
	if (cls != (elf64 ? 2 : 1))
		lerr("%s: wrong ELF class for the target", path);
	if (rd(d + 16, 2) != 1)
		lerr("%s: not a relocatable object", path);
	if ((int)rd(d + 18, 2) != machine)
		lerr("%s: wrong machine for the target", path);
	GROW(objs, nobjs + 1);
	o = &objs[nobjs++];
	memset(o, 0, sizeof *o);
	o->path = path;
	o->data = d;
	o->size = len;
	if (elf64) {
		shoff = rd(d + 40, 8);
		shent = rd(d + 58, 2);
		shnum = rd(d + 60, 2);
		shstr = rd(d + 62, 2);
	} else {
		shoff = rd(d + 32, 4);
		shent = rd(d + 46, 2);
		shnum = rd(d + 48, 2);
		shstr = rd(d + 50, 2);
	}
	if (shoff + (uint64_t)shnum * shent > (uint64_t)len)
		lerr("%s: truncated section headers", path);
	o->nsec = shnum;
	o->sec = emalloc((shnum + 1) * sizeof o->sec[0]);
	memset(o->sec, 0, (shnum + 1) * sizeof o->sec[0]);
	st = 0;
	if (shstr < shnum) {
		sh = d + shoff + shstr * shent;
		st = d + (elf64 ? rd(sh + 24, 8) : rd(sh + 16, 4));
	}
	for (n=0; n<shnum; n++) {
		sh = d + shoff + n * shent;
		s = &o->sec[n];
		if (elf64) {
			s->name = st ? (char *)st + rd(sh, 4) : "";
			s->type = rd(sh + 4, 4);
			s->flags = rd(sh + 8, 8);
			s->size = rd(sh + 32, 8);
			s->align = rd(sh + 48, 8);
			s->data = d + rd(sh + 24, 8);
			s->info = rd(sh + 44, 4);
		} else {
			s->name = st ? (char *)st + rd(sh, 4) : "";
			s->type = rd(sh + 4, 4);
			s->flags = rd(sh + 8, 4);
			s->size = rd(sh + 20, 4);
			s->align = rd(sh + 32, 4);
			s->data = d + rd(sh + 16, 4);
			s->info = rd(sh + 28, 4);
		}
		s->out = -1;
		if (s->type != 8 && s->data + s->size > d + len)
			lerr("%s: section %s out of file", path, s->name);
	}
	/* symboly */
	for (n=0; n<shnum; n++) {
		s = &o->sec[n];
		if (s->type != 2)
			continue;
		sh = d + shoff + n * shent;
		link = elf64 ? rd(sh + 40, 4) : rd(sh + 24, 4);
		syment = elf64 ? 24 : 16;
		nsym = s->size / syment;
		st = o->sec[link].data;
		o->nsym = nsym;
		o->sym = emalloc((nsym + 1) * sizeof o->sym[0]);
		for (m=0; m<nsym; m++) {
			uchar *e = s->data + m * syment;
			y = &o->sym[m];
			if (elf64) {
				y->name = (char *)st + rd(e, 4);
				y->bind = e[4] >> 4;
				y->type = e[4] & 15;
				y->shndx = rd(e + 6, 2);
				y->value = rd(e + 8, 8);
				y->size = rd(e + 16, 8);
			} else {
				y->name = (char *)st + rd(e, 4);
				y->value = rd(e + 4, 4);
				y->size = rd(e + 8, 4);
				y->bind = e[12] >> 4;
				y->type = e[12] & 15;
				y->shndx = rd(e + 14, 2);
			}
			if (y->shndx == 0xfff1)
				y->shndx = -1;
			else if (y->shndx == 0xfff2)
				y->shndx = -2;
			else if (y->shndx >= 0xff00)
				lerr("%s: unsupported symbol section index", path);
			y->gsym = -1;
		}
	}
	/* relokace */
	for (n=0; n<shnum; n++) {
		s = &o->sec[n];
		if (s->type != 4 && s->type != 9)
			continue;
		if ((uint)s->info >= shnum)
			lerr("%s: bad relocation section", path);
		rent = s->type == 4 ? (elf64 ? 24 : 12) : (elf64 ? 16 : 8);
		nrel = s->size / rent;
		o->sec[s->info].rel = emalloc((nrel + 1) * sizeof(IRel));
		o->sec[s->info].nrel = nrel;
		o->sec[s->info].rela = s->type == 4;
		for (m=0; m<nrel; m++) {
			uchar *e = s->data + m * rent;
			IRel *r = &o->sec[s->info].rel[m];
			if (elf64) {
				r->off = rd(e, 8);
				r->sym = rd(e + 8, 8) >> 32;
				r->type = rd(e + 8, 4);
				r->add = s->type == 4 ? (int64_t)rd(e + 16, 8) : 0;
			} else {
				r->off = rd(e, 4);
				r->sym = rd(e + 4, 4) >> 8;
				r->type = e[4];
				r->add = s->type == 4 ? (int32_t)rd(e + 8, 4) : 0;
			}
		}
	}
	(void)shsize; (void)size; (void)symoff; (void)symsize; (void)stroff; (void)roff; (void)rsize;
}

/* ---- symboly ---- */

static int
findg(char *name)
{
	uint n;

	for (n=0; n<ngsyms; n++)
		if (strcmp(gsyms[n].name, name) == 0)
			return n;
	return -1;
}

static void
resolve(void)
{
	IObj *o;
	ISym *y;
	GSym *g;
	uint n, m;
	int gi;

	for (n=0; n<nobjs; n++) {
		o = &objs[n];
		for (m=0; m<o->nsym; m++) {
			y = &o->sym[m];
			if (y->bind == 0 || !y->name[0])
				continue;
			gi = findg(y->name);
			if (gi < 0) {
				GROW(gsyms, ngsyms + 1);
				gi = ngsyms++;
				g = &gsyms[gi];
				memset(g, 0, sizeof *g);
				g->name = y->name;
				g->sym = -1;
				g->sec = -1;
			}
			g = &gsyms[gi];
			y->gsym = gi;
			if (y->shndx == 0)
				continue;
			if (y->shndx == -2) {
				/* common: největší velikost, nejsilnější vazba */
				if (g->sym >= 0 && !g->common)
					continue;
				if (y->size > g->common)
					g->common = y->size;
				if (y->value > g->calign)
					g->calign = y->value;
				if (g->sym < 0) {
					g->obj = o;
					g->sym = m;
				}
				continue;
			}
			if (g->sym >= 0 && !g->common) {
				if (y->bind == 2)
					continue;
				if (!g->weak)
					lerr("duplicate symbol %s (%s and %s)", y->name, g->obj->path, o->path);
			}
			g->obj = o;
			g->sym = m;
			g->weak = y->bind == 2;
			g->common = 0;
		}
	}
}

/* ---- výběr sekcí a gc ---- */

static int
outsec(ISec *s)
{
	if (!(s->flags & 2))
		return -1;
	if (s->flags & 0x400)
		lerr("thread-local section %s is not supported", s->name);
	if (s->type == 8)
		return OBss;
	if (s->flags & 4)
		return OText;
	if (s->flags & 1)
		return OData;
	return ORodata;
}

/* sekce symbolu relokace (lokální přes sekci, globální přes tabulku) */
static ISec *
relsec(IObj *o, IRel *r, uint64_t *value)
{
	ISym *y;
	GSym *g;

	if (r->sym >= (uint32_t)o->nsym)
		lerr("%s: bad relocation symbol", o->path);
	y = &o->sym[r->sym];
	if (y->gsym >= 0) {
		g = &gsyms[y->gsym];
		if (g->sym < 0)
			return 0;
		if (g->common) {
			*value = g->value;
			return (ISec *)-1;
		}
		y = &g->obj->sym[g->sym];
		o = g->obj;
	}
	if (y->shndx == -1) {
		*value = y->value;
		return (ISec *)-1;
	}
	if (y->shndx <= 0)
		return 0;
	*value = y->value;
	return &o->sec[y->shndx];
}

static void
mark(ISec *s)
{
	IObj *o;
	ISec *t;
	IRel *r;
	uint n, m;
	uint64_t v;

	if (s->keep)
		return;
	s->keep = 1;
	/* vlastník kvůli symbolům */
	for (n=0; n<nobjs; n++)
		if (s >= objs[n].sec && s < objs[n].sec + objs[n].nsec)
			break;
	o = &objs[n];
	for (m=0; m<s->nrel; m++) {
		r = &s->rel[m];
		t = relsec(o, r, &v);
		if (t && t != (ISec *)-1)
			mark(t);
		else if (!t) {
			ISym *y = &o->sym[r->sym];
			GSym *g = y->gsym >= 0 ? &gsyms[y->gsym] : 0;
			if (!g || (g->sym < 0 && !g->weak))
				lerr("undefined symbol %s (%s)", y->name, o->path);
		}
	}
}

static void
gc(void)
{
	IObj *o;
	ISec *s;
	GSym *g;
	ISym *y;
	uint n, m;
	int gi;

	for (n=0; n<nobjs; n++)
		for (m=0; m<objs[n].nsec; m++)
			objs[n].sec[m].out = outsec(&objs[n].sec[m]);
	gi = findg(entryname);
	if (gi < 0 || gsyms[gi].sym < 0)
		lerr("entry symbol %s is not defined", entryname);
	g = &gsyms[gi];
	y = &g->obj->sym[g->sym];
	if (y->shndx <= 0)
		lerr("entry symbol %s has no section", entryname);
	mark(&g->obj->sec[y->shndx]);
	/* poznámky (SHT_NOTE) alokované se nechávají */
	for (n=0; n<nobjs; n++) {
		o = &objs[n];
		for (m=0; m<o->nsec; m++) {
			s = &o->sec[m];
			if (s->out >= 0 && s->type == 7)
				mark(s);
		}
	}
	/* common symboly: jen použité (přes relokace z živých sekcí) */
	for (n=0; n<nobjs; n++) {
		o = &objs[n];
		for (m=0; m<o->nsec; m++) {
			s = &o->sec[m];
			if (!s->keep)
				continue;
			{
				uint k;
				for (k=0; k<s->nrel; k++) {
					ISym *ys = &o->sym[s->rel[k].sym];
					if (ys->gsym >= 0 && gsyms[ys->gsym].common)
						gsyms[ys->gsym].sec = OBss;
				}
			}
		}
	}
}

/* ---- rozvržení ---- */

static uint64_t
layoutsec(int k, uint64_t addr)
{
	IObj *o;
	ISec *s;
	GSym *g;
	uint n, m;
	uint64_t start;

	start = addr;
	for (n=0; n<nobjs; n++) {
		o = &objs[n];
		for (m=0; m<o->nsec; m++) {
			s = &o->sec[m];
			if (!s->keep || s->out != k)
				continue;
			addr = alignup(addr, s->align);
			if (s->align > osecs[k].align)
				osecs[k].align = s->align;
			s->addr = addr;
			addr += s->size;
		}
	}
	if (k == OBss)
		for (n=0; n<ngsyms; n++) {
			g = &gsyms[n];
			if (!g->common || g->sec < 0)
				continue;
			addr = alignup(addr, g->calign);
			if (g->calign > osecs[k].align)
				osecs[k].align = g->calign;
			g->value = addr;
			addr += g->common;
		}
	osecs[k].size = addr - start;
	return addr;
}

static uint64_t page = 4096;

static void
layout(void)
{
	uint64_t hdr, off, va, k;
	int phnum, i;

	phnum = 5; /* 4 LOAD + GNU_STACK */
	hdr = elf64 ? 64 + 56 * phnum : 52 + 32 * phnum;
	/* .text ve druhé stránce (oddělený kód jako ld), .rodata na další
	 * stránce, .data/.bss na stránce s posunem shodným se souborem */
	off = page;
	va = base + page;
	osecs[OText].off = off;
	osecs[OText].addr = va;
	va = layoutsec(OText, va);
	off += osecs[OText].size;
	off = alignup(off, page);
	va = alignup(va, page);
	osecs[ORodata].off = off;
	osecs[ORodata].addr = va;
	va = layoutsec(ORodata, va);
	off += osecs[ORodata].size;
	/* RW segment: další stránka, posun v souboru dál bez zarovnání na
	 * stránku (vaddr ≡ offset mod stránka) */
	k = alignup(off, 16);
	off = k;
	va = alignup(va, page) + (off % page);
	osecs[OData].off = off;
	osecs[OData].addr = va;
	va = layoutsec(OData, va);
	off += osecs[OData].size;
	osecs[OBss].off = off;
	osecs[OBss].addr = va;
	va = layoutsec(OBss, va);
	for (i=0; i<NOut; i++)
		if (osecs[i].align < 1)
			osecs[i].align = 1;
	(void)hdr;
}

/* ---- relokace ---- */

/* jméno symbolu relokace pro hlášení (u symbolu sekce jméno sekce) */
static char *
relname(IObj *o, ISym *y)
{
	if (y->name[0])
		return y->name;
	if (y->shndx > 0 && (uint)y->shndx < o->nsec)
		return o->sec[y->shndx].name;
	return "(unnamed symbol)";
}

static void
relocate(uchar *out)
{
	IObj *o;
	ISec *s, *t;
	IRel *r;
	ISym *y;
	uint n, m, k;
	uint64_t v, p, a;
	int64_t val;
	uchar *field;

	for (n=0; n<nobjs; n++) {
		o = &objs[n];
		for (m=0; m<o->nsec; m++) {
			s = &o->sec[m];
			if (!s->keep || s->out < 0)
				continue;
			if (s->type != 8)
				memcpy(out + osecs[s->out].off + (s->addr - osecs[s->out].addr), s->data, s->size);
			for (k=0; k<s->nrel; k++) {
				r = &s->rel[k];
				y = &o->sym[r->sym];
				t = relsec(o, r, &v);
				if (!t) {
					if (y->gsym >= 0 && gsyms[y->gsym].weak)
						v = 0;
					else
						lerr("undefined symbol %s (%s)", y->name, o->path);
				} else if (t != (ISec *)-1) {
					if (!t->keep)
						lerr("%s: relocation against discarded section %s", o->path, t->name);
					v += t->addr;
				}
				if (s->type == 8)
					lerr("%s: relocation in a nobits section", o->path);
				field = out + osecs[s->out].off + (s->addr - osecs[s->out].addr) + r->off;
				p = s->addr + r->off;
				a = r->add;
				if (!s->rela)
					a = (int64_t)(int32_t)rd(field, 4);
				if (elf64)
					switch (r->type) {
					case 2:  /* PC32 */
					case 4:  /* PLT32 */
						val = (int64_t)(v + a) - (int64_t)p;
						if (val < INT32_MIN || val > INT32_MAX)
							lerr("%s: relocation out of range for %s", o->path, relname(o, y));
						wr(field, val, 4);
						break;
					case 1:  /* 64 */
					case 10: /* 32 */
					case 11: /* 32S */
						if (pie)
							lerr("%s: absolute relocation against %s in a position-independent executable", o->path, relname(o, y));
						val = v + a;
						if (r->type == 10 && (val < 0 || val > UINT32_MAX))
							lerr("%s: relocation out of range for %s", o->path, relname(o, y));
						if (r->type == 11 && (val < INT32_MIN || val > INT32_MAX))
							lerr("%s: relocation out of range for %s", o->path, relname(o, y));
						wr(field, val, r->type == 1 ? 8 : 4);
						break;
					default:
						lerr("%s: unsupported relocation type %u", o->path, r->type);
					}
				else
					switch (r->type) {
					case 1:  /* R_386_32 */
						if (pie)
							lerr("%s: absolute relocation against %s in a position-independent executable", o->path, relname(o, y));
						wr(field, v + a, 4);
						break;
					case 2:  /* R_386_PC32 */
					case 4:  /* R_386_PLT32 */
						wr(field, (int64_t)(v + a) - (int64_t)p, 4);
						break;
					default:
						lerr("%s: unsupported relocation type %u", o->path, r->type);
					}
			}
		}
	}
}

/* ---- výstup ---- */

static void
phdr(uchar *p, uint32_t type, uint32_t flags, uint64_t off, uint64_t va, uint64_t filesz, uint64_t memsz, uint64_t align)
{
	if (elf64) {
		wr(p, type, 4);
		wr(p + 4, flags, 4);
		wr(p + 8, off, 8);
		wr(p + 16, va, 8);
		wr(p + 24, va, 8);
		wr(p + 32, filesz, 8);
		wr(p + 40, memsz, 8);
		wr(p + 48, align, 8);
	} else {
		wr(p, type, 4);
		wr(p + 4, off, 4);
		wr(p + 8, va, 4);
		wr(p + 12, va, 4);
		wr(p + 16, filesz, 4);
		wr(p + 20, memsz, 4);
		wr(p + 24, flags, 4);
		wr(p + 28, align, 4);
	}
}

static void
shdr(uchar *p, uint32_t name, uint32_t type, uint64_t flags, uint64_t addr, uint64_t off, uint64_t size, uint64_t align)
{
	int w;

	w = elf64 ? 8 : 4;
	wr(p, name, 4);
	wr(p + 4, type, 4);
	wr(p + 8, flags, w);
	wr(p + 8 + w, addr, w);
	wr(p + 8 + 2 * w, off, w);
	wr(p + 8 + 3 * w, size, w);
	wr(p + 8 + 4 * w, 0, 4);
	wr(p + 12 + 4 * w, 0, 4);
	wr(p + 16 + 4 * w, align, w);
	wr(p + 16 + 5 * w, 0, w);
}

static void
writeexe(char *out)
{
	uchar *buf, *p;
	FILE *f;
	uint64_t total, shoff, entry, n, end;
	int phnum, phent, shent, ehsz, i, gi;
	GSym *g;
	ISym *y;
	static char shstr[] = "\0.text\0.rodata\0.data\0.bss\0.shstrtab";
	uint32_t names[NOut] = {1, 7, 15, 21};

	phnum = 5;
	phent = elf64 ? 56 : 32;
	shent = elf64 ? 64 : 40;
	ehsz = elf64 ? 64 : 52;
	end = osecs[OData].off + osecs[OData].size;
	shoff = alignup(end + sizeof shstr, 8);
	total = shoff + (NOut + 2) * shent;
	buf = emalloc(total);
	memset(buf, 0, total);
	relocate(buf);
	/* vstupní bod */
	gi = findg(entryname);
	g = &gsyms[gi];
	y = &g->obj->sym[g->sym];
	entry = g->obj->sec[y->shndx].addr + y->value;
	/* hlavička */
	memcpy(buf, "\177ELF", 4);
	buf[4] = elf64 ? 2 : 1;
	buf[5] = 1;
	buf[6] = 1;
	wr(buf + 16, pie ? 3 : 2, 2);
	wr(buf + 18, machine, 2);
	wr(buf + 20, 1, 4);
	if (elf64) {
		wr(buf + 24, entry, 8);
		wr(buf + 32, 64, 8);
		wr(buf + 40, shoff, 8);
		wr(buf + 52, 64, 2);
		wr(buf + 54, phent, 2);
		wr(buf + 56, phnum, 2);
		wr(buf + 58, shent, 2);
		wr(buf + 60, NOut + 2, 2);
		wr(buf + 62, NOut + 1, 2);
	} else {
		wr(buf + 24, entry, 4);
		wr(buf + 28, 52, 4);
		wr(buf + 32, shoff, 4);
		wr(buf + 40, 52, 2);
		wr(buf + 42, phent, 2);
		wr(buf + 44, phnum, 2);
		wr(buf + 46, shent, 2);
		wr(buf + 48, NOut + 2, 2);
		wr(buf + 50, NOut + 1, 2);
	}
	p = buf + ehsz;
	phdr(p, 1, 4, 0, base, ehsz + phnum * phent, ehsz + phnum * phent, page);
	p += phent;
	phdr(p, 1, 5, osecs[OText].off, osecs[OText].addr, osecs[OText].size, osecs[OText].size, page);
	p += phent;
	phdr(p, 1, 4, osecs[ORodata].off, osecs[ORodata].addr, osecs[ORodata].size, osecs[ORodata].size, page);
	p += phent;
	phdr(p, 1, 6, osecs[OData].off, osecs[OData].addr, osecs[OData].size,
		osecs[OBss].addr + osecs[OBss].size - osecs[OData].addr, page);
	p += phent;
	phdr(p, 0x6474e551, 6, 0, 0, 0, 0, 16);
	/* hlavičky sekcí */
	memcpy(buf + end, shstr, sizeof shstr);
	p = buf + shoff;
	shdr(p, 0, 0, 0, 0, 0, 0, 0);
	for (i=0; i<NOut; i++) {
		p += shent;
		shdr(p, names[i], osecs[i].nobits ? 8 : 1, osecs[i].flags, osecs[i].addr, osecs[i].off, osecs[i].size, osecs[i].align);
	}
	p += shent;
	shdr(p, 26, 3, 0, 0, end, sizeof shstr, 1);
	f = fopen(out, "wb");
	if (!f)
		lerr("cannot create %s", out);
	for (n=0; n<total; n++)
		fputc(buf[n], f);
	fclose(f);
	free(buf);
	luxir_chmod_exe(out);
}

int
link(char **inputs, int ninputs, char *out, int is64, int mach, int ispie, char *entry)
{
	int n;

	elf64 = is64;
	machine = mach;
	pie = ispie;
	entryname = entry ? entry : "_start";
	base = pie ? 0 : (elf64 ? 0x400000 : 0x08048000);
	for (n=0; n<ninputs; n++)
		readobj(inputs[n]);
	resolve();
	gc();
	layout();
	writeexe(out);
	return 0;
}

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
 * nezapisuje (strip), zůstávají hlavičky sekcí pro nástroje.
 *
 * Režim bare metal (-b) nahrazuje linker skript runtime LSL pro desku
 * (runtime/arch/baremetal_riscv64.ld v LSL): ET_EXEC od RAM 0x80000000,
 * nejdřív .text.start, sekce na hranici stránky kvůli oprávněním
 * stránkování, za .bss zásobníky pastí hartů a hlavní zásobník se
 * strážními stránkami a symboly rozložení, které skript definuje
 * (lsl_text_end, __bss_start, lsl_stack_top, lsl_image_end, …). Hlavičky
 * se nenahrávají (pod RAM nic není).
 *
 * Dynamický program (vstup .libs se jmény knihoven, jen x86-64): PIE
 * s PT_INTERP /lib64/ld-linux-x86-64.so.2 a PT_DYNAMIC; nedefinované
 * symboly jsou importy z knihoven (DT_NEEDED), volání jdou přes stub
 * v .plt (jmp *GOT) a slot v .got, který ld.so naváže při startu
 * (BIND_NOW, relokace GLOB_DAT); tabulky .dynsym, .dynstr, .hash,
 * .rela.dyn a .dynamic leží v zapisovatelném segmentu za .data. Žádný
 * symbol se nevyváží, absolutní relokace zůstávají chybou. Tak LSL
 * volá systémové knihovny (glibc, GTK) bez libc ve vlastním runtime. */

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
	int linkdef;     /* symbol rozložení definovaný linkerem (bare metal) */
	int imp;         /* import z knihovny (dynamický program) nebo -1 */
};

static IObj *objs;
static uint nobjs;
static GSym *gsyms;
static uint ngsyms;
static int elf64, machine, pie, bare;

/* dynamický program */
static int dynamic;
static char **dynlibs;
static uint ndynlibs;
static int *impsyms;        /* importy: index v gsyms */
static uint nimps;
static uint64_t pltaddr, interpaddr, gotaddr, dynsymaddr, dynstraddr, hashaddr, reladdr, dynaddr;
static uint64_t dynstrsz, dynsz;
static char *dynstr;
static char *interp = "/lib64/ld-linux-x86-64.so.2";
enum { PltEntry = 16 };
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

/* bare metal: sekce bez vstupu za .bss a symboly rozložení */
enum {
	BareHarts = 8,
	BareTrapStack = 20480,   /* 4 KiB stráž + 16 KiB na hart */
	BareStack = 0x800000,    /* hlavní zásobník 8 MiB pod strážní stránkou */
};
static const uint64_t BareRam = 0x80000000;
static OSec baresecs[2] = {
	{".trapstacks", 3, 1, 0, 0, 0, 4096},
	{".stack", 3, 1, 0, 0, 0, 4096},
};
static char *barenames[] = {
	"lsl_text_end", "lsl_rodata_end", "__bss_start", "__bss_end",
	"lsl_trap_stacks", "lsl_stack_guard", "lsl_stack_top", "lsl_image_end",
};
enum { NBareSym = sizeof barenames / sizeof barenames[0] };
static uint64_t barevals[NBareSym];

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

/* .libs: jména sdílených knihoven (soname) po řádcích; komentáře '#' */
static void
readlibs(char *path)
{
	FILE *f;
	char *d, *p, *e;
	long cap, len, got;

	f = fopen(path, "rb");
	if (!f)
		lerr("cannot open %s", path);
	cap = 4096;
	d = emalloc(cap);
	len = 0;
	for (;;) {
		if (len + 1024 > cap) {
			cap *= 2;
			d = realloc(d, cap);
			if (!d)
				die("out of memory");
		}
		got = fread(d + len, 1, cap - len - 1, f);
		if (got == 0)
			break;
		len += got;
	}
	fclose(f);
	d[len] = 0;
	dynamic = 1;
	p = d;
	while (*p) {
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		if (*p == '#') {
			while (*p && *p != '\n')
				p++;
			continue;
		}
		e = p;
		while (*e && *e != '#' && *e != '\n' && *e != '\r' && *e != ' ' && *e != '\t')
			e++;
		if (e > p) {
			GROW(dynlibs, ndynlibs + 1);
			dynlibs[ndynlibs] = emalloc(e - p + 1);
			memcpy(dynlibs[ndynlibs], p, e - p);
			dynlibs[ndynlibs][e - p] = 0;
			ndynlibs++;
		}
		p = e;
	}
}

/* ---- importy: ověření proti sdíleným knihovnám hostitele ---- */

/* definované symboly knihovny (SHT_DYNSYM, shndx != 0); 0 = knihovna není
 * na hostiteli (křížový překlad: kontrola se přeskočí) */
static char **
libsyms(char *soname, uint *count)
{
	static char *dirs[] = {"/lib64/", "/usr/lib64/", "/usr/lib/x86_64-linux-gnu/", "/lib/x86_64-linux-gnu/", "/usr/lib/", "/lib/", "/usr/local/lib/", 0};
	char path[1024], **names;
	uchar *d, *sh, *st;
	FILE *f;
	long cap, len, got;
	uint64_t shoff, off, size, stroff, k, n;
	uint shnum, shent, link, nn;
	int i;

	*count = 0;
	f = 0;
	for (i=0; dirs[i]; i++) {
		if (strlen(dirs[i]) + strlen(soname) + 1 > sizeof path)
			continue;
		memcpy(path, dirs[i], strlen(dirs[i]));
		memcpy(path + strlen(dirs[i]), soname, strlen(soname) + 1);
		f = fopen(path, "rb");
		if (f)
			break;
	}
	if (!f)
		return 0;
	cap = 1 << 20;
	d = emalloc(cap);
	len = 0;
	for (;;) {
		if (len + 65536 > cap) {
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
	if (len < 64 || memcmp(d, "\177ELF", 4) != 0 || d[4] != 2) {
		free(d);
		return 0;
	}
	shoff = rd(d + 40, 8);
	shent = rd(d + 58, 2);
	shnum = rd(d + 60, 2);
	names = 0;
	nn = 0;
	for (k=0; k<shnum; k++) {
		sh = d + shoff + k * shent;
		if (rd(sh + 4, 4) != 11)      /* SHT_DYNSYM */
			continue;
		off = rd(sh + 24, 8);
		size = rd(sh + 32, 8);
		link = rd(sh + 40, 4);
		stroff = rd(d + shoff + link * shent + 24, 8);
		st = d + stroff;
		for (n=0; n<size/24; n++) {
			uchar *e = d + off + n * 24;
			if (rd(e + 6, 2) == 0)    /* nedefinovaný */
				continue;
			GROW(names, nn + 1);
			names[nn++] = (char *)st + rd(e, 4);
		}
		break;
	}
	*count = nn;
	return names;   /* d zůstává: jména do něj ukazují */
}

static int
symin(char **names, uint count, char *name)
{
	uint n;

	for (n=0; n<count; n++)
		if (strcmp(names[n], name) == 0)
			return 1;
	return 0;
}

/* každý import musí definovat některá z knihoven .libs (je-li na
 * hostiteli); jinak by program spadl až při startu v ld.so */
static void
checkimports(void)
{
	char ***syms;
	uint *counts, n, k;
	int known;

	syms = emalloc((ndynlibs + 1) * sizeof syms[0]);
	counts = emalloc((ndynlibs + 1) * sizeof counts[0]);
	known = 0;
	for (n=0; n<ndynlibs; n++) {
		syms[n] = libsyms(dynlibs[n], &counts[n]);
		if (syms[n])
			known = 1;
	}
	if (!known)
		return;
	for (k=0; k<nimps; k++) {
		for (n=0; n<ndynlibs; n++)
			if (syms[n] && symin(syms[n], counts[n], gsyms[impsyms[k]].name))
				break;
		if (n == ndynlibs)
			lerr("undefined symbol %s (not in %s%s)", gsyms[impsyms[k]].name, dynlibs[0], ndynlibs > 1 ? ", ..." : "");
	}
}

/* ---- symboly ---- */

/* hašovací tabulka globálních jmen: index+1 do gsyms */
static uint *ghash;
static uint ghashcap;

static uint
gnamehash(char *s)
{
	uint h;

	h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (uchar)*s) * 16777619u;
	return h;
}

static void
grehash(void)
{
	uint n, i, cap;

	cap = ghashcap ? 2 * ghashcap : 4096;
	free(ghash);
	ghash = emalloc(cap * sizeof ghash[0]);
	memset(ghash, 0, cap * sizeof ghash[0]);
	ghashcap = cap;
	for (n=0; n<ngsyms; n++) {
		i = gnamehash(gsyms[n].name) & (cap - 1);
		while (ghash[i])
			i = (i + 1) & (cap - 1);
		ghash[i] = n + 1;
	}
}

static int
findg(char *name)
{
	uint i;

	if (!ghashcap)
		grehash();
	i = gnamehash(name) & (ghashcap - 1);
	while (ghash[i]) {
		if (strcmp(gsyms[ghash[i] - 1].name, name) == 0)
			return ghash[i] - 1;
		i = (i + 1) & (ghashcap - 1);
	}
	return -1;
}

/* nový globální symbol (jméno ještě v tabulce není) */
static int
addg(char *name)
{
	GSym *g;
	uint i;

	GROW(gsyms, ngsyms + 1);
	g = &gsyms[ngsyms];
	memset(g, 0, sizeof *g);
	g->name = name;
	g->sym = -1;
	g->sec = -1;
	g->imp = -1;
	ngsyms++;
	if (2 * ngsyms > ghashcap)
		grehash();
	else {
		i = gnamehash(name) & (ghashcap - 1);
		while (ghash[i])
			i = (i + 1) & (ghashcap - 1);
		ghash[i] = ngsyms;
	}
	return ngsyms - 1;
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
			if (gi < 0)
				gi = addg(y->name);
			g = &gsyms[gi];
			g->imp = -1;
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
	if (bare)
		for (n=0; n<ngsyms; n++)
			for (m=0; m<NBareSym; m++)
				if (gsyms[n].sym < 0 && strcmp(gsyms[n].name, barenames[m]) == 0)
					gsyms[n].linkdef = 1;
}

/* ---- výběr sekcí a gc ---- */

static int
outsec(ISec *s)
{
	if (!(s->flags & 2))
		return -1;
	if (s->flags & 0x400)
		lerr("thread-local section %s is not supported", s->name);
	/* /DISCARD/ linker skriptu bare metal */
	if (bare && (strcmp(s->name, ".eh_frame") == 0 || strcmp(s->name, ".note.GNU-stack") == 0))
		return -1;
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
		if (g->sym < 0 && g->linkdef) {
			*value = g->value;
			return (ISec *)-1;
		}
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
			if (g && g->sym < 0 && !g->weak && dynamic) {
				/* import z knihovny: slot GOT a stub PLT */
				if (g->imp < 0) {
					GROW(impsyms, nimps + 1);
					impsyms[nimps] = y->gsym;
					g->imp = nimps++;
				}
				continue;
			}
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
	int pass, first;

	start = addr;
	/* bare metal: .text.start na začátek obrazu (KEEP(*(.text.start))) */
	first = bare && k == OText;
	for (pass=!first; pass<2; pass++)
	for (n=0; n<nobjs; n++) {
		o = &objs[n];
		for (m=0; m<o->nsec; m++) {
			s = &o->sec[m];
			if (!s->keep || s->out != k)
				continue;
			if (first && (strcmp(s->name, ".text.start") == 0) != (pass == 0))
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

/* dynamický program: stuby PLT na konci kódu */
static uint64_t
layoutplt(uint64_t va)
{
	if (!dynamic)
		return va;
	va = alignup(va, 16);
	pltaddr = va;
	va += PltEntry * (uint64_t)nimps;
	osecs[OText].size = va - osecs[OText].addr;
	return va;
}

/* dynamický program: za .data sloty GOT, .dynsym, .dynstr, .hash,
 * .rela.dyn a .dynamic (zapisovatelný segment: ld.so plní GOT a DT_DEBUG) */
static uint64_t
layoutdynamic(uint64_t va)
{
	uint n;
	uint64_t len;
	char *name;

	va = alignup(va, 8);
	gotaddr = va;
	va += 8 * (uint64_t)nimps;
	dynsymaddr = va;
	va += 24 * ((uint64_t)nimps + 1);
	/* .dynstr: prázdné jméno, importy, knihovny */
	len = 1;
	for (n=0; n<nimps; n++)
		len += strlen(gsyms[impsyms[n]].name) + 1;
	for (n=0; n<ndynlibs; n++)
		len += strlen(dynlibs[n]) + 1;
	dynstr = emalloc(len + 1);
	memset(dynstr, 0, len + 1);
	len = 1;
	for (n=0; n<nimps; n++) {
		name = gsyms[impsyms[n]].name;
		memcpy(dynstr + len, name, strlen(name));
		len += strlen(name) + 1;
	}
	for (n=0; n<ndynlibs; n++) {
		memcpy(dynstr + len, dynlibs[n], strlen(dynlibs[n]));
		len += strlen(dynlibs[n]) + 1;
	}
	dynstrsz = len;
	dynstraddr = va;
	va = alignup(va + len, 8);
	hashaddr = va;
	va += 4 * (2 + 1 + ((uint64_t)nimps + 1));
	va = alignup(va, 8);
	reladdr = va;
	va += 24 * (uint64_t)nimps;
	dynaddr = va;
	/* NEEDED×n, HASH, STRTAB, SYMTAB, STRSZ, SYMENT, RELA, RELASZ, RELAENT,
	 * FLAGS, FLAGS_1, DEBUG, NULL */
	dynsz = 16 * ((uint64_t)ndynlibs + 12);
	va += dynsz;
	osecs[OData].size = va - osecs[OData].addr;
	return va;
}

/* největší zarovnání vstupu výstupní sekce (začátek výstupní sekce jako ld) */
static uint64_t
maxalign(int k)
{
	uint n, m;
	uint64_t a;

	a = 1;
	for (n=0; n<nobjs; n++)
		for (m=0; m<objs[n].nsec; m++)
			if (objs[n].sec[m].keep && objs[n].sec[m].out == k && objs[n].sec[m].align > a)
				a = objs[n].sec[m].align;
	if (k == OBss)
		for (n=0; n<ngsyms; n++)
			if (gsyms[n].common && gsyms[n].sec >= 0 && gsyms[n].calign > a)
				a = gsyms[n].calign;
	return a;
}

/* bare metal: rozložení podle linker skriptu desky; v souboru leží obsah
 * od druhé stránky s posunem adresa - RAM + 4096 (hlavičky v první) */
static void
layoutbare(void)
{
	uint64_t va;
	uint n, m;
	int i;

	va = BareRam;
	osecs[OText].addr = va;
	va = layoutsec(OText, va);
	va = alignup(va, 4096);
	barevals[0] = va;                       /* lsl_text_end */
	osecs[ORodata].addr = va;
	va = layoutsec(ORodata, va);
	va = alignup(va, 4096);
	barevals[1] = va;                       /* lsl_rodata_end */
	va = alignup(va, maxalign(OData));
	osecs[OData].addr = va;
	va = layoutsec(OData, va);
	va = alignup(va, maxalign(OBss) > 16 ? maxalign(OBss) : 16);
	osecs[OBss].addr = va;
	barevals[2] = va;                       /* __bss_start */
	va = layoutsec(OBss, va);
	va = alignup(va, 16);
	osecs[OBss].size = va - osecs[OBss].addr;
	barevals[3] = va;                       /* __bss_end */
	va = alignup(va, 4096);
	baresecs[0].addr = va;
	barevals[4] = va;                       /* lsl_trap_stacks */
	va += BareHarts * BareTrapStack;
	baresecs[0].size = va - baresecs[0].addr;
	va = alignup(va, 4096);
	baresecs[1].addr = va;
	barevals[5] = va;                       /* lsl_stack_guard */
	va += 4096 + BareStack;
	baresecs[1].size = va - baresecs[1].addr;
	barevals[6] = va;                       /* lsl_stack_top */
	barevals[7] = va;                       /* lsl_image_end */
	for (i=OText; i<=OData; i++)
		osecs[i].off = osecs[i].addr - BareRam + 4096;
	osecs[OBss].off = osecs[OData].off + osecs[OData].size;
	baresecs[0].off = baresecs[1].off = osecs[OBss].off;
	for (n=0; n<ngsyms; n++)
		if (gsyms[n].linkdef)
			for (m=0; m<NBareSym; m++)
				if (strcmp(gsyms[n].name, barenames[m]) == 0)
					gsyms[n].value = barevals[m];
	for (i=0; i<NOut; i++)
		if (osecs[i].align < 1)
			osecs[i].align = 1;
}

static void
layout(void)
{
	uint64_t hdr, off, va, k;
	int phnum, i;

	phnum = 5 + (dynamic ? 3 : 0); /* 4 LOAD + GNU_STACK (+ PHDR, INTERP, DYNAMIC) */
	hdr = elf64 ? 64 + 56 * phnum : 52 + 32 * phnum;
	if (page <= 4096) {
		/* .text ve druhé stránce (oddělený kód jako ld), .rodata na další
		 * stránce, .data/.bss na stránce s posunem shodným se souborem */
		off = page;
		va = base + page;
		osecs[OText].off = off;
		osecs[OText].addr = va;
		va = layoutsec(OText, va);
		va = layoutplt(va);
		off += osecs[OText].size;
		off = alignup(off, page);
		va = alignup(va, page);
	} else {
		/* velká stránka (arm64: jádra se 16 nebo 64 KiB): bez výplně
		 * v souboru, každý segment na další stránce s adresou shodnou
		 * s posunem v souboru modulo stránka, jako GNU ld */
		off = alignup(hdr, 16);
		va = alignup(base + hdr, page) + off % page;
		osecs[OText].off = off;
		osecs[OText].addr = va;
		va = layoutsec(OText, va);
		va = layoutplt(va);
		off += osecs[OText].size;
		off = alignup(off, 16);
		va = alignup(va, page) + off % page;
	}
	osecs[ORodata].off = off;
	osecs[ORodata].addr = va;
	va = layoutsec(ORodata, va);
	if (dynamic) {
		/* řetězec interpretu za daty jen pro čtení */
		interpaddr = va;
		va += strlen(interp) + 1;
		osecs[ORodata].size = va - osecs[ORodata].addr;
	}
	off += osecs[ORodata].size;
	/* RW segment: další stránka, posun v souboru dál bez zarovnání na
	 * stránku (vaddr ≡ offset mod stránka) */
	k = alignup(off, 16);
	off = k;
	va = alignup(va, page) + (off % page);
	osecs[OData].off = off;
	osecs[OData].addr = va;
	va = layoutsec(OData, va);
	if (dynamic)
		va = layoutdynamic(va);
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

/* hodnota symbolu relokace po rozvržení (0 pro slabý nedefinovaný) */
static uint64_t
relvalue(IObj *o, IRel *r)
{
	ISec *t;
	ISym *y;
	uint64_t v;

	y = &o->sym[r->sym];
	t = relsec(o, r, &v);
	if (!t) {
		if (y->gsym >= 0 && gsyms[y->gsym].imp >= 0)
			return pltaddr + PltEntry * gsyms[y->gsym].imp;
		if (y->gsym >= 0 && gsyms[y->gsym].weak)
			return 0;
		lerr("undefined symbol %s (%s)", y->name, o->path);
	} else if (t != (ISec *)-1) {
		if (!t->keep)
			lerr("%s: relocation against discarded section %s", o->path, t->name);
		v += t->addr;
	}
	return v;
}

/* RISC-V: pole větve a skoku */
static uint32_t
rvbimm(int64_t v)
{
	return ((uint32_t)((v >> 12) & 1) << 31) | ((uint32_t)((v >> 5) & 0x3f) << 25)
		| ((uint32_t)((v >> 1) & 0xf) << 8) | ((uint32_t)((v >> 11) & 1) << 7);
}

static uint32_t
rvjimm(int64_t v)
{
	return ((uint32_t)((v >> 20) & 1) << 31) | ((uint32_t)((v >> 1) & 0x3ff) << 21)
		| ((uint32_t)((v >> 11) & 1) << 20) | ((uint32_t)((v >> 12) & 0xff) << 12);
}

/* RISC-V %pcrel_lo: hodnota relokace PCREL_HI20 na adrese addr téže
 * sekce (symbol .L0 u auipc); vrací S + A - P té relokace */
static int64_t
rvpcrelhi(IObj *o, ISec *s, uint k, uint64_t addr)
{
	IRel *h;
	uint64_t off;
	int64_t d;
	uint m;

	if (addr < s->addr || addr >= s->addr + s->size)
		lerr("%s: %%pcrel_lo refers outside its section", o->path);
	off = addr - s->addr;
	h = 0;
	for (m=k; m-- > 0; )
		if (s->rel[m].off == off && (s->rel[m].type == 23 || s->rel[m].type == 18 || s->rel[m].type == 19)) {
			h = &s->rel[m];
			break;
		}
	if (!h)
		for (m=k; m<s->nrel; m++)
			if (s->rel[m].off == off && (s->rel[m].type == 23 || s->rel[m].type == 18 || s->rel[m].type == 19)) {
				h = &s->rel[m];
				break;
			}
	if (!h)
		lerr("%s: %%pcrel_lo without a matching auipc relocation", o->path);
	d = (int64_t)(relvalue(o, h) + h->add) - (int64_t)addr;
	return d;
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
					if (y->gsym >= 0 && gsyms[y->gsym].imp >= 0)
						v = pltaddr + PltEntry * gsyms[y->gsym].imp;
					else if (y->gsym >= 0 && gsyms[y->gsym].weak)
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
				if (machine == 40) {
					/* ARM: objekty REL, addend v poli podle typu */
					uint32_t ins = rd(field, 4);
					int64_t sa, d;
					switch (r->type) {
					case 28: case 29:
						a = (int64_t)(((int32_t)(ins << 8)) >> 8) << 2;
						break;
					case 43: case 44:
						a = (int64_t)(int16_t)((((ins >> 16) & 0xf) << 12) | (ins & 0xfff));
						break;
					case 42:
						a = (int64_t)(((int32_t)(ins << 1)) >> 1);
						break;
					default:
						a = (int64_t)(int32_t)ins;
					}
					sa = (int64_t)(v + a);
					switch (r->type) {
					case 2: /* ABS32 */
						if (pie)
							lerr("%s: absolute relocation against %s in a position-independent executable", o->path, relname(o, y));
						wr(field, sa, 4);
						continue;
					case 3: /* REL32 */
						wr(field, sa - (int64_t)p, 4);
						continue;
					case 42: /* PREL31 */
						wr(field, ((sa - (int64_t)p) & 0x7fffffff) | (ins & 0x80000000), 4);
						continue;
					case 28: /* CALL */
					case 29: /* JUMP24 */
						d = sa - (int64_t)p;
						if ((d & 3) || d < -(1 << 25) || d >= (1 << 25))
							lerr("%s: branch out of range for %s", o->path, relname(o, y));
						wr(field, (ins & 0xff000000) | ((uint32_t)(d >> 2) & 0xffffff), 4);
						continue;
					case 43: /* MOVW_ABS_NC */
					case 44: /* MOVT_ABS */
						if (pie)
							lerr("%s: absolute relocation against %s in a position-independent executable", o->path, relname(o, y));
						d = r->type == 43 ? (sa & 0xffff) : ((sa >> 16) & 0xffff);
						wr(field, (ins & 0xfff0f000) | ((uint32_t)(d >> 12) << 16) | ((uint32_t)d & 0xfff), 4);
						continue;
					default:
						lerr("%s: unsupported relocation type %u", o->path, r->type);
					}
				}
				if (machine == 243) {
					uint32_t ins = rd(field, 4);
					int64_t sa = (int64_t)(v + a), d, hi, lo;
					switch (r->type) {
					case 1: /* 32 */
					case 2: /* 64 */
						if (pie)
							lerr("%s: absolute relocation against %s in a position-independent executable", o->path, relname(o, y));
						wr(field, sa, r->type == 2 ? 8 : 4);
						continue;
					case 16: /* BRANCH */
						d = sa - (int64_t)p;
						if ((d & 1) || d < -4096 || d >= 4096)
							lerr("%s: branch out of range for %s", o->path, relname(o, y));
						ins = (ins & 0x01fff07f) | rvbimm(d);
						break;
					case 17: /* JAL */
						d = sa - (int64_t)p;
						if ((d & 1) || d < -(1 << 20) || d >= (1 << 20))
							lerr("%s: jump out of range for %s", o->path, relname(o, y));
						ins = (ins & 0xfff) | rvjimm(d);
						break;
					case 18: /* CALL */
					case 19: /* CALL_PLT */
					case 23: /* PCREL_HI20 */
						d = sa - (int64_t)p;
						hi = (d + 0x800) >> 12;
						lo = d - (hi << 12);
						if (hi < -(1 << 19) || hi >= (1 << 19))
							lerr("%s: pc-relative address out of range for %s", o->path, relname(o, y));
						ins = (ins & 0xfff) | ((uint32_t)(hi & 0xfffff) << 12);
						if (r->type != 23) {
							uint32_t jalr = rd(field + 4, 4);
							wr(field + 4, (jalr & 0xfffff) | ((uint32_t)(lo & 0xfff) << 20), 4);
						}
						break;
					case 24: /* PCREL_LO12_I */
					case 25: /* PCREL_LO12_S */
						d = rvpcrelhi(o, s, k, sa);
						hi = (d + 0x800) >> 12;
						lo = d - (hi << 12);
						if (r->type == 24)
							ins = (ins & 0xfffff) | ((uint32_t)(lo & 0xfff) << 20);
						else
							ins = (ins & 0x01fff07f) | ((uint32_t)((lo >> 5) & 0x7f) << 25) | ((uint32_t)(lo & 0x1f) << 7);
						break;
					case 33: case 34: case 35: case 36: /* ADD8/16/32/64 */
					case 37: case 38: case 39: case 40: /* SUB8/16/32/64 */
					case 54: case 55: case 56: {         /* SET8/16/32 */
						/* rozdíly návěští (tabulky skoků, .eh_frame):
						 * pole += / -= / = S + A */
						int sz = r->type == 33 || r->type == 37 || r->type == 54 ? 1
							: r->type == 34 || r->type == 38 || r->type == 55 ? 2
							: r->type == 35 || r->type == 39 || r->type == 56 ? 4 : 8;
						int64_t cur = (int64_t)rd(field, sz);
						if (r->type <= 36)
							cur += sa;
						else if (r->type <= 40)
							cur -= sa;
						else
							cur = sa;
						wr(field, cur, sz);
						continue;
					}
					case 52: /* SUB6 */
						wr(field, (rd(field, 1) & 0xc0) | (((rd(field, 1) & 0x3f) - sa) & 0x3f), 1);
						continue;
					case 53: /* SET6 */
						wr(field, (rd(field, 1) & 0xc0) | (sa & 0x3f), 1);
						continue;
					case 57: /* 32_PCREL */
						wr(field, sa - (int64_t)p, 4);
						continue;
					case 26: /* HI20 */
					case 27: /* LO12_I */
					case 28: /* LO12_S */
						if (pie)
							lerr("%s: absolute relocation against %s in a position-independent executable", o->path, relname(o, y));
						hi = (sa + 0x800) >> 12;
						lo = sa - (hi << 12);
						if (sa != (int64_t)(int32_t)sa)
							lerr("%s: absolute address out of range for %s", o->path, relname(o, y));
						if (r->type == 26)
							ins = (ins & 0xfff) | ((uint32_t)(hi & 0xfffff) << 12);
						else if (r->type == 27)
							ins = (ins & 0xfffff) | ((uint32_t)(lo & 0xfff) << 20);
						else
							ins = (ins & 0x01fff07f) | ((uint32_t)((lo >> 5) & 0x7f) << 25) | ((uint32_t)(lo & 0x1f) << 7);
						break;
					default:
						lerr("%s: unsupported relocation type %u", o->path, r->type);
					}
					wr(field, ins, 4);
					continue;
				}
				if (machine == 183) {
					uint32_t ins = rd(field, 4);
					int64_t sa = (int64_t)(v + a);
					switch (r->type) {
					case 257: /* ABS64 */
					case 258: /* ABS32 */
						if (pie)
							lerr("%s: absolute relocation against %s in a position-independent executable", o->path, relname(o, y));
						wr(field, sa, r->type == 257 ? 8 : 4);
						continue;
					case 260: /* PREL64 */
						wr(field, sa - (int64_t)p, 8);
						continue;
					case 261: /* PREL32 */
						wr(field, sa - (int64_t)p, 4);
						continue;
					case 275: /* ADR_PREL_PG_HI21 */
						val = ((sa >> 12) - ((int64_t)p >> 12));
						if (val < -(1 << 20) || val >= (1 << 20))
							lerr("%s: adrp out of range for %s", o->path, relname(o, y));
						ins = (ins & 0x9f00001f) | ((uint32_t)(val & 3) << 29) | ((uint32_t)((val >> 2) & 0x7ffff) << 5);
						break;
					case 277: /* ADD_ABS_LO12_NC */
						ins = (ins & 0xffc003ff) | ((uint32_t)(sa & 0xfff) << 10);
						break;
					case 278: case 284: case 285: case 286: case 299: {
						int sh = r->type == 278 ? 0 : r->type == 284 ? 1 : r->type == 285 ? 2 : r->type == 286 ? 3 : 4;
						if (sa & ((1 << sh) - 1))
							lerr("%s: misaligned load/store offset for %s", o->path, relname(o, y));
						ins = (ins & 0xffc003ff) | ((uint32_t)(((sa & 0xfff) >> sh) & 0xfff) << 10);
						break;
					}
					case 279: /* TSTBR14 */
						val = (sa - (int64_t)p) >> 2;
						if (val < -(1 << 13) || val >= (1 << 13))
							lerr("%s: branch out of range for %s", o->path, relname(o, y));
						ins = (ins & 0xfff8001f) | ((uint32_t)(val & 0x3fff) << 5);
						break;
					case 280: /* CONDBR19 */
						val = (sa - (int64_t)p) >> 2;
						if (val < -(1 << 18) || val >= (1 << 18))
							lerr("%s: branch out of range for %s", o->path, relname(o, y));
						ins = (ins & 0xff00001f) | ((uint32_t)(val & 0x7ffff) << 5);
						break;
					case 282: /* JUMP26 */
					case 283: /* CALL26 */
						val = (sa - (int64_t)p) >> 2;
						if (val < -(1 << 25) || val >= (1 << 25))
							lerr("%s: branch out of range for %s", o->path, relname(o, y));
						ins = (ins & 0xfc000000) | (uint32_t)(val & 0x3ffffff);
						break;
					default:
						lerr("%s: unsupported relocation type %u", o->path, r->type);
					}
					wr(field, ins, 4);
					continue;
				}
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

/* hlavička sekce s odkazem (sh_link), sh_info a velikostí položky */
static void
shdrx(uchar *p, uint32_t name, uint32_t type, uint64_t flags, uint64_t addr, uint64_t off, uint64_t size, uint64_t align, uint32_t link, uint32_t info, uint64_t entsize)
{
	int w;

	w = elf64 ? 8 : 4;
	shdr(p, name, type, flags, addr, off, size, align);
	wr(p + 8 + 4 * w, link, 4);
	wr(p + 12 + 4 * w, info, 4);
	wr(p + 16 + 5 * w, entsize, w);
}

/* dynamický program: obsah PLT, GOT, tabulek ld.so a .dynamic */
static void
writedynamic(uchar *buf)
{
	uchar *p, *b;
	uint n;
	uint64_t off, s;
	char *name;

	memcpy(buf + osecs[ORodata].off + (interpaddr - osecs[ORodata].addr), interp, strlen(interp) + 1);
	b = buf + osecs[OData].off - osecs[OData].addr;   /* adresa -> soubor v .data */
	for (n=0; n<nimps; n++) {
		/* stub: jmp *GOT(%rip); výplň int3 */
		p = buf + osecs[OText].off + (pltaddr + PltEntry * n - osecs[OText].addr);
		p[0] = 0xff;
		p[1] = 0x25;
		wr(p + 2, (int64_t)(gotaddr + 8 * n) - (int64_t)(pltaddr + PltEntry * n + 6), 4);
		memset(p + 6, 0xcc, PltEntry - 6);
		/* .dynsym: nedefinovaná funkce GLOBAL */
		p = b + dynsymaddr + 24 * (n + 1);
		off = 1;
		for (s=0; s<n; s++)
			off += strlen(gsyms[impsyms[s]].name) + 1;
		wr(p, off, 4);
		p[4] = 0x12;
		/* .rela.dyn: GLOB_DAT do slotu GOT */
		p = b + reladdr + 24 * n;
		wr(p, gotaddr + 8 * n, 8);
		wr(p + 8, ((uint64_t)(n + 1) << 32) | 6, 8);
		wr(p + 16, 0, 8);
	}
	memcpy(b + dynstraddr, dynstr, dynstrsz);
	/* .hash: jeden koš, nic v něm (program nic nevyváží) */
	p = b + hashaddr;
	wr(p, 1, 4);
	wr(p + 4, nimps + 1, 4);
	/* .dynamic */
	p = b + dynaddr;
	off = 1;
	for (n=0; n<nimps; n++)
		off += strlen(gsyms[impsyms[n]].name) + 1;
	for (n=0; n<ndynlibs; n++) {
		name = dynlibs[n];
		wr(p, 1, 8);              /* DT_NEEDED */
		wr(p + 8, off, 8);
		off += strlen(name) + 1;
		p += 16;
	}
	{
		uint64_t tags[12][2] = {
			{4, hashaddr}, {5, dynstraddr}, {6, dynsymaddr}, {10, dynstrsz}, {11, 24},
			{7, reladdr}, {8, 24 * (uint64_t)nimps}, {9, 24},
			{30, 8},                       /* DT_FLAGS: BIND_NOW */
			{0x6ffffffb, 1 | 0x08000000},  /* DT_FLAGS_1: NOW | PIE */
			{21, 0},                       /* DT_DEBUG */
			{0, 0},
		};
		for (n=0; n<12; n++) {
			wr(p, tags[n][0], 8);
			wr(p + 8, tags[n][1], 8);
			p += 16;
		}
	}
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
	static char bareshstr[] = "\0.text\0.rodata\0.data\0.bss\0.trapstacks\0.stack\0.shstrtab";
	/* dynamický program: interpret a tabulky ld.so mají vlastní sekce, aby
	 * je strip/objcopy (BFD skládá soubor ze sekcí) zachoval; data v nich
	 * leží uvnitř segmentů .rodata a .data, hlavičky sekcí se nepřekrývají */
	static char dynshstr[] = "\0.text\0.rodata\0.data\0.bss\0.shstrtab\0.interp\0.got\0.dynsym\0.dynstr\0.hash\0.rela.dyn\0.dynamic";
	uint32_t names[NOut] = {1, 7, 15, 21};
	char *strs;
	uint64_t nstrs;
	int nsh;

	/* bare metal: bez segmentu hlaviček, sekce bez vstupu za .bss;
	 * dynamický program: PHDR, INTERP a DYNAMIC navíc */
	phnum = bare ? 4 : dynamic ? 8 : 5;
	nsh = bare ? NOut + 4 : dynamic ? NOut + 9 : NOut + 2;
	strs = bare ? bareshstr : dynamic ? dynshstr : shstr;
	nstrs = bare ? sizeof bareshstr : dynamic ? sizeof dynshstr : sizeof shstr;
	phent = elf64 ? 56 : 32;
	shent = elf64 ? 64 : 40;
	ehsz = elf64 ? 64 : 52;
	end = osecs[OData].off + osecs[OData].size;
	shoff = alignup(end + nstrs, 8);
	total = shoff + nsh * shent;
	buf = emalloc(total);
	memset(buf, 0, total);
	relocate(buf);
	if (dynamic)
		writedynamic(buf);
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
	/* e_flags: RISC-V ABI lp64d; ARM EABI 5 s VFP argumenty */
	wr(buf + (elf64 ? 48 : 36), machine == 243 ? 4 : machine == 40 ? 0x5000400 : 0, 4);
	if (elf64) {
		wr(buf + 24, entry, 8);
		wr(buf + 32, 64, 8);
		wr(buf + 40, shoff, 8);
		wr(buf + 52, 64, 2);
		wr(buf + 54, phent, 2);
		wr(buf + 56, phnum, 2);
		wr(buf + 58, shent, 2);
		wr(buf + 60, nsh, 2);
		wr(buf + 62, nsh - 1, 2);
	} else {
		wr(buf + 24, entry, 4);
		wr(buf + 28, 52, 4);
		wr(buf + 32, shoff, 4);
		wr(buf + 40, 52, 2);
		wr(buf + 42, phent, 2);
		wr(buf + 44, phnum, 2);
		wr(buf + 46, shent, 2);
		wr(buf + 48, nsh, 2);
		wr(buf + 50, nsh - 1, 2);
	}
	p = buf + ehsz;
	if (dynamic) {
		/* PT_PHDR a PT_INTERP před nahrávanými segmenty */
		phdr(p, 6, 4, ehsz, base + ehsz, phnum * phent, phnum * phent, 8);
		p += phent;
		phdr(p, 3, 4, osecs[ORodata].off + (interpaddr - osecs[ORodata].addr), interpaddr, strlen(interp) + 1, strlen(interp) + 1, 1);
		p += phent;
	}
	if (!bare) {
		phdr(p, 1, 4, 0, base, ehsz + phnum * phent, ehsz + phnum * phent, page);
		p += phent;
	}
	phdr(p, 1, 5, osecs[OText].off, osecs[OText].addr, osecs[OText].size, osecs[OText].size, page);
	p += phent;
	phdr(p, 1, 4, osecs[ORodata].off, osecs[ORodata].addr, osecs[ORodata].size, osecs[ORodata].size, page);
	p += phent;
	phdr(p, 1, 6, osecs[OData].off, osecs[OData].addr, osecs[OData].size,
		(bare ? barevals[7] : osecs[OBss].addr + osecs[OBss].size) - osecs[OData].addr, page);
	p += phent;
	if (dynamic) {
		phdr(p, 2, 6, osecs[OData].off + (dynaddr - osecs[OData].addr), dynaddr, dynsz, dynsz, 8);
		p += phent;
	}
	phdr(p, 0x6474e551, 6, 0, 0, 0, 0, 16);
	/* hlavičky sekcí */
	memcpy(buf + end, strs, nstrs);
	p = buf + shoff;
	shdr(p, 0, 0, 0, 0, 0, 0, 0);
	if (dynamic) {
		/* pořadí podle adres: .text .rodata .interp .data .got .dynsym
		 * .dynstr .hash .rela.dyn .dynamic .bss (.dynsym = 6, .dynstr = 7) */
		uint64_t rooff = osecs[ORodata].off - osecs[ORodata].addr;
		uint64_t daoff = osecs[OData].off - osecs[OData].addr;
		p += shent;
		shdr(p, 1, 1, osecs[OText].flags, osecs[OText].addr, osecs[OText].off, osecs[OText].size, osecs[OText].align);
		p += shent;
		shdr(p, 7, 1, osecs[ORodata].flags, osecs[ORodata].addr, osecs[ORodata].off, interpaddr - osecs[ORodata].addr, osecs[ORodata].align);
		p += shent;
		shdr(p, 36, 1, 2, interpaddr, rooff + interpaddr, strlen(interp) + 1, 1);
		p += shent;
		shdr(p, 15, 1, osecs[OData].flags, osecs[OData].addr, osecs[OData].off, gotaddr - osecs[OData].addr, osecs[OData].align);
		p += shent;
		shdr(p, 44, 1, 3, gotaddr, daoff + gotaddr, 8 * (uint64_t)nimps, 8);
		p += shent;
		shdrx(p, 49, 11, 2, dynsymaddr, daoff + dynsymaddr, 24 * ((uint64_t)nimps + 1), 8, 7, 1, 24);
		p += shent;
		shdr(p, 57, 3, 2, dynstraddr, daoff + dynstraddr, dynstrsz, 1);
		p += shent;
		shdrx(p, 65, 5, 2, hashaddr, daoff + hashaddr, 4 * (2 + 1 + ((uint64_t)nimps + 1)), 8, 6, 0, 4);
		p += shent;
		shdrx(p, 71, 4, 2, reladdr, daoff + reladdr, 24 * (uint64_t)nimps, 8, 6, 0, 24);
		p += shent;
		shdrx(p, 81, 6, 3, dynaddr, daoff + dynaddr, dynsz, 8, 7, 0, 16);
		p += shent;
		shdr(p, 21, 8, osecs[OBss].flags, osecs[OBss].addr, osecs[OBss].off, osecs[OBss].size, osecs[OBss].align);
	} else
	for (i=0; i<NOut; i++) {
		p += shent;
		shdr(p, names[i], osecs[i].nobits ? 8 : 1, osecs[i].flags, osecs[i].addr, osecs[i].off, osecs[i].size, osecs[i].align);
	}
	if (bare) {
		p += shent;
		shdr(p, 26, 8, 3, baresecs[0].addr, baresecs[0].off, baresecs[0].size, 4096);
		p += shent;
		shdr(p, 38, 8, 3, baresecs[1].addr, baresecs[1].off, baresecs[1].size, 4096);
	}
	p += shent;
	shdr(p, bare ? 45 : 26, 3, 0, 0, end, nstrs, 1);
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
link(char **inputs, int ninputs, char *out, int is64, int mach, int ispie, int isbare, char *entry)
{
	int n;

	elf64 = is64;
	/* arm64: GNU ld zarovnává segmenty na 64 KiB kvůli jádrům s větší
	 * stránkou; ostatní cíle mají stránku 4 KiB */
	page = mach == 183 ? 65536 : 4096;
	machine = mach;
	/* bare metal: pevné adresy (ET_EXEC), absolutní relokace dovolené */
	bare = isbare;
	pie = ispie && !bare;
	entryname = entry ? entry : "_start";
	/* pevná báze jako GNU ld: i386 0x08048000, arm 0x10000, jinak 0x400000 */
	base = pie ? 0 : (elf64 ? 0x400000 : machine == 40 ? 0x10000 : 0x08048000);
	for (n=0; n<ninputs; n++) {
		size_t l = strlen(inputs[n]);
		if (l > 5 && strcmp(inputs[n] + l - 5, ".libs") == 0) {
			readlibs(inputs[n]);
			continue;
		}
		readobj(inputs[n]);
	}
	if (dynamic && (machine != 62 || !pie))
		lerr("dynamic linking needs a PIE for x86-64");
	resolve();
	gc();
	if (dynamic)
		checkimports();
	if (bare)
		layoutbare();
	else
		layout();
	writeexe(out);
	return 0;
}

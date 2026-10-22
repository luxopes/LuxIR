#include "../all.h"
#include <stdarg.h>

/* Vestavěný assembler WebAssembly (luxir -a -t wasm32): text WAT, jak ho
 * vypisuje emitor wasm32 LuxIR a vrstvy architektury runtime LSL, na
 * binární modul bajtově shodný s wat2wasm (WABT) bez ladicích jmen.
 *
 * Vstupy se spojí do jednoho modulu: každý je buď (module …), nebo jen
 * posloupnost polí (zkratka formátu textu). Pole: type, import (func,
 * memory, global, table), func (vložené exporty, param a local se jmény
 * i bez), table, memory (vložené exporty, limity, shared), global, export,
 * start, elem (aktivní do tabulky 0, seznam funkcí) a data (aktivní
 * i pasivní). Instrukce v ploché i vnořené (folded) podobě: MVP,
 * sign-extension, saturační převody, bulk memory a atomické operace
 * (vlákna). Konstanty f32/f64, typy bloků s parametry nebo více výsledky
 * a vložené importy nepodporuje (ohlásí chybu). Modul nevaliduje: to
 * dělá běhové prostředí při načtení.
 *
 * Jako WABT: typy jsou nejdřív explicitní pole type v pořadí textu, za
 * nimi signatury funkcí a importů funkcí bez shodného typu, přidané
 * v pořadí polí; v každém prostoru indexů jsou importy první (import za
 * definicí je chyba). Sekce DataCount je jen u modulu, jehož kód používá
 * memory.init nebo data.drop. Čísla LEB128 v nejkratší podobě. */



enum {
	TLpar,
	TRpar,
	TAtom,
	TStr,
};

typedef struct WBuf WBuf;
struct WBuf {
	uchar *p;
	uint n, cap;
};

/* jména: prostor + token $jméno → hodnota */
enum {
	NFunc,
	NType,
	NGlobal,
	NTable,
	NMem,
	NData,
	NElem,
	NLocal,
	NOpcode,
};

typedef struct WEnt WEnt;
struct WEnt {
	int ns;
	char *s;       /* text klíče (do src nebo literál) */
	uint len;
	int val;
	int used;
};

typedef struct WTab WTab;
struct WTab {
	WEnt *e;
	uint cap;      /* mocnina dvou */
	uint n;
};

/* instrukce: druh okamžitých operandů */
enum {
	INone,
	IMem,      /* memarg s přirozeným zarovnáním al */
	IBlock,    /* block, loop, if */
	IElse,
	IEnd,
	ILabel,    /* br, br_if */
	IBrTable,
	ICall,
	ICallInd,
	ILocal,
	IGlobal,
	II32,
	II64,
	IZero,     /* jeden nulový bajt (index paměti) */
	IZero2,    /* memory.copy */
	IDataMem,  /* memory.init: data, paměť */
	IData,     /* data.drop */
	IFloat,    /* f32.const, f64.const: nepodporováno */
};

typedef struct WOp WOp;
struct WOp {
	int prefix;    /* 0, 0xfc, 0xfe */
	int code;
	int kind;
	int al;        /* log2 přirozeného zarovnání (IMem) */
};

/* Tabulka instrukcí jako jeden řetězec: „jméno kód druh;“, kód je hex
 * (s předponou fc/fe), druh: - nic, m0..m3 memarg, b blok, e else, n end,
 * l návěští, t br_table, c call, i call_indirect, L local, G global,
 * 4 i32.const, 8 i64.const, f float, z nulový bajt, Z dva, D memory.init,
 * x data.drop. */
static char optext[] =
	"unreachable 00 -;nop 01 -;block 02 b;loop 03 b;if 04 b;else 05 e;end 0b n;"
	"br 0c l;br_if 0d l;br_table 0e t;return 0f -;call 10 c;call_indirect 11 i;"
	"drop 1a -;select 1b -;local.get 20 L;local.set 21 L;local.tee 22 L;"
	"global.get 23 G;global.set 24 G;"
	"i32.load 28 m2;i64.load 29 m3;f32.load 2a m2;f64.load 2b m3;"
	"i32.load8_s 2c m0;i32.load8_u 2d m0;i32.load16_s 2e m1;i32.load16_u 2f m1;"
	"i64.load8_s 30 m0;i64.load8_u 31 m0;i64.load16_s 32 m1;i64.load16_u 33 m1;"
	"i64.load32_s 34 m2;i64.load32_u 35 m2;i32.store 36 m2;i64.store 37 m3;"
	"f32.store 38 m2;f64.store 39 m3;i32.store8 3a m0;i32.store16 3b m1;"
	"i64.store8 3c m0;i64.store16 3d m1;i64.store32 3e m2;"
	"memory.size 3f z;memory.grow 40 z;i32.const 41 4;i64.const 42 8;f32.const 43 f;f64.const 44 f;"
	"i32.eqz 45 -;i32.eq 46 -;i32.ne 47 -;i32.lt_s 48 -;i32.lt_u 49 -;i32.gt_s 4a -;"
	"i32.gt_u 4b -;i32.le_s 4c -;i32.le_u 4d -;i32.ge_s 4e -;i32.ge_u 4f -;"
	"i64.eqz 50 -;i64.eq 51 -;i64.ne 52 -;i64.lt_s 53 -;i64.lt_u 54 -;i64.gt_s 55 -;"
	"i64.gt_u 56 -;i64.le_s 57 -;i64.le_u 58 -;i64.ge_s 59 -;i64.ge_u 5a -;"
	"f32.eq 5b -;f32.ne 5c -;f32.lt 5d -;f32.gt 5e -;f32.le 5f -;f32.ge 60 -;"
	"f64.eq 61 -;f64.ne 62 -;f64.lt 63 -;f64.gt 64 -;f64.le 65 -;f64.ge 66 -;"
	"i32.clz 67 -;i32.ctz 68 -;i32.popcnt 69 -;i32.add 6a -;i32.sub 6b -;i32.mul 6c -;"
	"i32.div_s 6d -;i32.div_u 6e -;i32.rem_s 6f -;i32.rem_u 70 -;i32.and 71 -;"
	"i32.or 72 -;i32.xor 73 -;i32.shl 74 -;i32.shr_s 75 -;i32.shr_u 76 -;"
	"i32.rotl 77 -;i32.rotr 78 -;"
	"i64.clz 79 -;i64.ctz 7a -;i64.popcnt 7b -;i64.add 7c -;i64.sub 7d -;i64.mul 7e -;"
	"i64.div_s 7f -;i64.div_u 80 -;i64.rem_s 81 -;i64.rem_u 82 -;i64.and 83 -;"
	"i64.or 84 -;i64.xor 85 -;i64.shl 86 -;i64.shr_s 87 -;i64.shr_u 88 -;"
	"i64.rotl 89 -;i64.rotr 8a -;"
	"f32.abs 8b -;f32.neg 8c -;f32.ceil 8d -;f32.floor 8e -;f32.trunc 8f -;"
	"f32.nearest 90 -;f32.sqrt 91 -;f32.add 92 -;f32.sub 93 -;f32.mul 94 -;"
	"f32.div 95 -;f32.min 96 -;f32.max 97 -;f32.copysign 98 -;"
	"f64.abs 99 -;f64.neg 9a -;f64.ceil 9b -;f64.floor 9c -;f64.trunc 9d -;"
	"f64.nearest 9e -;f64.sqrt 9f -;f64.add a0 -;f64.sub a1 -;f64.mul a2 -;"
	"f64.div a3 -;f64.min a4 -;f64.max a5 -;f64.copysign a6 -;"
	"i32.wrap_i64 a7 -;i32.trunc_f32_s a8 -;i32.trunc_f32_u a9 -;i32.trunc_f64_s aa -;"
	"i32.trunc_f64_u ab -;i64.extend_i32_s ac -;i64.extend_i32_u ad -;"
	"i64.trunc_f32_s ae -;i64.trunc_f32_u af -;i64.trunc_f64_s b0 -;i64.trunc_f64_u b1 -;"
	"f32.convert_i32_s b2 -;f32.convert_i32_u b3 -;f32.convert_i64_s b4 -;"
	"f32.convert_i64_u b5 -;f32.demote_f64 b6 -;f64.convert_i32_s b7 -;"
	"f64.convert_i32_u b8 -;f64.convert_i64_s b9 -;f64.convert_i64_u ba -;"
	"f64.promote_f32 bb -;i32.reinterpret_f32 bc -;i64.reinterpret_f64 bd -;"
	"f32.reinterpret_i32 be -;f64.reinterpret_i64 bf -;"
	"i32.extend8_s c0 -;i32.extend16_s c1 -;i64.extend8_s c2 -;i64.extend16_s c3 -;"
	"i64.extend32_s c4 -;"
	"i32.trunc_sat_f32_s fc00 -;i32.trunc_sat_f32_u fc01 -;i32.trunc_sat_f64_s fc02 -;"
	"i32.trunc_sat_f64_u fc03 -;i64.trunc_sat_f32_s fc04 -;i64.trunc_sat_f32_u fc05 -;"
	"i64.trunc_sat_f64_s fc06 -;i64.trunc_sat_f64_u fc07 -;"
	"memory.init fc08 D;data.drop fc09 x;memory.copy fc0a Z;memory.fill fc0b z;"
	"memory.atomic.notify fe00 m2;memory.atomic.wait32 fe01 m2;memory.atomic.wait64 fe02 m3;"
	"atomic.fence fe03 z;"
	"i32.atomic.load fe10 m2;i64.atomic.load fe11 m3;i32.atomic.load8_u fe12 m0;"
	"i32.atomic.load16_u fe13 m1;i64.atomic.load8_u fe14 m0;i64.atomic.load16_u fe15 m1;"
	"i64.atomic.load32_u fe16 m2;i32.atomic.store fe17 m2;i64.atomic.store fe18 m3;"
	"i32.atomic.store8 fe19 m0;i32.atomic.store16 fe1a m1;i64.atomic.store8 fe1b m0;"
	"i64.atomic.store16 fe1c m1;i64.atomic.store32 fe1d m2;"
	"i32.atomic.rmw.add fe1e m2;i64.atomic.rmw.add fe1f m3;i32.atomic.rmw8.add_u fe20 m0;"
	"i32.atomic.rmw16.add_u fe21 m1;i64.atomic.rmw8.add_u fe22 m0;i64.atomic.rmw16.add_u fe23 m1;"
	"i64.atomic.rmw32.add_u fe24 m2;"
	"i32.atomic.rmw.sub fe25 m2;i64.atomic.rmw.sub fe26 m3;i32.atomic.rmw8.sub_u fe27 m0;"
	"i32.atomic.rmw16.sub_u fe28 m1;i64.atomic.rmw8.sub_u fe29 m0;i64.atomic.rmw16.sub_u fe2a m1;"
	"i64.atomic.rmw32.sub_u fe2b m2;"
	"i32.atomic.rmw.and fe2c m2;i64.atomic.rmw.and fe2d m3;i32.atomic.rmw8.and_u fe2e m0;"
	"i32.atomic.rmw16.and_u fe2f m1;i64.atomic.rmw8.and_u fe30 m0;i64.atomic.rmw16.and_u fe31 m1;"
	"i64.atomic.rmw32.and_u fe32 m2;"
	"i32.atomic.rmw.or fe33 m2;i64.atomic.rmw.or fe34 m3;i32.atomic.rmw8.or_u fe35 m0;"
	"i32.atomic.rmw16.or_u fe36 m1;i64.atomic.rmw8.or_u fe37 m0;i64.atomic.rmw16.or_u fe38 m1;"
	"i64.atomic.rmw32.or_u fe39 m2;"
	"i32.atomic.rmw.xor fe3a m2;i64.atomic.rmw.xor fe3b m3;i32.atomic.rmw8.xor_u fe3c m0;"
	"i32.atomic.rmw16.xor_u fe3d m1;i64.atomic.rmw8.xor_u fe3e m0;i64.atomic.rmw16.xor_u fe3f m1;"
	"i64.atomic.rmw32.xor_u fe40 m2;"
	"i32.atomic.rmw.xchg fe41 m2;i64.atomic.rmw.xchg fe42 m3;i32.atomic.rmw8.xchg_u fe43 m0;"
	"i32.atomic.rmw16.xchg_u fe44 m1;i64.atomic.rmw8.xchg_u fe45 m0;i64.atomic.rmw16.xchg_u fe46 m1;"
	"i64.atomic.rmw32.xchg_u fe47 m2;"
	"i32.atomic.rmw.cmpxchg fe48 m2;i64.atomic.rmw.cmpxchg fe49 m3;i32.atomic.rmw8.cmpxchg_u fe4a m0;"
	"i32.atomic.rmw16.cmpxchg_u fe4b m1;i64.atomic.rmw8.cmpxchg_u fe4c m0;"
	"i64.atomic.rmw16.cmpxchg_u fe4d m1;i64.atomic.rmw32.cmpxchg_u fe4e m2;";

static WOp *ops;
static uint nops;

/* vstup: spojený text všech souborů a tokeny */
static char *src;
static uint srclen;
static char **fnames;
static uint *fstart;
static int nfiles;
static uchar *tkind;
static uint *toff, *tlen, *tend;  /* tend: u '(' index párové ')' */
static uint ntok, tcap;

static WTab names;
static WTab locals;
static WTab opnames;

/* typy: signatury v zásobě bajtů (parametry, pak výsledky) */
static uchar *sigs;
static uint nsigs, sigcap;
static uint *tsig, *tnp, *tnr;
static uint ntypes, typecap;

typedef struct WImp WImp;
struct WImp {
	uint mod, name;  /* tokeny řetězců */
	int kind;        /* 0 func, 1 table, 2 memory, 3 global */
	uint desc;       /* token '(' popisu */
	int type;        /* func: index typu */
};

typedef struct WFld WFld;
struct WFld {
	uint tok;        /* '(' pole */
	int type;        /* func: index typu */
};

typedef struct WExp WExp;
struct WExp {
	uint name;       /* token řetězce */
	int kind;
	int idx;         /* index, nebo -1: var v tokenu var */
	uint var;
};

static WImp *imps;
static uint nimps;
static WFld *funcs, *tables, *mems, *globals, *datas, *elems;
static uint nfuncs, ntables, nmems, nglobals, ndatas, nelems;
static WExp *exps;
static uint nexps;
static uint nimpfunc, nimpglobal, nimptable, nimpmem;
static int starttok;
static int datacount;

/* stav kódu funkce */
static uint *labels;   /* zásobník návěští: token jména nebo 0 */
static uint nlabels, labelcap;

/* ---- chyby a buffery ---- */

extern char *tmpasm;

static void
werr(uint t, char *fmt, ...)
{
	va_list ap;
	uint off, line, i;
	int f;

	off = t < ntok ? toff[t] : srclen;
	f = 0;
	while (f + 1 < nfiles && fstart[f + 1] <= off)
		f++;
	line = 1;
	for (i=fstart[f]; i<off; i++)
		if (src[i] == '\n')
			line++;
	fprintf(stderr, "luxir: wasm: %s:%u: ", fnames[f], line);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	/* dočasný text modulu režimu v jednom běhu nezůstává */
	if (tmpasm)
		luxir_unlink(tmpasm);
	exit(1);
}

static void *
wgrow(void *p, uint *cap, uint need, size_t esz)
{
	uint c;

	if (need <= *cap)
		return p;
	c = *cap ? *cap : 16;
	while (c < need)
		c *= 2;
	p = realloc(p, (size_t)c * esz);
	if (!p)
		die("out of memory");
	*cap = c;
	return p;
}

static void
bput(WBuf *b, int c)
{
	if (b->n == b->cap)
		b->p = wgrow(b->p, &b->cap, b->n + 1, 1);
	b->p[b->n++] = c;
}

static void
bbytes(WBuf *b, uchar *p, uint n)
{
	b->p = wgrow(b->p, &b->cap, b->n + n, 1);
	memcpy(b->p + b->n, p, n);
	b->n += n;
}

static void
uleb(WBuf *b, uint64_t v)
{
	int c;

	do {
		c = v & 0x7f;
		v >>= 7;
		if (v)
			c |= 0x80;
		bput(b, c);
	} while (v);
}

static void
sleb(WBuf *b, int64_t v)
{
	int c;

	for (;;) {
		c = v & 0x7f;
		v >>= 7;
		if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) {
			bput(b, c);
			return;
		}
		bput(b, c | 0x80);
	}
}

static void
bfree(WBuf *b)
{
	free(b->p);
	b->p = 0;
	b->n = b->cap = 0;
}

/* ---- tabulka jmen ---- */

static uint
whash(int ns, char *s, uint n)
{
	uint h, i;

	h = 2166136261u ^ (uint)ns;
	for (i=0; i<n; i++)
		h = (h ^ (uchar)s[i]) * 16777619u;
	return h;
}

static void
tabinit(WTab *t, uint cap)
{
	t->cap = cap;
	t->n = 0;
	t->e = emalloc(cap * sizeof t->e[0]);
	memset(t->e, 0, cap * sizeof t->e[0]);
}

static WEnt *
tabfind(WTab *t, int ns, char *s, uint n)
{
	uint i;
	WEnt *e;

	i = whash(ns, s, n) & (t->cap - 1);
	for (;;) {
		e = &t->e[i];
		if (!e->used || (e->ns == ns && e->len == n && memcmp(e->s, s, n) == 0))
			return e;
		i = (i + 1) & (t->cap - 1);
	}
}

static void
tabadd(WTab *t, int ns, char *s, uint n, int val, uint tok)
{
	WEnt *e, *old;
	uint i, cap;

	if (2 * (t->n + 1) > t->cap) {
		old = t->e;
		cap = t->cap;
		tabinit(t, cap * 2);
		for (i=0; i<cap; i++)
			if (old[i].used) {
				e = tabfind(t, old[i].ns, old[i].s, old[i].len);
				*e = old[i];
				t->n++;
			}
		free(old);
	}
	e = tabfind(t, ns, s, n);
	if (e->used)
		werr(tok, "duplicate name %.*s", (int)n, s);
	e->used = 1;
	e->ns = ns;
	e->s = s;
	e->len = n;
	e->val = val;
	t->n++;
}

/* ---- tokeny ---- */

static int
isid(uint t)
{
	return tkind[t] == TAtom && src[toff[t]] == '$';
}

static int
teq(uint t, char *s)
{
	uint n;

	if (tkind[t] != TAtom)
		return 0;
	n = strlen(s);
	return tlen[t] == n && memcmp(src + toff[t], s, n) == 0;
}

/* klíčové slovo pole nebo vnořené formy: atom za '(' */
static int
formis(uint t, char *s)
{
	return tkind[t] == TLpar && t + 1 < ntok && teq(t + 1, s);
}

static void
addtok(int kind, uint off, uint len)
{
	if (ntok == tcap) {
		uint c = tcap;
		tkind = wgrow(tkind, &c, ntok + 1, 1);
		c = tcap;
		toff = wgrow(toff, &c, ntok + 1, sizeof toff[0]);
		c = tcap;
		tlen = wgrow(tlen, &c, ntok + 1, sizeof tlen[0]);
		c = tcap;
		tend = wgrow(tend, &c, ntok + 1, sizeof tend[0]);
		tcap = c;
	}
	tkind[ntok] = kind;
	toff[ntok] = off;
	tlen[ntok] = len;
	tend[ntok] = 0;
	ntok++;
}

static void
tokenize(void)
{
	uint i, start, depth, *stack, nstack, stackcap;
	int c;

	stack = 0;
	nstack = stackcap = 0;
	i = 0;
	while (i < srclen) {
		c = src[i];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			i++;
			continue;
		}
		if (c == ';' && src[i + 1] == ';') {
			while (i < srclen && src[i] != '\n')
				i++;
			continue;
		}
		if (c == '(' && src[i + 1] == ';') {
			depth = 1;
			i += 2;
			while (i < srclen && depth) {
				if (src[i] == '(' && src[i + 1] == ';') {
					depth++;
					i += 2;
				} else if (src[i] == ';' && src[i + 1] == ')') {
					depth--;
					i += 2;
				} else
					i++;
			}
			continue;
		}
		if (c == '(') {
			stack = wgrow(stack, &stackcap, nstack + 1, sizeof stack[0]);
			stack[nstack++] = ntok;
			addtok(TLpar, i, 1);
			i++;
			continue;
		}
		if (c == ')') {
			if (!nstack) {
				addtok(TRpar, i, 1);
				werr(ntok - 1, "unbalanced )");
			}
			tend[stack[--nstack]] = ntok;
			addtok(TRpar, i, 1);
			i++;
			continue;
		}
		if (c == '"') {
			start = i++;
			while (i < srclen && src[i] != '"') {
				if (src[i] == '\\')
					i++;
				i++;
			}
			if (i >= srclen) {
				addtok(TStr, start, 1);
				werr(ntok - 1, "unterminated string");
			}
			i++;
			addtok(TStr, start, i - start);
			continue;
		}
		start = i;
		while (i < srclen) {
			c = src[i];
			if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '(' || c == ')' || c == '"'
			|| (c == ';' && src[i + 1] == ';'))
				break;
			i++;
		}
		addtok(TAtom, start, i - start);
	}
	if (nstack)
		werr(stack[nstack - 1], "unbalanced (");
	free(stack);
	/* zarážky: tokeny za posledním nejsou '(' ani atomy */
	for (i=0; i<4; i++)
		addtok(TRpar, srclen, 0);
	ntok -= 4;
}

static void
readsrc(char **paths, int npaths)
{
	FILE *f;
	uint cap;
	int k;
	size_t got;

	cap = 1 << 16;
	src = emalloc(cap);
	srclen = 0;
	fnames = emalloc((npaths + 1) * sizeof fnames[0]);
	fstart = emalloc((npaths + 1) * sizeof fstart[0]);
	nfiles = npaths;
	for (k=0; k<npaths; k++) {
		fnames[k] = paths[k];
		fstart[k] = srclen;
		f = fopen(paths[k], "rb");
		if (!f) {
			fprintf(stderr, "luxir: wasm: cannot open %s\n", paths[k]);
			exit(1);
		}
		for (;;) {
			src = wgrow(src, &cap, srclen + 4097, 1);
			got = fread(src + srclen, 1, cap - srclen - 2, f);
			if (got == 0)
				break;
			srclen += got;
		}
		fclose(f);
		src[srclen++] = '\n';
	}
	src[srclen] = 0;
}

/* ---- čísla a řetězce ---- */

static int
digit(int c, int base)
{
	int d;

	if (c >= '0' && c <= '9')
		d = c - '0';
	else if (c >= 'a' && c <= 'f')
		d = c - 'a' + 10;
	else if (c >= 'A' && c <= 'F')
		d = c - 'A' + 10;
	else
		return -1;
	return d < base ? d : -1;
}

/* celé číslo atomu (i ve tvaru jméno=číslo od posunu skip); 0 = není */
static int
intat(uint t, uint skip, int64_t *v)
{
	char *s;
	uint n, i;
	uint64_t x;
	int neg, base, d, any;

	if (tkind[t] != TAtom || tlen[t] <= skip)
		return 0;
	s = src + toff[t] + skip;
	n = tlen[t] - skip;
	i = 0;
	neg = 0;
	if (s[0] == '-' || s[0] == '+') {
		neg = s[0] == '-';
		i++;
	}
	base = 10;
	if (i + 1 < n && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
		base = 16;
		i += 2;
	}
	x = 0;
	any = 0;
	for (; i<n; i++) {
		if (s[i] == '_')
			continue;
		d = digit(s[i], base);
		if (d < 0)
			return 0;
		x = x * base + d;
		any = 1;
	}
	if (!any)
		return 0;
	*v = neg ? -(int64_t)x : (int64_t)x;
	return 1;
}

static int64_t
needint(uint t)
{
	int64_t v;

	if (!intat(t, 0, &v))
		werr(t, "number expected");
	return v;
}

/* obsah řetězce s escape sekvencemi do b */
static void
strbytes(WBuf *b, uint t)
{
	char *s;
	uint n, i;
	int c, d1, d2;
	uint64_t u;

	if (tkind[t] != TStr)
		werr(t, "string expected");
	s = src + toff[t] + 1;
	n = tlen[t] - 2;
	for (i=0; i<n; i++) {
		c = (uchar)s[i];
		if (c != '\\') {
			bput(b, c);
			continue;
		}
		c = s[++i];
		if (c == 'n')
			bput(b, '\n');
		else if (c == 't')
			bput(b, '\t');
		else if (c == 'r')
			bput(b, '\r');
		else if (c == '"' || c == '\'' || c == '\\')
			bput(b, c);
		else if (c == 'u') {
			if (s[i + 1] != '{')
				werr(t, "bad \\u escape");
			i += 2;
			u = 0;
			while (i < n && s[i] != '}') {
				if (digit(s[i], 16) < 0)
					werr(t, "bad \\u escape");
				u = u * 16 + digit(s[i], 16);
				i++;
			}
			/* UTF-8 */
			if (u < 0x80)
				bput(b, u);
			else if (u < 0x800) {
				bput(b, 0xc0 | (u >> 6));
				bput(b, 0x80 | (u & 0x3f));
			} else if (u < 0x10000) {
				bput(b, 0xe0 | (u >> 12));
				bput(b, 0x80 | ((u >> 6) & 0x3f));
				bput(b, 0x80 | (u & 0x3f));
			} else {
				bput(b, 0xf0 | (u >> 18));
				bput(b, 0x80 | ((u >> 12) & 0x3f));
				bput(b, 0x80 | ((u >> 6) & 0x3f));
				bput(b, 0x80 | (u & 0x3f));
			}
		} else {
			d1 = digit(c, 16);
			d2 = i + 1 < n ? digit(s[i + 1], 16) : -1;
			if (d1 < 0 || d2 < 0)
				werr(t, "bad string escape");
			bput(b, d1 * 16 + d2);
			i++;
		}
	}
}

/* jméno jako vektor bajtů */
static void
strvec(WBuf *b, uint t)
{
	WBuf s;

	memset(&s, 0, sizeof s);
	strbytes(&s, t);
	uleb(b, s.n);
	if (s.n)
		bbytes(b, s.p, s.n);
	bfree(&s);
}

static int
valtype(uint t)
{
	if (teq(t, "i32"))
		return 0x7f;
	if (teq(t, "i64"))
		return 0x7e;
	if (teq(t, "f32"))
		return 0x7d;
	if (teq(t, "f64"))
		return 0x7c;
	if (teq(t, "funcref"))
		return 0x70;
	if (teq(t, "externref"))
		return 0x6f;
	werr(t, "value type expected");
	return 0;
}

/* ---- instrukce: tabulka ---- */

static void
opinit(void)
{
	char *p, *name;
	uint n, len;
	WOp *o;
	int v, d;

	n = 0;
	for (p=optext; *p; p++)
		if (*p == ';')
			n++;
	ops = emalloc(n * sizeof ops[0]);
	nops = 0;
	tabinit(&opnames, 1024);
	p = optext;
	while (*p) {
		name = p;
		while (*p != ' ')
			p++;
		len = p - name;
		p++;
		o = &ops[nops];
		o->prefix = 0;
		if (p[0] == 'f' && (p[1] == 'c' || p[1] == 'e') && p[2] != ' ') {
			o->prefix = p[1] == 'c' ? 0xfc : 0xfe;
			p += 2;
		}
		v = 0;
		while ((d = digit(*p, 16)) >= 0) {
			v = v * 16 + d;
			p++;
		}
		o->code = v;
		p++;
		o->al = 0;
		switch (*p) {
		case 'm': o->kind = IMem; o->al = p[1] - '0'; p++; break;
		case 'b': o->kind = IBlock; break;
		case 'e': o->kind = IElse; break;
		case 'n': o->kind = IEnd; break;
		case 'l': o->kind = ILabel; break;
		case 't': o->kind = IBrTable; break;
		case 'c': o->kind = ICall; break;
		case 'i': o->kind = ICallInd; break;
		case 'L': o->kind = ILocal; break;
		case 'G': o->kind = IGlobal; break;
		case '4': o->kind = II32; break;
		case '8': o->kind = II64; break;
		case 'f': o->kind = IFloat; break;
		case 'z': o->kind = IZero; break;
		case 'Z': o->kind = IZero2; break;
		case 'D': o->kind = IDataMem; break;
		case 'x': o->kind = IData; break;
		default: o->kind = INone; break;
		}
		p++;
		if (*p == ';')
			p++;
		tabadd(&opnames, NOpcode, name, len, nops, 0);
		nops++;
	}
}

static WOp *
findop(uint t)
{
	WEnt *e;

	if (tkind[t] != TAtom)
		werr(t, "instruction expected");
	e = tabfind(&opnames, NOpcode, src + toff[t], tlen[t]);
	if (!e->used)
		werr(t, "unknown instruction %.*s", (int)tlen[t], src + toff[t]);
	return &ops[e->val];
}

/* index: číslo nebo $jméno v prostoru ns */
static int
varidx(uint t, int ns)
{
	int64_t v;
	WEnt *e;

	if (isid(t)) {
		e = tabfind(ns == NLocal ? &locals : &names, ns, src + toff[t], tlen[t]);
		if (!e->used)
			werr(t, "undefined name %.*s", (int)tlen[t], src + toff[t]);
		return e->val;
	}
	if (!intat(t, 0, &v) || v < 0)
		werr(t, "index expected");
	return v;
}

static void
defname(uint t, int ns, int val)
{
	tabadd(ns == NLocal ? &locals : &names, ns, src + toff[t], tlen[t], val, t);
}

/* ---- typy ---- */

/* signatura z (param …) a (result …) od tokenu t; vrací token za nimi.
 * pnames: u parametrů se jmény jejich definice jako lokálních (kód) */
static uint
sigparse(uint t, WBuf *p, WBuf *r, int pnames)
{
	uint u;

	while (formis(t, "param")) {
		u = t + 2;
		if (isid(u)) {
			if (pnames)
				defname(u, NLocal, p->n);
			bput(p, valtype(u + 1));
			if (u + 2 != tend[t])
				werr(t, "named param with more than one type");
		} else
			for (; u<tend[t]; u++)
				bput(p, valtype(u));
		t = tend[t] + 1;
	}
	while (formis(t, "result")) {
		for (u=t + 2; u<tend[t]; u++)
			bput(r, valtype(u));
		t = tend[t] + 1;
	}
	return t;
}

static int
findsig(WBuf *p, WBuf *r)
{
	uint i;

	for (i=0; i<ntypes; i++)
		if (tnp[i] == p->n && tnr[i] == r->n
		&& (p->n == 0 || memcmp(sigs + tsig[i], p->p, p->n) == 0)
		&& (r->n == 0 || memcmp(sigs + tsig[i] + p->n, r->p, r->n) == 0))
			return i;
	return -1;
}

static int
addsig(WBuf *p, WBuf *r)
{
	uint c;

	c = typecap;
	tsig = wgrow(tsig, &c, ntypes + 1, sizeof tsig[0]);
	c = typecap;
	tnp = wgrow(tnp, &c, ntypes + 1, sizeof tnp[0]);
	c = typecap;
	tnr = wgrow(tnr, &c, ntypes + 1, sizeof tnr[0]);
	typecap = c;
	sigs = wgrow(sigs, &sigcap, nsigs + p->n + r->n + 1, 1);
	tsig[ntypes] = nsigs;
	if (p->n)
		memcpy(sigs + nsigs, p->p, p->n);
	if (r->n)
		memcpy(sigs + nsigs + p->n, r->p, r->n);
	nsigs += p->n + r->n;
	tnp[ntypes] = p->n;
	tnr[ntypes] = r->n;
	return ntypes++;
}

/* typeuse funkce od tokenu t: (type $x)? (param)* (result)*; vrací index
 * typu (implicitní přidá jako WABT) a token za ním v *next */
static int
typeuse(uint t, uint *next, int pnames)
{
	WBuf p, r;
	int ty;
	uint i;

	memset(&p, 0, sizeof p);
	memset(&r, 0, sizeof r);
	ty = -1;
	if (formis(t, "type")) {
		ty = varidx(t + 2, NType);
		if ((uint)ty >= ntypes)
			werr(t + 2, "unknown type");
		t = tend[t] + 1;
	}
	t = sigparse(t, &p, &r, pnames);
	if (ty >= 0 && pnames && p.n == 0)
		/* parametry jen z typu: bez jmen */
		for (i=0; i<tnp[ty]; i++)
			bput(&p, sigs[tsig[ty] + i]);
	if (ty < 0) {
		ty = findsig(&p, &r);
		if (ty < 0)
			ty = addsig(&p, &r);
	}
	*next = t;
	bfree(&p);
	bfree(&r);
	return ty;
}

/* ---- pole modulu ---- */

static WFld *
addfld(WFld *v, uint *n, uint tok)
{
	v = realloc(v, (*n + 1) * sizeof v[0]);
	if (!v)
		die("out of memory");
	v[*n].tok = tok;
	v[*n].type = -1;
	(*n)++;
	return v;
}

static void
addexp(uint name, int kind, int idx, uint var)
{
	exps = realloc(exps, (nexps + 1) * sizeof exps[0]);
	if (!exps)
		die("out of memory");
	exps[nexps].name = name;
	exps[nexps].kind = kind;
	exps[nexps].idx = idx;
	exps[nexps].var = var;
	nexps++;
}

/* vložené (export "…") od tokenu t; vrací token za nimi */
static uint
inlineexp(uint t, int kind, int idx)
{
	while (formis(t, "export")) {
		addexp(t + 2, kind, idx, 0);
		t = tend[t] + 1;
	}
	if (formis(t, "import"))
		werr(t, "inline imports are not supported");
	return t;
}

static int defined;  /* už bylo pole s definicí (importy musí být před ní) */
static int expns[4] = {NFunc, NTable, NMem, NGlobal};

static void
field(uint t)
{
	uint u, e;
	int kind, idx;

	e = tend[t];
	u = t + 1;
	if (teq(u, "type")) {
		return;   /* typy už ve sběru typů */
	} else if (teq(u, "import")) {
		if (defined)
			werr(t, "imports must occur before all non-import definitions");
		if (tkind[u + 1] != TStr || tkind[u + 2] != TStr || tkind[u + 3] != TLpar)
			werr(t, "bad import");
		imps = realloc(imps, (nimps + 1) * sizeof imps[0]);
		if (!imps)
			die("out of memory");
		imps[nimps].mod = u + 1;
		imps[nimps].name = u + 2;
		imps[nimps].desc = u + 3;
		imps[nimps].type = -1;
		u += 4;
		if (teq(u, "func")) {
			kind = 0;
			idx = nimpfunc++;
			if (isid(u + 1)) {
				defname(u + 1, NFunc, idx);
				u++;
			}
			imps[nimps].type = typeuse(u + 1, &u, 0);
		} else if (teq(u, "table")) {
			kind = 1;
			idx = nimptable++;
			if (isid(u + 1))
				defname(u + 1, NTable, idx);
		} else if (teq(u, "memory")) {
			kind = 2;
			idx = nimpmem++;
			if (isid(u + 1))
				defname(u + 1, NMem, idx);
		} else if (teq(u, "global")) {
			kind = 3;
			idx = nimpglobal++;
			if (isid(u + 1))
				defname(u + 1, NGlobal, idx);
		} else
			werr(u, "unknown import kind");
		imps[nimps].kind = kind;
		nimps++;
		return;
	}
	/* definice: import za nimi je chyba (export, start, elem a data ne) */
	if (teq(u, "func") || teq(u, "table") || teq(u, "memory") || teq(u, "global"))
		defined = 1;
	if (teq(u, "func")) {
		idx = nimpfunc + nfuncs;
		funcs = addfld(funcs, &nfuncs, t);
		u++;
		if (isid(u)) {
			defname(u, NFunc, idx);
			u++;
		}
		u = inlineexp(u, 0, idx);
		funcs[nfuncs - 1].type = typeuse(u, &u, 0);
	} else if (teq(u, "table")) {
		idx = nimptable + ntables;
		tables = addfld(tables, &ntables, t);
		u++;
		if (isid(u)) {
			defname(u, NTable, idx);
			u++;
		}
		inlineexp(u, 1, idx);
	} else if (teq(u, "memory")) {
		idx = nimpmem + nmems;
		mems = addfld(mems, &nmems, t);
		u++;
		if (isid(u)) {
			defname(u, NMem, idx);
			u++;
		}
		inlineexp(u, 2, idx);
	} else if (teq(u, "global")) {
		idx = nimpglobal + nglobals;
		globals = addfld(globals, &nglobals, t);
		u++;
		if (isid(u)) {
			defname(u, NGlobal, idx);
			u++;
		}
		inlineexp(u, 3, idx);
	} else if (teq(u, "export")) {
		if (tkind[u + 1] != TStr || tkind[u + 2] != TLpar)
			werr(t, "bad export");
		u += 2;
		if (formis(u, "func"))
			kind = 0;
		else if (formis(u, "table"))
			kind = 1;
		else if (formis(u, "memory"))
			kind = 2;
		else if (formis(u, "global"))
			kind = 3;
		else
			werr(u, "unknown export kind");
		addexp(t + 2, kind, -1, u + 2);
	} else if (teq(u, "start")) {
		starttok = u + 1;
	} else if (teq(u, "elem")) {
		idx = nelems;
		elems = addfld(elems, &nelems, t);
		if (isid(u + 1))
			defname(u + 1, NElem, idx);
	} else if (teq(u, "data")) {
		idx = ndatas;
		datas = addfld(datas, &ndatas, t);
		if (isid(u + 1))
			defname(u + 1, NData, idx);
	} else
		werr(u, "unknown module field");
	(void)e;
}

/* pole modulu od tokenu t do e (bez); (module …) se rozbalí */
static void
fields(uint t, uint e, int types)
{
	uint u;
	WBuf p, r;

	while (t < e) {
		if (tkind[t] != TLpar)
			werr(t, "module field expected");
		if (teq(t + 1, "module")) {
			u = t + 2;
			if (u < tend[t] && isid(u))
				u++;
			fields(u, tend[t], types);
		} else if (types) {
			if (teq(t + 1, "type")) {
				u = t + 2;
				if (isid(u)) {
					defname(u, NType, ntypes);
					u++;
				}
				if (!formis(u, "func"))
					werr(u, "func type expected");
				memset(&p, 0, sizeof p);
				memset(&r, 0, sizeof r);
				if (sigparse(u + 2, &p, &r, 0) != tend[u])
					werr(u, "bad func type");
				addsig(&p, &r);
				bfree(&p);
				bfree(&r);
			}
		} else
			field(t);
		t = tend[t] + 1;
	}
}

/* ---- kód ---- */

static void instrs(WBuf *b, uint t, uint e);

static void
pushlabel(uint name)
{
	labels = wgrow(labels, &labelcap, nlabels + 1, sizeof labels[0]);
	labels[nlabels++] = name;
}

static int
labeldepth(uint t)
{
	uint i;
	int64_t v;

	if (isid(t)) {
		for (i=nlabels; i-- > 0; )
			if (labels[i] && tlen[labels[i]] == tlen[t]
			&& memcmp(src + toff[labels[i]], src + toff[t], tlen[t]) == 0)
				return nlabels - 1 - i;
		werr(t, "undefined label %.*s", (int)tlen[t], src + toff[t]);
	}
	if (!intat(t, 0, &v) || v < 0)
		werr(t, "label expected");
	return v;
}

/* typ bloku od tokenu t: (result t)? ; vrací token za ním */
static uint
blocktype(WBuf *b, uint t)
{
	if (formis(t, "type") || formis(t, "param"))
		werr(t, "block types with parameters are not supported");
	if (formis(t, "result")) {
		if (tend[t] != t + 3)
			werr(t, "multi-value blocks are not supported");
		bput(b, valtype(t + 2));
		return tend[t] + 1;
	}
	bput(b, 0x40);
	return t;
}

static void
opcode(WBuf *b, WOp *o)
{
	if (o->prefix) {
		bput(b, o->prefix);
		uleb(b, o->code);
	} else
		bput(b, o->code);
}

/* instrukce o s okamžitými operandy od tokenu t (do e) do b; vrací token
 * za operandy. Bloky v ploché podobě: návěští na zásobník (block, loop,
 * if), else a end. */
static uint
encode(WBuf *b, WOp *o, uint t, uint e)
{
	int64_t v, off, al;
	int n;
	uint u;

	switch (o->kind) {
	case INone:
		opcode(b, o);
		return t;
	case IBlock:
		opcode(b, o);
		u = 0;
		if (t < e && isid(t))
			u = t++;
		t = blocktype(b, t);
		pushlabel(u);
		return t;
	case IElse:
	case IEnd:
		if (o->kind == IEnd) {
			if (!nlabels)
				werr(t - 1, "end without block");
			nlabels--;
		}
		u = t;
		if (u < e && isid(u))
			u++;
		/* prázdná větev else se nezapisuje (WABT) */
		if (o->kind == IElse && u < e && teq(u, "end"))
			return u;
		opcode(b, o);
		return u;
	case ILabel:
		opcode(b, o);
		uleb(b, labeldepth(t));
		return t + 1;
	case IBrTable:
		opcode(b, o);
		for (u=t; u<e && tkind[u] == TAtom && (isid(u) || intat(u, 0, &v)); u++)
			;
		if (u == t)
			werr(t, "br_table needs labels");
		uleb(b, u - t - 1);
		for (; t<u; t++)
			uleb(b, labeldepth(t));
		return t;
	case ICall:
		opcode(b, o);
		uleb(b, varidx(t, NFunc));
		return t + 1;
	case ICallInd:
		n = 0;
		if (t < e && tkind[t] == TAtom) {
			n = varidx(t, NTable);
			t++;
		}
		if (!formis(t, "type"))
			werr(t, "call_indirect needs (type …)");
		opcode(b, o);
		uleb(b, varidx(t + 2, NType));
		uleb(b, n);
		t = tend[t] + 1;
		if (formis(t, "param") || formis(t, "result"))
			werr(t, "inline call_indirect signatures are not supported");
		return t;
	case ILocal:
		opcode(b, o);
		uleb(b, varidx(t, NLocal));
		return t + 1;
	case IGlobal:
		opcode(b, o);
		uleb(b, varidx(t, NGlobal));
		return t + 1;
	case II32:
		opcode(b, o);
		v = needint(t);
		sleb(b, (int32_t)v);
		return t + 1;
	case II64:
		opcode(b, o);
		sleb(b, needint(t));
		return t + 1;
	case IMem:
		off = 0;
		al = -1;
		for (;;) {
			if (t < e && tkind[t] == TAtom && tlen[t] > 7 && memcmp(src + toff[t], "offset=", 7) == 0) {
				if (!intat(t, 7, &off) || off < 0)
					werr(t, "bad offset");
				t++;
			} else if (t < e && tkind[t] == TAtom && tlen[t] > 6 && memcmp(src + toff[t], "align=", 6) == 0) {
				if (!intat(t, 6, &v) || v <= 0 || (v & (v - 1)))
					werr(t, "bad align");
				for (al=0; (1 << al) < v; al++)
					;
				t++;
			} else
				break;
		}
		opcode(b, o);
		uleb(b, al < 0 ? o->al : al);
		uleb(b, off);
		return t;
	case IZero:
		opcode(b, o);
		bput(b, 0);
		return t;
	case IZero2:
		opcode(b, o);
		bput(b, 0);
		bput(b, 0);
		return t;
	case IDataMem:
		opcode(b, o);
		uleb(b, varidx(t, NData));
		bput(b, 0);
		datacount = 1;
		return t + 1;
	case IData:
		opcode(b, o);
		uleb(b, varidx(t, NData));
		datacount = 1;
		return t + 1;
	case IFloat:
		werr(t - 1, "f32.const and f64.const are not supported");
	}
	return t;
}

/* vnořená instrukce (op imm… operandy…) od '(' v tokenu t */
static void
folded(WBuf *b, uint t)
{
	WOp *o;
	WBuf ins;
	uint e, u, c;

	e = tend[t];
	if (teq(t + 1, "block") || teq(t + 1, "loop")) {
		o = findop(t + 1);
		u = encode(b, o, t + 2, e);
		instrs(b, u, e);
		nlabels--;
		bput(b, 0x0b);
		return;
	}
	if (teq(t + 1, "if")) {
		memset(&ins, 0, sizeof ins);
		o = findop(t + 1);
		u = t + 2;
		c = 0;
		if (u < e && isid(u))
			c = u++;
		bput(&ins, 0x04);
		u = blocktype(&ins, u);
		/* podmínka: vnořené výrazy před (then) */
		while (u < e && !formis(u, "then")) {
			if (tkind[u] != TLpar)
				werr(u, "folded if needs (then …)");
			folded(b, u);
			u = tend[u] + 1;
		}
		if (u >= e)
			werr(t, "folded if needs (then …)");
		bbytes(b, ins.p, ins.n);
		bfree(&ins);
		pushlabel(c);
		instrs(b, u + 2, tend[u]);
		u = tend[u] + 1;
		if (u < e && formis(u, "else")) {
			if (tend[u] > u + 2)
				bput(b, 0x05);
			instrs(b, u + 2, tend[u]);
			u = tend[u] + 1;
		}
		if (u != e)
			werr(u, "unexpected token in folded if");
		nlabels--;
		bput(b, 0x0b);
		(void)o;
		return;
	}
	o = findop(t + 1);
	if (o->kind == IBlock || o->kind == IElse || o->kind == IEnd)
		werr(t + 1, "unexpected block instruction");
	memset(&ins, 0, sizeof ins);
	u = encode(&ins, o, t + 2, e);
	instrs(b, u, e);
	bbytes(b, ins.p, ins.n);
	bfree(&ins);
}

static void
instrs(WBuf *b, uint t, uint e)
{
	WOp *o;

	while (t < e) {
		if (tkind[t] == TLpar) {
			folded(b, t);
			t = tend[t] + 1;
		} else if (tkind[t] == TAtom) {
			o = findop(t);
			t = encode(b, o, t + 1, e);
		} else
			werr(t, "instruction expected");
	}
}

/* konstantní výraz (init globálu, posun dat a elem) od t do e */
static void
constexpr(WBuf *b, uint t, uint e)
{
	nlabels = 0;
	instrs(b, t, e);
	bput(b, 0x0b);
}

/* tělo funkce: lokály a kód */
static void
funcbody(WBuf *code, WFld *f)
{
	WBuf body, lt;
	uint t, e, u, i, n, run;
	int nparams;

	t = f->tok + 2;
	e = tend[f->tok];
	tabinit(&locals, 64);
	if (isid(t))
		t++;
	while (formis(t, "export"))
		t = tend[t] + 1;
	/* parametry jako lokály (jména), počet z typu */
	typeuse(t, &t, 1);
	nparams = tnp[f->type];
	memset(&lt, 0, sizeof lt);
	n = nparams;
	while (formis(t, "local")) {
		u = t + 2;
		if (isid(u)) {
			defname(u, NLocal, n++);
			bput(&lt, valtype(u + 1));
			if (u + 2 != tend[t])
				werr(t, "named local with more than one type");
		} else
			for (; u<tend[t]; u++) {
				bput(&lt, valtype(u));
				n++;
			}
		t = tend[t] + 1;
	}
	memset(&body, 0, sizeof body);
	/* lokály po skupinách stejného typu */
	run = 0;
	for (i=0; i<lt.n; i++)
		if (i == 0 || lt.p[i] != lt.p[i - 1])
			run++;
	uleb(&body, run);
	for (i=0; i<lt.n; i=u) {
		for (u=i; u<lt.n && lt.p[u] == lt.p[i]; u++)
			;
		uleb(&body, u - i);
		bput(&body, lt.p[i]);
	}
	nlabels = 0;
	instrs(&body, t, e);
	if (nlabels)
		werr(e, "unclosed block");
	bput(&body, 0x0b);
	uleb(code, body.n);
	bbytes(code, body.p, body.n);
	bfree(&body);
	bfree(&lt);
	free(locals.e);
}

/* limity od tokenu t (min max? shared?) */
static uint
limits(WBuf *b, uint t, uint e)
{
	int64_t mn, mx;
	int hasmax, shared;

	mn = needint(t++);
	hasmax = 0;
	mx = 0;
	if (t < e && intat(t, 0, &mx)) {
		hasmax = 1;
		t++;
	}
	shared = 0;
	if (t < e && teq(t, "shared")) {
		shared = 1;
		t++;
	}
	bput(b, (hasmax ? 1 : 0) | (shared ? 2 : 0));
	uleb(b, mn);
	if (hasmax)
		uleb(b, mx);
	return t;
}

static void
section(WBuf *mod, int id, WBuf *s, uint count)
{
	WBuf h;

	memset(&h, 0, sizeof h);
	uleb(&h, count);
	bput(mod, id);
	uleb(mod, h.n + s->n);
	bbytes(mod, h.p, h.n);
	if (s->n)
		bbytes(mod, s->p, s->n);
	bfree(&h);
	bfree(s);
}

static uint
afterexports(uint t)
{
	while (formis(t, "export"))
		t = tend[t] + 1;
	return t;
}

int
watassemble(char **paths, int npaths, char *out)
{
	WBuf mod, s;
	FILE *f;
	uint i, t, e;
	WImp *im;

	readsrc(paths, npaths);
	tokenize();
	opinit();
	tabinit(&names, 1024);
	fields(0, ntok, 1);
	fields(0, ntok, 0);
	memset(&mod, 0, sizeof mod);
	memset(&s, 0, sizeof s);
	/* \0asm, verze 1 */
	bput(&mod, 0);
	bput(&mod, 'a');
	bput(&mod, 's');
	bput(&mod, 'm');
	bput(&mod, 1);
	bput(&mod, 0);
	bput(&mod, 0);
	bput(&mod, 0);
	/* typy */
	if (ntypes) {
		for (i=0; i<ntypes; i++) {
			bput(&s, 0x60);
			uleb(&s, tnp[i]);
			bbytes(&s, sigs + tsig[i], tnp[i]);
			uleb(&s, tnr[i]);
			bbytes(&s, sigs + tsig[i] + tnp[i], tnr[i]);
		}
		section(&mod, 1, &s, ntypes);
	}
	/* importy */
	if (nimps) {
		for (i=0; i<nimps; i++) {
			im = &imps[i];
			strvec(&s, im->mod);
			strvec(&s, im->name);
			bput(&s, im->kind);
			t = im->desc + 2;
			e = tend[im->desc];
			if (t < e && isid(t))
				t++;
			if (im->kind == 0)
				uleb(&s, im->type);
			else if (im->kind == 1) {
				/* (table min max? reftype) */
				bput(&s, valtype(e - 1));
				limits(&s, t, e - 1);
			} else if (im->kind == 2)
				limits(&s, t, e);
			else if (formis(t, "mut")) {
				bput(&s, valtype(t + 2));
				bput(&s, 1);
			} else {
				bput(&s, valtype(t));
				bput(&s, 0);
			}
		}
		section(&mod, 2, &s, nimps);
	}
	/* funkce */
	if (nfuncs) {
		for (i=0; i<nfuncs; i++)
			uleb(&s, funcs[i].type);
		section(&mod, 3, &s, nfuncs);
	}
	/* tabulky: (table $T? export* min max? reftype) */
	if (ntables) {
		for (i=0; i<ntables; i++) {
			t = tables[i].tok + 2;
			e = tend[tables[i].tok];
			if (isid(t))
				t++;
			t = afterexports(t);
			if (e < t + 2)
				werr(t, "bad table");
			bput(&s, valtype(e - 1));
			limits(&s, t, e - 1);
		}
		section(&mod, 4, &s, ntables);
	}
	/* paměti */
	if (nmems) {
		for (i=0; i<nmems; i++) {
			t = mems[i].tok + 2;
			e = tend[mems[i].tok];
			if (isid(t))
				t++;
			t = afterexports(t);
			limits(&s, t, e);
		}
		section(&mod, 5, &s, nmems);
	}
	/* globály */
	if (nglobals) {
		for (i=0; i<nglobals; i++) {
			t = globals[i].tok + 2;
			e = tend[globals[i].tok];
			if (isid(t))
				t++;
			t = afterexports(t);
			if (formis(t, "mut")) {
				bput(&s, valtype(t + 2));
				bput(&s, 1);
				t = tend[t] + 1;
			} else {
				bput(&s, valtype(t));
				bput(&s, 0);
				t++;
			}
			constexpr(&s, t, e);
		}
		section(&mod, 6, &s, nglobals);
	}
	/* exporty */
	if (nexps) {
		for (i=0; i<nexps; i++) {
			strvec(&s, exps[i].name);
			bput(&s, exps[i].kind);
			uleb(&s, exps[i].idx >= 0 ? exps[i].idx : varidx(exps[i].var, expns[exps[i].kind]));
		}
		section(&mod, 7, &s, nexps);
	}
	if (starttok) {
		uleb(&s, varidx(starttok, NFunc));
		bput(&mod, 8);
		uleb(&mod, s.n);
		bbytes(&mod, s.p, s.n);
		bfree(&s);
	}
	/* elem: (elem $e? (offset)? func $f…) aktivní do tabulky 0 */
	if (nelems) {
		for (i=0; i<nelems; i++) {
			t = elems[i].tok + 2;
			e = tend[elems[i].tok];
			if (isid(t))
				t++;
			if (tkind[t] != TLpar || formis(t, "table"))
				werr(t, "only active elem segments of table 0 are supported");
			bput(&s, 0);
			if (formis(t, "offset"))
				constexpr(&s, t + 2, tend[t]);
			else
				constexpr(&s, t, tend[t] + 1);
			t = tend[t] + 1;
			if (t < e && teq(t, "func"))
				t++;
			uleb(&s, e - t);
			for (; t<e; t++)
				uleb(&s, varidx(t, NFunc));
		}
		section(&mod, 9, &s, nelems);
	}
	/* kód (DataCount podle instrukcí v kódu: kód napřed do bufferu) */
	{
		WBuf code;
		memset(&code, 0, sizeof code);
		for (i=0; i<nfuncs; i++)
			funcbody(&code, &funcs[i]);
		if (datacount && ndatas) {
			uleb(&s, ndatas);
			bput(&mod, 12);
			uleb(&mod, s.n);
			bbytes(&mod, s.p, s.n);
			bfree(&s);
		}
		if (nfuncs)
			section(&mod, 10, &code, nfuncs);
	}
	/* data: (data $d? (memory 0)? offset? "…"*) */
	if (ndatas) {
		for (i=0; i<ndatas; i++) {
			WBuf d;
			t = datas[i].tok + 2;
			e = tend[datas[i].tok];
			if (isid(t))
				t++;
			if (formis(t, "memory"))
				werr(t, "data with an explicit memory is not supported");
			if (t < e && tkind[t] == TLpar) {
				bput(&s, 0);
				if (formis(t, "offset"))
					constexpr(&s, t + 2, tend[t]);
				else
					constexpr(&s, t, tend[t] + 1);
				t = tend[t] + 1;
			} else
				bput(&s, 1);
			memset(&d, 0, sizeof d);
			for (; t<e; t++)
				strbytes(&d, t);
			uleb(&s, d.n);
			if (d.n)
				bbytes(&s, d.p, d.n);
			bfree(&d);
		}
		section(&mod, 11, &s, ndatas);
	}
	f = fopen(out, "wb");
	if (!f) {
		fprintf(stderr, "luxir: wasm: cannot create %s\n", out);
		exit(1);
	}
	for (i=0; i<mod.n; i++)
		fputc(mod.p[i], f);
	fclose(f);
	return 0;
}

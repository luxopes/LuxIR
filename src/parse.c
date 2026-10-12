/* Čtečka LuxIR: textová IR se syntaxí ve stylu LSL.
 *
 * Výstupem jsou stejné struktury Fn/Blk/Ins/Dat/Typ, jaké používají
 * všechny další průchody backendu. Specifikace jazyka je v doc/ir.md.
 * Tělo funkce se nejdřív načte do bufferu tokenů; předběžný průchod
 * v něm najde lokální jména, takže lokální hodnoty i globální symboly
 * se píší bez prefixů.
 */
#include "all.h"
#include "version.h"
#include <ctype.h>
#include <stdarg.h>

enum {
	Ksb = 4, /* odpovídá pořadí Oarg/Opar/Jret */
	Kub,
	Ksh,
	Kuh,
	Kc,
	K0,
	Kenv, /* jen anotace Env(...) */

	Ke = -2, /* chybný režim */
	Km = Kl, /* ukazatel do paměti */
};

Op optab[NOp] = {
#undef F
#define F(cf, hi, id, co, as, im, ic, lg, cv, pn) \
	.canfold = cf, \
	.hasid = hi, .idval = id, \
	.commutes = co, .assoc = as, \
	.idemp = im, \
	.cmpeqwl = ic, .cmplgtewl = lg, .eqval = cv, \
	.pinned = pn
#define O(op, k, flags) [O##op]={.name = #op, .argcls = k, flags},
	#include "ops.h"
#undef F
};

typedef enum {
	PXXX,
	PLbl,
	PPhi,
	PIns,
	PEnd,
} PState;

enum Token {
	Txxx = 0,
	Tid,
	Tint,
	Tflt,
	Tstr,
	Tlparen,
	Trparen,
	Tlbrack,
	Trbrack,
	Tcomma,
	Tcolon,
	Teq,
	Tplus,
	Tstar,
	Tarrow,
	Tdots,
	Tnl,
	Tfnend, /* konec těla funkce v bufferu */
	Teof,
};

/* klíčová slova a jména typů */
enum {
	Wnone = 0,
	Wfunction,
	Wdata,
	Wtype,
	Wexport,
	Wthread,
	Wcommon,
	Wsection,
	Walign,
	Wfile,
	Wend,
	Wgoto,
	Wif,
	Welse,
	Wreturn,
	Whalt,
	Wcall,
	Wphi,
	Wstore,
	Wblit,
	Wvastart,
	Wdbgloc,
	Wstruct,
	Wunion,
	Wopaque,
	Wzero,
	Wextern,
	Wbits,
	Wirversion,
	Wruntimeabi,
	WInt8,
	WUInt8,
	WInt16,
	WUInt16,
	WInt32,
	WInt64,
	WFloat32,
	WFloat64,
	WPtr,
	WEnv,
	Nkw,

	/* hodnoty pro instrukce */
	WOp = 100,          /* WOp + O...: přímá instrukce */
	WPseudo = 1000,     /* WPseudo + P...: odvozená z typu operandu */
};

/* instrukce, jejichž přesná operace závisí na třídě operandů */
enum {
	Peq,
	Pne,
	Plt,
	Ple,
	Pgt,
	Pge,
	Pult,
	Pule,
	Pugt,
	Puge,
	Pord,
	Punord,
	Pftoi,
	Pftou,
	Pitof,
	Putof,
	NPseudo
};

static char *kwname[Nkw] = {
	[Wirversion] = "ir_version",
	[Wruntimeabi] = "runtime_abi",
	[Wfunction] = "function",
	[Wdata] = "data",
	[Wtype] = "type",
	[Wexport] = "export",
	[Wthread] = "thread",
	[Wcommon] = "common",
	[Wsection] = "section",
	[Walign] = "align",
	[Wfile] = "file",
	[Wend] = "end",
	[Wgoto] = "goto",
	[Wif] = "if",
	[Welse] = "else",
	[Wreturn] = "return",
	[Whalt] = "halt",
	[Wcall] = "call",
	[Wphi] = "phi",
	[Wstore] = "store",
	[Wblit] = "blit",
	[Wvastart] = "vastart",
	[Wdbgloc] = "dbgloc",
	[Wstruct] = "struct",
	[Wunion] = "union",
	[Wopaque] = "opaque",
	[Wzero] = "zero",
	[Wextern] = "extern",
	[Wbits] = "bits",
	[WInt8] = "Int8",
	[WUInt8] = "UInt8",
	[WInt16] = "Int16",
	[WUInt16] = "UInt16",
	[WInt32] = "Int32",
	[WInt64] = "Int64",
	[WFloat32] = "Float32",
	[WFloat64] = "Float64",
	[WPtr] = "Ptr",
	[WEnv] = "Env",
};

static char *pseudoname[NPseudo] = {
	[Peq] = "eq",
	[Pne] = "ne",
	[Plt] = "lt",
	[Ple] = "le",
	[Pgt] = "gt",
	[Pge] = "ge",
	[Pult] = "ult",
	[Pule] = "ule",
	[Pugt] = "ugt",
	[Puge] = "uge",
	[Pord] = "ord",
	[Punord] = "unord",
	[Pftoi] = "ftoi",
	[Pftou] = "ftou",
	[Pitof] = "itof",
	[Putof] = "utof",
};

/* přímé instrukce: jméno v LuxIR a operace backendu */
static char *opname[] = {
	"add", "sub", "neg", "div", "rem", "udiv", "urem", "mul", "sqrt", "umulh", "mulh", "clz", "ctz", "popcount",
	"atomic_load", "atomic_store", "atomic_add", "atomic_xchg", "atomic_cas", "atomic_fence",
	"and", "or", "xor", "sar", "shr", "shl", "copy",
	"load", "load_i8", "load_u8", "load_i16", "load_u16",
	"load_i32", "load_u32",
	"sext8", "zext8", "sext16", "zext16", "sext32", "zext32",
	"fext", "ftrunc", "bitcast",
	"alloc1", "alloc2", "alloc4", "alloc8", "alloc16",
	"vaarg",
	0
};
static int opcode[] = {
	Oadd, Osub, Oneg, Odiv, Orem, Oudiv, Ourem, Omul, Osqrt, Oumulh, Omulh, Oclz, Octz, Opopcount,
	Oaload, Oastore, Oaadd, Oaxchg, Oacas, Oafence,
	Oand, Oor, Oxor, Osar, Oshr, Oshl, Ocopy,
	Oload, Oloadsb, Oloadub, Oloadsh, Oloaduh,
	Oloadsw, Oloaduw,
	Oextsb, Oextub, Oextsh, Oextuh, Oextsw, Oextuw,
	Oexts, Otruncd, Ocast,
	Oalloc4, Oalloc4, Oalloc4, Oalloc8, Oalloc16,
	Ovaarg,
	0
};

enum {
	NPred = 63,

	NMask = 1023, /* tabulka klíčových slov */
};

#define NEXTC(c) do { c = src[spos++]; if (c == 0) c = endch(); } while (0)
#define BACKC(c) do { if (c != EOF) spos--; } while (0)

typedef struct Tok Tok;
typedef struct Opd Opd;

struct Tok {
	int t;
	int line;
	int id;    /* jméno ve funkci pro Tid */
	int64_t num;
	char *str;
};

/* rozparsovaný operand; konstanty vznikají až se známou třídou */
enum {
	OTmp,
	OGlo,
	OInt,
	OFlt,
};

struct Opd {
	int kind;
	Ref r;
	int cls;   /* třída hodnoty (dočasná hodnota, symbol) nebo Kx */
	int ann;   /* typová anotace Type(...) nebo Kx */
	int tyn;   /* agregát pro ann == Kc */
	int symty; /* SThr/SExt pro symboly */
	int rawbits; /* bits(N): vzor bitů i v plovoucím kontextu */
	uint32_t sym;
	int64_t num;
	char *flt;
};

static char *nmstr[NMask+1];
static int nmval[NMask+1];

/* celý vstup v paměti, zakončený nulou */
static uchar *src;
static uint64_t spos;
static uint64_t slen;
static char *inpath;
static int lnum;
static int depth;
static int thead;
static char *lexbuf;
static int lexcap;
static int64_t lexnum;
/* identifikátor: začátek ve vstupu, délka a hash */
static uint64_t idpos;
static int idlen;
static uint32_t idhash;
static int collect; /* lexer plní buffer funkce */

/* třídy znaků */
enum {
	CName = 1,
	CStart = 2,
	CDigit = 4,
	CHex = 8,
};
static uchar chcls[256];

/* aktuální token pro parser */
static char *tstr;
static int64_t tnum;
static int tid;

/* buffer tokenů funkce */
static Tok *tk;
static uint ntk;
static uint tkcap;
static uint itk;
static int buffered;

/* jména funkce: id -> vlastnosti */
enum { NLoc = -3 };
static char **fnstr;
static int *fnlen;
static uint32_t *fnhash;
static int *fnw;      /* klíčové slovo nebo instrukce */
static int *fnloc;    /* třída lokálu nebo NLoc */
static int *fntmp;    /* dočasná hodnota, 0 když ještě nevznikla */
static Blk **fnblk;   /* blok s tímto návěštím */
static uint32_t *fnsym; /* intern() pro globální symbol */
static char *fnsymok;
static uint *fnslot;
static int *fnh;      /* hash tabulka: id+1 */
static uint fnhcap;
static uint nfn;
static uint fncap;

static Fn *curf;
static Phi **plink;
static Blk *curb;
static Blk **blink;
static int nblk;
static int rcls;
static uint ntyp;

void
err(char *s, ...)
{
	va_list ap;

	va_start(ap, s);
	fprintf(stderr, "luxir:%s:%d: ", inpath, lnum);
	vfprintf(stderr, s, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static void
nameadd(char *s, int v)
{
	uint h;

	h = hash(s) & NMask;
	while (nmstr[h])
		h = (h+1) & NMask;
	nmstr[h] = s;
	nmval[h] = v;
}

static int
namefind(char *s)
{
	uint h;

	h = hash(s) & NMask;
	while (nmstr[h]) {
		if (strcmp(nmstr[h], s) == 0)
			return nmval[h];
		h = (h+1) & NMask;
	}
	return Wnone;
}

static void
lexinit()
{
	static int done;
	int i;

	if (done)
		return;
	for (i=1; i<Nkw; i++)
		nameadd(kwname[i], i);
	for (i=0; opname[i]; i++)
		nameadd(opname[i], WOp + opcode[i]);
	for (i=0; i<NPseudo; i++)
		nameadd(pseudoname[i], WPseudo + i);
	for (i=0; i<256; i++) {
		if ((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || i == '_' || i == '.')
			chcls[i] |= CName | CStart;
		if (i >= '0' && i <= '9')
			chcls[i] |= CName | CDigit | CHex;
		if ((i >= 'a' && i <= 'f') || (i >= 'A' && i <= 'F'))
			chcls[i] |= CHex;
		if (i == '$')
			chcls[i] |= CName;
	}
	lexcap = 128;
	lexbuf = vnew(lexcap, 1, PHeap);
	tk = vnew(256, sizeof tk[0], PHeap);
	fncap = 0;
	fnhcap = 0;
	nfn = 0;
	done = 1;
}

/* konec vstupu nebo nulový bajt uvnitř */
static int
endch()
{
	if (spos <= slen)
		err("NUL byte in input");
	spos = slen;
	return EOF;
}

/* znak do hlášení: tisknutelné ASCII přímo, jinak číselně */
static char *
chname(int c)
{
	static char buf[16];

	if (c == EOF)
		return "end of file";
	if (c > 32 && c < 127)
		sprintf(buf, "%c", c);
	else
		sprintf(buf, "\\x%02x", c);
	return buf;
}

static void
bufput(int i, int c)
{
	if (i+2 > lexcap) {
		lexcap = 2 * (i+2);
		vgrow(&lexbuf, lexcap);
	}
	lexbuf[i] = c;
}

static int
ishex(int c)
{
	return c != EOF && (chcls[c] & CHex);
}

static int
isname(int c)
{
	return c != EOF && (chcls[c] & CName);
}

/* číslo: desítkově (i desetinné), 0x, 0o, 0b; celé se redukuje mod 2^64 */
/* číslo: desítkově (i desetinné), 0x, 0o, 0b; celé se redukuje mod 2^64 */
static int
lexnumber(int c)
{
	uint64_t n, start;
	int neg, base, flt, d, e, ndig;

	start = spos - 1;
	neg = 0;
	if (c == '-') {
		neg = 1;
		NEXTC(c);
	}
	base = 10;
	if (c == '0') {
		NEXTC(d);
		if (d == 'x' || d == 'X')
			base = 16;
		else if (d == 'o' || d == 'O')
			base = 8;
		else if (d == 'b' || d == 'B')
			base = 2;
		else
			BACKC(d);
		if (base != 10)
			NEXTC(c);
	}
	n = 0;
	flt = 0;
	e = 0;
	ndig = 0;
	for (;;) {
		if (base == 10) {
			if (c != EOF && (chcls[c] & CDigit)) {
				if (!flt)
					n = 10*n + (c - '0');
			} else if (c == '.' && !flt) {
				flt = 1;
			} else if ((c == 'e' || c == 'E') && !e) {
				flt = 1;
				e = 1;
				NEXTC(c);
				if (c != '+' && c != '-' && !(c != EOF && (chcls[c] & CDigit)))
					err("invalid number exponent");
			} else
				break;
		} else {
			if (!ishex(c))
				break;
			d = (chcls[c] & CDigit) ? c - '0' : (c | 32) - 'a' + 10;
			if (d >= base)
				err("invalid digit %s in number", chname(c));
			n = base*n + d;
		}
		ndig++;
		NEXTC(c);
	}
	if (isname(c))
		err("invalid character %s in number", chname(c));
	BACKC(c);
	if (base != 10 && ndig == 0)
		err("digits expected after number prefix");
	if (flt) {
		/* text desetinného čísla se převede až podle třídy */
		ndig = spos - start;
		bufput(ndig, 0);
		memcpy(lexbuf, &src[start], ndig);
		lexbuf[ndig] = 0;
		return Tflt;
	}
	if (neg)
		n = 1 + ~n;
	lexnum = *(int64_t *)&n;
	return Tint;
}

/* řetězec zůstává v uvozovkách jako text pro assembler;
 * escape sekvence LSL se převedou na tvar, kterému rozumí assembler */
/* řetězec zůstává v uvozovkách jako text pro assembler;
 * escape sekvence LSL se převedou na tvar, kterému rozumí assembler */
static int
lexstring()
{
	int c, i, d, n;

	i = 0;
	bufput(i++, '"');
	for (;;) {
		NEXTC(c);
		if (c == EOF || c == '\n')
			err("unterminated string");
		if (c == '"') {
			bufput(i++, c);
			bufput(i, 0);
			return Tstr;
		}
		if (c != '\\') {
			bufput(i++, c);
			continue;
		}
		NEXTC(c);
		switch (c) {
		case 'n':
		case 'r':
		case 't':
		case '"':
		case '\\':
			bufput(i++, '\\');
			bufput(i++, c);
			break;
		case '\'':
			bufput(i++, '\'');
			break;
		case '0':
			/* osmičkově, aby se nespojilo s následujícími číslicemi */
			bufput(i++, '\\');
			bufput(i++, '0');
			bufput(i++, '0');
			bufput(i++, '0');
			break;
		case 'x':
			/* assembler by u \x spojil i další hex číslice,
			 * proto se bajt zapíše třemi osmičkovými číslicemi */
			d = 0;
			for (n=0; n<2; n++) {
				NEXTC(c);
				if (!ishex(c))
					err("two hex digits expected after \\x");
				d = 16*d + ((chcls[c] & CDigit) ? c - '0' : (c | 32) - 'a' + 10);
			}
			bufput(i++, '\\');
			bufput(i++, '0' + (d >> 6));
			bufput(i++, '0' + ((d >> 3) & 7));
			bufput(i++, '0' + (d & 7));
			break;
		default:
			err("invalid escape sequence \\%s", chname(c));
		}
	}
}

static int
lex()
{
	int c;
	uint32_t h;
	uint64_t start;

	for (;;) {
		NEXTC(c);
		if (c == ' ' || c == '\t' || c == '\r')
			continue;
		if (c == '#') {
			do
				NEXTC(c);
			while (c != '\n' && c != EOF);
			if (c == EOF)
				return Teof;
		}
		if (c == '\n') {
			lnum++;
			if (depth > 0)
				continue;
			return Tnl;
		}
		break;
	}
	if (c != EOF && (chcls[c] & CStart)) {
		if (c == '.' && src[spos] == '.') {
			if (src[spos+1] != '.')
				err("... expected");
			spos += 2;
			return Tdots;
		}
		start = spos - 1;
		h = 0;
		do {
			h = c + 17*h;
			NEXTC(c);
		} while (c != EOF && (chcls[c] & CName));
		BACKC(c);
		idpos = start;
		idlen = spos - start;
		idhash = h;
		if (!collect) {
			bufput(idlen, 0);
			memcpy(lexbuf, &src[start], idlen);
			lexbuf[idlen] = 0;
		}
		return Tid;
	}
	if (c != EOF && (chcls[c] & CDigit))
		return lexnumber(c);
	switch (c) {
	case EOF:
		return Teof;
	case ',':
		return Tcomma;
	case '(':
		depth++;
		return Tlparen;
	case ')':
		if (depth > 0)
			depth--;
		return Trparen;
	case '[':
		depth++;
		return Tlbrack;
	case ']':
		if (depth > 0)
			depth--;
		return Trbrack;
	case ':':
		return Tcolon;
	case '=':
		return Teq;
	case '+':
		return Tplus;
	case '*':
		return Tstar;
	case '"':
		return lexstring();
	case '-':
		NEXTC(c);
		if (c == '>')
			return Tarrow;
		if (c == EOF || !(chcls[c] & CDigit))
			err("number or -> expected after -");
		BACKC(c);
		return lexnumber('-');
	}
	err("invalid character %s", chname(c));
	return Txxx;
}

static int
peek()
{
	if (buffered)
		return tk[itk].t;
	if (thead == Txxx)
		thead = lex();
	return thead;
}

static int
peek2()
{
	assert(buffered);
	if (tk[itk].t == Tfnend)
		return Tfnend;
	return tk[itk+1].t;
}

static int
next()
{
	Tok *k;
	int t;

	if (buffered) {
		k = &tk[itk];
		if (k->t != Tfnend)
			itk++;
		lnum = k->line;
		tstr = k->str;
		tnum = k->num;
		tid = k->id;
		return k->t;
	}
	t = peek();
	thead = Txxx;
	tstr = lexbuf;
	tnum = lexnum;
	return t;
}

static int
nextnl()
{
	int t;

	while ((t = next()) == Tnl)
		;
	return t;
}

static char *
tokname(int t)
{
	static char *ttoa[] = {
		[Tid] = "name",
		[Tint] = "integer",
		[Tflt] = "number",
		[Tstr] = "string",
		[Tlparen] = "(",
		[Trparen] = ")",
		[Tlbrack] = "[",
		[Trbrack] = "]",
		[Tcomma] = ",",
		[Tcolon] = ":",
		[Teq] = "=",
		[Tplus] = "+",
		[Tstar] = "*",
		[Tarrow] = "->",
		[Tdots] = "...",
		[Tnl] = "newline",
		[Tfnend] = "end",
		[Teof] = "end of file",
	};

	if (t > Txxx && t <= Teof)
		return ttoa[t];
	return "??";
}

static void
expect(int t)
{
	int t1;

	t1 = next();
	if (t == t1)
		return;
	err("%s expected, got %s instead", tokname(t), tokname(t1));
}

static int
iskw(int w)
{
	if (buffered)
		return fnw[tid] == w;
	return namefind(tstr) == w;
}

/* --- lokální jména --- */

static void
fngrow()
{
	uint i, h, oldcap;

	if (nfn >= fncap) {
		fncap = fncap ? 2*fncap : 4096;
		if (!fnstr) {
			fnstr = vnew(fncap, sizeof fnstr[0], PHeap);
			fnlen = vnew(fncap, sizeof fnlen[0], PHeap);
			fnhash = vnew(fncap, sizeof fnhash[0], PHeap);
			fnw = vnew(fncap, sizeof fnw[0], PHeap);
			fnloc = vnew(fncap, sizeof fnloc[0], PHeap);
			fntmp = vnew(fncap, sizeof fntmp[0], PHeap);
			fnblk = vnew(fncap, sizeof fnblk[0], PHeap);
			fnsym = vnew(fncap, sizeof fnsym[0], PHeap);
			fnsymok = vnew(fncap, sizeof fnsymok[0], PHeap);
			fnslot = vnew(fncap, sizeof fnslot[0], PHeap);
		} else {
			vgrow(&fnstr, fncap);
			vgrow(&fnlen, fncap);
			vgrow(&fnhash, fncap);
			vgrow(&fnw, fncap);
			vgrow(&fnloc, fncap);
			vgrow(&fntmp, fncap);
			vgrow(&fnblk, fncap);
			vgrow(&fnsym, fncap);
			vgrow(&fnsymok, fncap);
			vgrow(&fnslot, fncap);
		}
	}
	if (2*(nfn+1) > fnhcap) {
		oldcap = fnhcap;
		fnhcap = fnhcap ? 2*fnhcap : 8192;
		if (oldcap)
			free(fnh);
		fnh = emalloc(fnhcap * sizeof fnh[0]);
		for (i=0; i<nfn; i++) {
			h = fnhash[i] & (fnhcap-1);
			while (fnh[h])
				h = (h+1) & (fnhcap-1);
			fnh[h] = i+1;
			fnslot[i] = h;
		}
	}
}

/* id jména ze vstupu; nové jméno se zkopíruje jednou */
static int
fnname(uint64_t pos, int len, uint32_t hash)
{
	uint h;
	int id;
	char *s;

	if (fnhcap) {
		h = hash & (fnhcap-1);
		while ((id = fnh[h])) {
			id--;
			if (fnhash[id] == hash && fnlen[id] == len
			&& memcmp(fnstr[id], &src[pos], len) == 0)
				return id;
			h = (h+1) & (fnhcap-1);
		}
	}
	fngrow();
	id = nfn++;
	s = alloc(len+1);
	memcpy(s, &src[pos], len);
	s[len] = 0;
	fnstr[id] = s;
	fnlen[id] = len;
	fnhash[id] = hash;
	fnw[id] = namefind(s);
	fnloc[id] = NLoc;
	fntmp[id] = 0;
	fnblk[id] = 0;
	fnsymok[id] = 0;
	h = hash & (fnhcap-1);
	while (fnh[h])
		h = (h+1) & (fnhcap-1);
	fnh[h] = id+1;
	fnslot[id] = h;
	return id;
}

static void
fnreset()
{
	uint i;

	for (i=0; i<nfn; i++)
		fnh[fnslot[i]] = 0;
	nfn = 0;
}

/* --- typy --- */

static int
findtyp(char *s, int n)
{
	while (--n >= 0)
		if (strcmp(s, typ[n].name) == 0)
			return n;
	return -1;
}

/* třída podle jména typu; Kc pro agregát, Kx když jméno není typ */
static int
typecls(char *s, int *tyn, int n)
{
	*tyn = -1;
	switch (namefind(s)) {
	case WInt8: return Ksb;
	case WUInt8: return Kub;
	case WInt16: return Ksh;
	case WUInt16: return Kuh;
	case WInt32: return Kw;
	case WInt64: return Kl;
	case WPtr: return Kl;
	case WFloat32: return Ks;
	case WFloat64: return Kd;
	case WEnv: return Kenv;
	}
	*tyn = findtyp(s, n);
	if (*tyn >= 0)
		return Kc;
	return Kx;
}

static int
basecls(int k)
{
	if (k >= Ksb && k <= Kuh)
		return Kw;
	if (k == Kc || k == Kenv)
		return Kl;
	return k;
}

static char *
clsname(int k)
{
	static char *ktoa[] = {"Int32", "Int64", "Float32", "Float64",
		"Int8", "UInt8", "Int16", "UInt16"};

	if (k >= Kw && k <= Kuh)
		return ktoa[k];
	return "?";
}

/* --- operandy --- */

static Ref
tmpid(int id)
{
	int t;

	t = fntmp[id];
	if (!t) {
		t = curf->ntmp;
		newtmp(0, Kx, curf);
		curf->tmp[t].name = fnstr[id];
		fntmp[id] = t;
	}
	return TMP(t);
}

static uint32_t
symid(int id)
{
	if (!fnsymok[id]) {
		fnsym[id] = intern(fnstr[id]);
		fnsymok[id] = 1;
	}
	return fnsym[id];
}

/* typ podle jména v těle funkce */
static int
typeclsid(int id, int *tyn)
{
	*tyn = -1;
	switch (fnw[id]) {
	case WInt8: return Ksb;
	case WUInt8: return Kub;
	case WInt16: return Ksh;
	case WUInt16: return Kuh;
	case WInt32: return Kw;
	case WInt64: return Kl;
	case WPtr: return Kl;
	case WFloat32: return Ks;
	case WFloat64: return Kd;
	case WEnv: return Kenv;
	}
	if (ntyp == 0)
		return Kx;
	*tyn = findtyp(fnstr[id], ntyp);
	if (*tyn >= 0)
		return Kc;
	return Kx;
}

static void
nameopd(Opd *o, int id)
{
	if (fnloc[id] != NLoc) {
		o->kind = OTmp;
		o->r = tmpid(id);
		o->cls = fnloc[id];
	} else {
		o->kind = OGlo;
		o->sym = symid(id);
		o->cls = Kl;
	}
}

static void parseopd(Opd *);

static void
parseann(Opd *o, int k, int tyn)
{
	next(); /* ( */
	if (peek() == Tid && (k == Ks || k == Kd)
	&& (strcmp(tk[itk].str, "inf") == 0 || strcmp(tk[itk].str, "nan") == 0)) {
		next();
		memset(o, 0, sizeof *o);
		o->kind = OFlt;
		o->flt = tstr;
		o->cls = Kx;
	} else {
		parseopd(o);
		if (o->ann != Kx)
			err("nested type annotation");
	}
	o->ann = k;
	o->tyn = tyn;
	expect(Trparen);
}

static void
parseopd(Opd *o)
{
	int t, k, w, tyn;

	memset(o, 0, sizeof *o);
	o->cls = Kx;
	o->ann = Kx;
	t = next();
	switch (t) {
	case Tint:
		o->kind = OInt;
		o->num = tnum;
		return;
	case Tflt:
		o->kind = OFlt;
		o->flt = tstr;
		return;
	case Tid:
		break;
	default:
		err("operand expected, got %s", tokname(t));
	}
	if (peek() == Tlparen) {
		w = fnw[tid];
		if (w == Wthread || w == Wextern) {
			next();
			if (next() != Tid)
				err("symbol name expected in %s(...)", kwname[w]);
			if (fnloc[tid] != NLoc)
				err("%s(...) needs a global symbol, %s is local", kwname[w], tstr);
			o->kind = OGlo;
			o->sym = symid(tid);
			o->cls = Kl;
			o->symty = w == Wthread ? SThr : SExt;
			expect(Trparen);
			return;
		}
		if (w == Wbits) {
			next();
			if (next() != Tint)
				err("integer expected in bits(...)");
			o->kind = OInt;
			o->num = tnum;
			o->rawbits = 1;
			expect(Trparen);
			return;
		}
		k = typeclsid(tid, &tyn);
		if (k != Kx) {
			parseann(o, k, tyn);
			return;
		}
	}
	nameopd(o, tid);
}

/* třída, kterou operand sám určuje; Kx pro literál bez anotace */
static int
opdcls(Opd *o)
{
	if (o->ann != Kx)
		return basecls(o->ann);
	return o->cls;
}

static Ref
mkref(Opd *o, int k)
{
	Con c;
	char *end;

	memset(&c, 0, sizeof c);
	switch (o->kind) {
	case OTmp:
		return o->r;
	case OGlo:
		c.type = CAddr;
		c.sym.id = o->sym;
		c.sym.type = o->symty;
		return newcon(&c, curf);
	case OInt:
		if ((k == Ks || k == Kd) && o->num != 0 && !o->rawbits)
			err("integer literal %"PRId64" used as %s; write a decimal point,"
				" e.g. %"PRId64".0, or bits(%"PRId64") for a bit pattern",
				o->num, clsname(k), o->num, o->num);
		c.type = CBits;
		c.bits.i = o->num;
		return newcon(&c, curf);
	case OFlt:
		c.type = CBits;
		end = o->flt;
		if (k == Ks) {
			c.bits.s = strtof(o->flt, &end);
			c.flt = 1;
		} else if (k == Kd) {
			c.bits.d = strtod(o->flt, &end);
			c.flt = 2;
		} else
			err("floating literal %s used as an integer", o->flt);
		if (*end)
			err("invalid floating literal %s", o->flt);
		return newcon(&c, curf);
	}
	die("unreachable");
}

/* anotace mimo store/call smí určit jen základní třídu */
static void
plain(Opd *o)
{
	if (o->ann > Kd)
		err("%s annotation is only valid in store or call",
			o->ann == Kc ? "aggregate" :
			o->ann == Kenv ? "Env" : clsname(o->ann));
}

/* --- funkce --- */

static Blk *
mkblk(char *name)
{
	Blk *b;

	b = newblk();
	b->id = nblk++;
	b->name = name;
	return b;
}

static Blk *
blkid(int id)
{
	if (!fnblk[id])
		fnblk[id] = mkblk(fnstr[id]);
	return fnblk[id];
}

static void
closeblk()
{
	idup(curb, insb, curi-insb);
	blink = &curb->link;
	curi = insb;
}

static void
emitins(int op, int k, Ref to, Ref a0, Ref a1)
{
	if (curi - insb >= NIns)
		err("too many instructions");
	curi->op = op;
	curi->cls = k;
	curi->to = to;
	curi->arg[0] = a0;
	curi->arg[1] = a1;
	curi++;
}

static void
startblk(Blk *b)
{
	if (curb && curb->jmp.type == Jxxx) {
		closeblk();
		curb->jmp.type = Jjmp;
		curb->s1 = b;
	}
	if (b->jmp.type != Jxxx)
		err("multiple definitions of block %s", b->name);
	*blink = b;
	curb = b;
	plink = &curb->phi;
}

/* argumenty volání: Type(hodnota), Env(hodnota), ... */
static void
parseargs()
{
	Opd o;
	int k, vararg, hasenv;
	Ref r;

	vararg = 0;
	hasenv = 0;
	expect(Tlparen);
	while (peek() != Trparen) {
		if (peek() == Tdots) {
			next();
			if (vararg)
				err("only one '...' allowed");
			vararg = 1;
			emitins(Oargv, Kw, R, R, R);
		} else {
			parseopd(&o);
			k = o.ann;
			if (k == Kx) {
				k = o.cls;
				if (k == Kx)
					err("call argument needs a type, e.g. Int32(1)");
			}
			if (k == Kenv) {
				if (hasenv)
					err("only one environment allowed");
				hasenv = 1;
				r = mkref(&o, Kl);
				emitins(Oarge, Kl, R, r, R);
			} else if (k == Kc) {
				r = mkref(&o, Kl);
				emitins(Oargc, Kl, R, TYPE(o.tyn), r);
			} else if (k >= Ksb) {
				r = mkref(&o, Kw);
				emitins(Oargsb+(k-Ksb), Kw, R, r, R);
			} else {
				r = mkref(&o, k);
				emitins(Oarg, k, R, r, R);
			}
		}
		if (peek() == Trparen)
			break;
		expect(Tcomma);
	}
	expect(Trparen);
}

static void
parsecall(Ref r, int k, int tyn)
{
	Opd o;
	Ref a0, a1;

	if (next() != Tid)
		err("function name expected after call");
	memset(&o, 0, sizeof o);
	nameopd(&o, tid);
	curf->leaf = 0;
	a0 = mkref(&o, Kl);
	parseargs();
	a1 = R;
	if (k == Kc) {
		k = Kl;
		a1 = TYPE(tyn);
	}
	if (k >= Ksb)
		k = Kw;
	emitins(Ocall, k, r, a0, a1);
}

static int
cmpsel(int p, int k)
{
	static int iop[] = {
		[Peq] = Oceqw, [Pne] = Ocnew,
		[Plt] = Ocsltw, [Ple] = Ocslew,
		[Pgt] = Ocsgtw, [Pge] = Ocsgew,
		[Pult] = Ocultw, [Pule] = Oculew,
		[Pugt] = Ocugtw, [Puge] = Ocugew,
		[Pord] = 0, [Punord] = 0,
	};
	static int fop[] = {
		[Peq] = Oceqs, [Pne] = Ocnes,
		[Plt] = Oclts, [Ple] = Ocles,
		[Pgt] = Ocgts, [Pge] = Ocges,
		[Pult] = 0, [Pule] = 0, [Pugt] = 0, [Puge] = 0,
		[Pord] = Ocos, [Punord] = Ocuos,
	};

	if (k == Kw || k == Kl) {
		if (p > Puge)
			err("%s needs floating operands", pseudoname[p]);
		return iop[p] + (k == Kl ? Oceql - Oceqw : 0);
	}
	if (p >= Pult && p <= Puge)
		err("%s needs integer operands", pseudoname[p]);
	return fop[p] + (k == Kd ? Oceqd - Oceqs : 0);
}

static int
nopds(Opd *o, int max)
{
	int n;

	expect(Tlparen);
	n = 0;
	while (peek() != Trparen) {
		if (n == max)
			err("too many operands");
		parseopd(&o[n++]);
		if (peek() != Tcomma)
			break;
		next();
	}
	expect(Trparen);
	return n;
}

static void
parseop(int w, Ref r, int k)
{
	Opd o[3];
	Ref a[2], packet, slot;
	int op, n, i, ak, kc, want;

	a[0] = R;
	a[1] = R;
	if (k >= Ksb)
		err("result of an instruction must be Int32, Int64, Ptr, Float32 or Float64");
	if (w >= WPseudo) {
		w -= WPseudo;
		want = w >= Pftoi ? 1 : 2;
		n = nopds(o, want);
		if (n != want)
			err("%s expects %d operand(s)", pseudoname[w], want);
		for (i=0; i<n; i++)
			plain(&o[i]);
		kc = opdcls(&o[0]);
		if (kc == Kx && n == 2)
			kc = opdcls(&o[1]);
		if (kc == Kx)
			err("cannot infer the operand type of %s; annotate an operand,"
				" e.g. Int64(1)", pseudoname[w]);
		switch (w) {
		case Pftoi:
		case Pftou:
			if (kc != Ks && kc != Kd)
				err("%s needs a Float32 or Float64 operand", pseudoname[w]);
			op = (w == Pftoi ? Ostosi : Ostoui) + (kc == Kd ? Odtosi - Ostosi : 0);
			break;
		case Pitof:
		case Putof:
			if (kc != Kw && kc != Kl)
				err("%s needs an Int32 or Int64 operand", pseudoname[w]);
			op = (w == Pitof ? Oswtof : Ouwtof) + (kc == Kl ? Osltof - Oswtof : 0);
			break;
		default:
			op = cmpsel(w, kc);
			break;
		}
		for (i=0; i<n; i++)
			a[i] = mkref(&o[i], kc);
		emitins(op, k, r, a[0], a[1]);
		return;
	}
	op = w - WOp;
	if (op == Oastore || op == Oafence)
		err("%s does not return a value", optab[op].name);
	if (op == Oacas) {
		if (k != Kw && k != Kl)
			err("atomic_cas requires Int32 or Int64 result");
		if (nopds(o, 3) != 3)
			err("atomic_cas expects pointer, expected and desired");
		for (i=0; i<3; i++)
			plain(&o[i]);
		a[0] = mkref(&o[0], Km);
		packet = newtmp("casargs", Kl, curf);
		slot = newtmp("casargs", Kl, curf);
		emitins(Oalloc8, Kl, packet, getcon(16, curf), R);
		emitins(Oadd, Kl, slot, packet, getcon(k == Kl ? 8 : 4, curf));
		emitins(k == Kl ? Ostorel : Ostorew, Kw, R, mkref(&o[1], k), packet);
		emitins(k == Kl ? Ostorel : Ostorew, Kw, R, mkref(&o[2], k), slot);
		emitins(Oacas, k, r, a[0], packet);
		return;
	}
	n = nopds(o, 2);
	for (i=0; i<n; i++) {
		plain(&o[i]);
		ak = optab[op].argcls[i][k];
		if (ak == Ke)
			err("invalid instruction type in %s", optab[op].name);
		if (ak == Kx)
			err("too many operands for %s", optab[op].name);
		a[i] = mkref(&o[i], ak);
	}
	emitins(op, k, r, a[0], a[1]);
}

static void
parsephi(Ref r, int k, PState ps)
{
	Ref arg[NPred];
	Blk *blk[NPred];
	Phi *phi;
	Opd o;
	int i;

	if (ps != PPhi || curb == curf->start)
		err("unexpected phi instruction");
	if (k >= Ksb)
		err("phi must be Int32, Int64, Ptr, Float32 or Float64");
	expect(Tlparen);
	i = 0;
	while (peek() != Trparen) {
		if (i == NPred)
			err("too many arguments");
		expect(Tid);
		blk[i] = blkid(tid);
		expect(Tcolon);
		parseopd(&o);
		plain(&o);
		arg[i] = mkref(&o, k);
		i++;
		if (peek() != Tcomma)
			break;
		next();
	}
	expect(Trparen);
	phi = alloc(sizeof *phi);
	phi->to = r;
	phi->cls = k;
	phi->arg = vnew(i, sizeof arg[0], PFn);
	memcpy(phi->arg, arg, i * sizeof arg[0]);
	phi->blk = vnew(i, sizeof blk[0], PFn);
	memcpy(phi->blk, blk, i * sizeof blk[0]);
	phi->narg = i;
	*plink = phi;
	plink = &phi->link;
}

static void
parsestore()
{
	Opd v, p;
	Ref a0, a1;
	int k, op;

	expect(Tlparen);
	parseopd(&v);
	expect(Tcomma);
	parseopd(&p);
	plain(&p);
	expect(Trparen);
	k = v.ann != Kx ? v.ann : v.cls;
	switch (k) {
	case Ksb:
	case Kub:
		op = Ostoreb;
		k = Kw;
		break;
	case Ksh:
	case Kuh:
		op = Ostoreh;
		k = Kw;
		break;
	case Kw: op = Ostorew; break;
	case Kl: op = Ostorel; break;
	case Ks: op = Ostores; break;
	case Kd: op = Ostored; break;
	default:
		err("store needs a typed value, e.g. store(Int32(0), p)");
	}
	a0 = mkref(&v, k);
	a1 = mkref(&p, Km);
	emitins(op, Kw, R, a0, a1);
}

static Blk *
jumptarget()
{
	Blk *b;

	expect(Tid);
	b = blkid(tid);
	if (b == curf->start)
		err("invalid jump to the start block");
	return b;
}

static PState
parseline(PState ps)
{
	Opd o[3];
	Ref r, a0, a1;
	Con *c;
	Blk *b;
	int t, w, k, tyn, n;

	t = nextnl();
	if (t == Tfnend)
		return PEnd;
	if (t == Tid && peek() == Tcolon) {
		b = blkid(tid);
		next();
		startblk(b);
		expect(Tnl);
		return PPhi;
	}
	if (ps == PLbl && curb)
		err("label or end expected after a jump");
	if (!curb) {
		/* tělo bez úvodního návěští má skrytý vstupní blok */
		startblk(mkblk(""));
		ps = PPhi;
	}
	if (t != Tid)
		err("instruction expected, got %s", tokname(t));
	w = fnw[tid];
	switch (w) {
	case Wreturn:
		curb->jmp.type = Jretw + rcls;
		if (peek() == Tnl)
			curb->jmp.type = Jret0;
		else if (rcls != K0) {
			parseopd(o);
			plain(o);
			curb->jmp.arg = mkref(o, basecls(rcls));
		}
		goto Close;
	case Wgoto:
		curb->jmp.type = Jjmp;
		curb->s1 = jumptarget();
		goto Close;
	case Wif:
		curb->jmp.type = Jjnz;
		parseopd(o);
		plain(o);
		curb->jmp.arg = mkref(o, Kw);
		if (next() != Tid || !iskw(Wgoto))
			err("goto expected after the if condition");
		curb->s1 = jumptarget();
		if (next() != Tid || !iskw(Welse))
			err("else expected in if ... goto ... else ...");
		curb->s2 = jumptarget();
		goto Close;
	case Whalt:
		curb->jmp.type = Jhlt;
	Close:
		expect(Tnl);
		closeblk();
		return PLbl;
	case Wcall:
		parsecall(R, Kw, 0);
		break;
	case Wstore:
		parsestore();
		break;
	case Wvastart:
		if (!curf->vararg)
			err("cannot use vastart in non-variadic function");
		if (nopds(o, 1) != 1)
			err("vastart expects one operand");
		plain(o);
		emitins(Ovastart, Kw, R, mkref(o, Km), R);
		break;
	case Wblit:
		if (nopds(o, 3) != 3)
			err("blit expects source, destination and size");
		plain(&o[0]);
		plain(&o[1]);
		a0 = mkref(&o[0], Km);
		a1 = mkref(&o[1], Km);
		if (o[2].kind != OInt || o[2].ann != Kx)
			err("blit size must be constant");
		r = mkref(&o[2], Kl);
		c = &curf->con[r.val];
		r = INT(c->bits.i);
		if (rsval(r) < 0 || rsval(r) != c->bits.i)
			err("invalid blit size");
		if (curi - insb >= NIns-1)
			err("too many instructions");
		emitins(Oblit0, Kw, R, a0, a1);
		emitins(Oblit1, Kw, R, r, R);
		break;
	case Wdbgloc:
		n = nopds(o, 2);
		if (n < 1 || o[0].kind != OInt || (n == 2 && o[1].kind != OInt))
			err("dbgloc expects a line and an optional column");
		a0 = INT(o[0].num);
		if (a0.val != o[0].num)
			err("line number too big");
		a1 = INT(0);
		if (n == 2) {
			a1 = INT(o[1].num);
			if (a1.val != o[1].num)
				err("column number too big");
		}
		emitins(Odbgloc, Kw, R, a0, a1);
		break;
	default:
		if (w == WOp+Oafence) {
			if (nopds(o, 0) != 0)
				err("atomic_fence expects no operands");
			emitins(Oafence, Kw, R, R, R);
			break;
		}
		if (w == WOp+Oastore) {
			if (nopds(o, 2) != 2)
				err("atomic_store expects pointer and value");
			plain(&o[0]);
			plain(&o[1]);
			k = opdcls(&o[1]);
			if (k != Kw && k != Kl)
				err("atomic_store value needs Int32 or Int64 type");
			emitins(Oastore, k, R, mkref(&o[0], Km), mkref(&o[1], k));
			break;
		}
		/* Type(jméno) = ... */
		k = typeclsid(tid, &tyn);
		if (k == Kx || k == Kenv) {
			if (w >= WOp)
				err("%s needs a result, e.g. Int32(x) = %s(...)", tstr, tstr);
			err("unknown instruction or type %s", tstr);
		}
		expect(Tlparen);
		expect(Tid);
		r = tmpid(tid);
		expect(Trparen);
		expect(Teq);
		w = Wnone;
		if (peek() == Tid) {
			w = fnw[tk[itk].id];
			if (w != Wcall && w != Wphi && (w < WOp || peek2() != Tlparen))
				w = Wnone;
		}
		if (w == Wcall) {
			next();
			parsecall(r, k, tyn);
		} else if (w == Wphi) {
			next();
			if (k == Kc)
				err("phi cannot have an aggregate type");
			parsephi(r, k, ps);
			expect(Tnl);
			return PPhi;
		} else if (k == Kc) {
			err("aggregate type is only valid for call results");
		} else if (w != Wnone) {
			next();
			parseop(w, r, k);
		} else if (peek() == Tid && peek2() == Tlparen
		&& fnw[tk[itk].id] != Wthread && fnw[tk[itk].id] != Wextern
		&& fnw[tk[itk].id] != Wbits
		&& typeclsid(tk[itk].id, &tyn) == Kx) {
			next();
			err("unknown instruction %s", tstr);
		} else {
			/* Type(x) = hodnota je kopie */
			if (k >= Ksb)
				err("result of copy must be Int32, Int64, Ptr, Float32 or Float64");
			parseopd(o);
			plain(o);
			emitins(Ocopy, k, r, mkref(o, k), R);
		}
		break;
	}
	expect(Tnl);
	return PIns;
}

static int
usecheck(Ref r, int k, Fn *fn)
{
	return rtype(r) != RTmp || fn->tmp[r.val].cls == k
		|| (fn->tmp[r.val].cls == Kl && k == Kw);
}

static void
typecheck(Fn *fn)
{
	Blk *b;
	Phi *p;
	Ins *i;
	uint n;
	int k;
	Tmp *t;
	Ref r;
	BSet pb[1], ppb[1];

	fillpreds(fn);
	bsinit(pb, fn->nblk);
	bsinit(ppb, fn->nblk);
	for (b=fn->start; b; b=b->link) {
		for (p=b->phi; p; p=p->link)
			fn->tmp[p->to.val].cls = p->cls;
		for (i=b->ins; i<&b->ins[b->nins]; i++)
			if (rtype(i->to) == RTmp) {
				t = &fn->tmp[i->to.val];
				if (clsmerge(&t->cls, i->cls))
					err("value %s is assigned with"
						" multiple types", t->name);
			}
	}
	for (b=fn->start; b; b=b->link) {
		bszero(pb);
		for (n=0; n<b->npred; n++)
			bsset(pb, b->pred[n]->id);
		for (p=b->phi; p; p=p->link) {
			bszero(ppb);
			t = &fn->tmp[p->to.val];
			for (n=0; n<p->narg; n++) {
				k = t->cls;
				if (bshas(ppb, p->blk[n]->id))
					err("multiple entries for %s in phi %s",
						p->blk[n]->name, t->name);
				if (!usecheck(p->arg[n], k, fn))
					err("invalid type for operand %s in phi %s",
						fn->tmp[p->arg[n].val].name, t->name);
				bsset(ppb, p->blk[n]->id);
			}
			if (!bsequal(pb, ppb))
				err("predecessors not matched in phi %s", t->name);
		}
		for (i=b->ins; i<&b->ins[b->nins]; i++)
			for (n=0; n<2; n++) {
				k = optab[i->op].argcls[n][i->cls];
				r = i->arg[n];
				t = &fn->tmp[r.val];
				if (k == Ke)
					err("invalid instruction type in %s",
						optab[i->op].name);
				if (rtype(r) == RType)
					continue;
				if (rtype(r) != -1 && k == Kx)
					err("no %s operand expected in %s",
						n == 1 ? "second" : "first",
						optab[i->op].name);
				if (rtype(r) == -1 && k != Kx)
					err("missing %s operand in %s",
						n == 1 ? "second" : "first",
						optab[i->op].name);
				if (!usecheck(r, k, fn))
					err("invalid type for %s operand %s in %s",
						n == 1 ? "second" : "first",
						t->name, optab[i->op].name);
			}
		r = b->jmp.arg;
		if (isret(b->jmp.type)) {
			if (b->jmp.type == Jretc)
				k = Kl;
			else if (b->jmp.type >= Jretsb)
				k = Kw;
			else
				k = b->jmp.type - Jretw;
			if (!usecheck(r, k, fn))
				goto JErr;
		}
		if (b->jmp.type == Jjnz && !usecheck(r, Kw, fn))
		JErr:
			err("invalid type for jump argument %s in block %s",
				fn->tmp[r.val].name, b->name);
		if (b->s1 && b->s1->jmp.type == Jxxx)
			err("block %s is used undefined", b->s1->name);
		if (b->s2 && b->s2->jmp.type == Jxxx)
			err("block %s is used undefined", b->s2->name);
	}
}

/* parametry: Type(jméno), Env(jméno), ... */
static int
parseparams()
{
	int k, tyn, vararg, hasenv;
	Ref r;

	vararg = 0;
	hasenv = 0;
	expect(Tlparen);
	while (peek() != Trparen) {
		if (vararg)
			err("no parameters allowed after '...'");
		if (peek() == Tdots) {
			next();
			vararg = 1;
		} else {
			if (next() != Tid)
				err("parameter expected, e.g. Int32(x)");
			k = typeclsid(tid, &tyn);
			if (k == Kx)
				err("unknown parameter type %s", tstr);
			expect(Tlparen);
			expect(Tid);
			if (fnloc[tid] == NLoc)
				fnloc[tid] = basecls(k);
			r = tmpid(tid);
			expect(Trparen);
			if (k == Kenv) {
				if (hasenv)
					err("only one environment allowed");
				hasenv = 1;
				emitins(Opare, Kl, r, R, R);
			} else if (k == Kc)
				emitins(Oparc, Kl, r, TYPE(tyn), R);
			else if (k >= Ksb)
				emitins(Oparsb+(k-Ksb), Kw, r, R, R);
			else
				emitins(Opar, k, r, R, R);
		}
		if (peek() == Trparen)
			break;
		expect(Tcomma);
	}
	expect(Trparen);
	return vararg;
}

static void
tokput(int t, int line)
{
	Tok *k;
	int n;

	if (ntk+2 > tkcap) {
		tkcap = 2 * (ntk+2);
		vgrow(&tk, tkcap);
	}
	k = &tk[ntk++];
	k->t = t;
	k->line = line;
	k->num = lexnum;
	k->id = 0;
	k->str = 0;
	if (t == Tid) {
		k->id = fnname(idpos, idlen, idhash);
		k->str = fnstr[k->id];
	} else if (t == Tflt || t == Tstr) {
		n = strlen(lexbuf);
		k->str = alloc(n+1);
		memcpy(k->str, lexbuf, n+1);
	}
}

/* načte tělo až po samostatné end a předem zapíše lokální jména */
/* načte hlavičku a tělo funkce až po samostatné end a předem
 * zapíše lokální jména */
static void
readbody()
{
	int t, atstart, line, k, tyn;
	uint64_t endpos;
	uint i;

	ntk = 0;
	atstart = 0;
	depth = 0;
	collect = 1;
	for (;;) {
		line = lnum;
		t = lex();
		if (t == Teof)
			err("missing end of function");
		if (atstart && t == Tid && idlen == 3
		&& memcmp(&src[idpos], "end", 3) == 0) {
			line = lnum;
			endpos = idpos;
			t = lex();
			if (t == Tcolon) {
				idpos = endpos;
				idlen = 3;
				idhash = hash("end");
				tokput(Tid, line);
				tokput(Tcolon, line);
				atstart = 0;
				continue;
			}
			if (t != Tnl && t != Teof)
				err("newline expected after end");
			if (t == Teof)
				thead = Teof;
			tokput(Tfnend, line);
			break;
		}
		/* newline patří řádku, který ukončuje */
		tokput(t, t == Tnl ? line : lnum);
		atstart = t == Tnl;
	}
	collect = 0;
	for (i=0; i+4<ntk; i++)
		if ((i == 0 || tk[i-1].t == Tnl)
		&& tk[i].t == Tid && tk[i+1].t == Tlparen
		&& tk[i+2].t == Tid && tk[i+3].t == Trparen
		&& tk[i+4].t == Teq) {
			k = typeclsid(tk[i].id, &tyn);
			if (k != Kx && k != Kenv && fnloc[tk[i+2].id] == NLoc)
				fnloc[tk[i+2].id] = basecls(k);
		}
	itk = 0;
}

static Fn *
parsefn(Lnk *lnk)
{
	Blk *b;
	int i, t;
	PState ps;

	curb = 0;
	nblk = 0;
	curi = insb;
	curf = alloc(sizeof *curf);
	curf->ntmp = 0;
	curf->ncon = 2;
	curf->tmp = vnew(curf->ntmp, sizeof curf->tmp[0], PFn);
	curf->con = vnew(curf->ncon, sizeof curf->con[0], PFn);
	for (i=0; i<Tmp0; ++i)
		if (T.fpr0 <= i && i < T.fpr0 + T.nfpr)
			newtmp(0, Kd, curf);
		else
			newtmp(0, Kl, curf);
	curf->con[0].type = CBits;
	curf->con[0].bits.i = 0xdeaddead;  /* UNDEF */
	curf->con[1].type = CBits;
	curf->lnk = *lnk;
	curf->leaf = 1;
	blink = &curf->start;
	curf->retty = Kx;
	fnreset();
	readbody();
	buffered = 1;
	if (next() != Tid)
		err("function name expected");
	curf->name = tstr;
	curf->vararg = parseparams();
	rcls = K0;
	t = next();
	if (t == Tarrow) {
		if (next() != Tid)
			err("return type expected after ->");
		rcls = typeclsid(tid, &curf->retty);
		if (rcls == Kx || rcls == Kenv)
			err("invalid return type %s", tstr);
		if (rcls != Kc)
			curf->retty = Kx;
		t = next();
	}
	if (t == Tcolon)
		t = next();
	if (t != Tnl)
		err("newline expected after the function header");
	ps = PLbl;
	do
		ps = parseline(ps);
	while (ps != PEnd);
	buffered = 0;
	if (!curb)
		err("empty function");
	if (curb->jmp.type == Jxxx)
		err("last block misses jump");
	curf->mem = vnew(0, sizeof curf->mem[0], PFn);
	curf->nmem = 0;
	curf->nblk = nblk;
	curf->rpo = vnew(nblk, sizeof curf->rpo[0], PFn);
	for (b=curf->start; b; b=b->link)
		b->dlink = 0;
	typecheck(curf);
	return curf;
}

/* --- typy a data --- */

static int
alignlog(int64_t n)
{
	int al;

	if (n <= 0 || (n & (n-1)) != 0)
		err("alignment must be a power of two");
	for (al=0; n /= 2; al++)
		;
	return al;
}

static void
parsefields(Field *fld, Typ *ty)
{
	Typ *ty1;
	int n, c, a, al, type, k, tyn;
	uint64_t sz, s;

	n = 0;
	sz = 0;
	al = ty->align;
	expect(Tlparen);
	while (peek() != Trparen) {
		ty1 = 0;
		if (next() != Tid)
			err("invalid type member specifier");
		k = typecls(tstr, &tyn, ntyp-1);
		switch (k) {
		default: err("invalid type member specifier %s", tstr);
		case Kd: type = Fd; s = 8; a = 3; break;
		case Kl: type = Fl; s = 8; a = 3; break;
		case Ks: type = Fs; s = 4; a = 2; break;
		case Kw: type = Fw; s = 4; a = 2; break;
		case Ksh: case Kuh: type = Fh; s = 2; a = 1; break;
		case Ksb: case Kub: type = Fb; s = 1; a = 0; break;
		case Kc:
			type = FTyp;
			ty1 = &typ[tyn];
			s = ty1->size;
			a = ty1->align;
			break;
		}
		if (a > al)
			al = a;
		a = (1 << a) - 1;
		a = ((sz + a) & ~a) - sz;
		if (a) {
			if (n < NField) {
				/* zarovnání */
				fld[n].type = FPad;
				fld[n].len = a;
				n++;
			}
		}
		c = 1;
		if (peek() == Tstar) {
			next();
			if (next() != Tint || tnum < 0)
				err("member count expected after *");
			c = tnum;
		}
		sz += a + c*s;
		if (type == FTyp)
			s = ty1 - typ;
		for (; c>0 && n<NField; c--, n++) {
			fld[n].type = type;
			fld[n].len = s;
		}
		if (peek() != Tcomma)
			break;
		next();
	}
	expect(Trparen);
	fld[n].type = FEnd;
	a = 1 << al;
	if (sz < ty->size)
		sz = ty->size;
	ty->size = (sz + a - 1) & -a;
	ty->align = al;
}

static void
parsetyp()
{
	Typ *ty;
	int t, tyn;
	uint n;

	/* pozor při rozšiřování syntaxe o vnořené typy:
	 * ukazatele do typ[] se mohou zneplatnit */
	vgrow(&typ, ntyp+1);
	ty = &typ[ntyp++];
	ty->isdark = 0;
	ty->isunion = 0;
	ty->align = -1;
	ty->size = 0;
	ty->nunion = 0;
	if (next() != Tid)
		err("type name expected");
	if (namefind(tstr) != Wnone || typecls(tstr, &tyn, ntyp-1) != Kx)
		err("type name %s is reserved or already defined", tstr);
	ty->name = strf(PHeap, "%s", tstr);
	t = next();
	if (t == Tid && iskw(Walign)) {
		if (next() != Tint)
			err("alignment expected");
		ty->align = alignlog(tnum);
		t = next();
	}
	if (t != Teq)
		err("= expected in type definition");
	if (next() != Tid)
		err("struct, union or opaque expected");
	switch (namefind(tstr)) {
	case Wopaque:
		expect(Tlparen);
		if (next() != Tint || tnum < 0)
			err("opaque size expected");
		ty->isdark = 1;
		ty->size = tnum;
		if (ty->align == -1)
			err("opaque types need alignment");
		expect(Trparen);
		break;
	case Wunion:
		ty->isunion = 1;
		n = 0;
		ty->fields = vnew(1, sizeof ty->fields[0], PHeap);
		expect(Tlparen);
		while (peek() != Trparen) {
			if (next() != Tid || !iskw(Wstruct))
				err("union members must be struct(...)");
			vgrow(&ty->fields, n+1);
			parsefields(ty->fields[n++], ty);
			if (peek() != Tcomma)
				break;
			next();
		}
		expect(Trparen);
		if (n == 0)
			err("empty union");
		ty->nunion = n;
		break;
	case Wstruct:
		ty->fields = vnew(1, sizeof ty->fields[0], PHeap);
		parsefields(ty->fields[0], ty);
		ty->nunion = 1;
		break;
	default:
		err("struct, union or opaque expected");
	}
	t = next();
	if (t == Teof)
		thead = Teof;
	else if (t != Tnl)
		err("newline expected after type definition");
}

static void
parsedatval(Dat *d, int k, void cb(Dat *))
{
	int t;

	d->isstr = 0;
	d->isref = 0;
	memset(&d->u, 0, sizeof d->u);
	t = next();
	switch (t) {
	case Tint:
		if ((k == Ks || k == Kd) && tnum != 0)
			err("integer literal %"PRId64" used as %s; write a decimal point",
				tnum, clsname(k));
		d->u.num = tnum;
		break;
	case Tflt:
		if (k == Ks)
			d->u.flts = strtof(tstr, 0);
		else if (k == Kd)
			d->u.fltd = strtod(tstr, 0);
		else
			err("floating literal %s used as an integer", tstr);
		break;
	case Tstr:
		d->isstr = 1;
		d->u.str = strf(PFn, "%s", tstr);
		break;
	case Tid:
		if (iskw(Wbits) && peek() == Tlparen) {
			next();
			if (next() != Tint)
				err("integer expected in bits(...)");
			d->u.num = tnum;
			expect(Trparen);
			break;
		}
		if (k == Ks || k == Kd)
			err("symbol %s used as a floating value", tstr);
		d->isref = 1;
		d->u.ref.name = strf(PFn, "%s", tstr);
		d->u.ref.off = 0;
		if (peek() == Tplus) {
			next();
			if (next() != Tint)
				err("integer offset expected after +");
			d->u.ref.off = tnum;
		}
		break;
	default:
		err("constant expected in data, got %s", tokname(t));
	}
	cb(d);
}

static void
parsedatitem(Dat *d, void cb(Dat *))
{
	int t, k, tyn;

	t = next();
	if (t == Tstr) {
		d->type = DB;
		d->isstr = 1;
		d->isref = 0;
		memset(&d->u, 0, sizeof d->u);
		d->u.str = strf(PFn, "%s", tstr);
		cb(d);
		return;
	}
	if (t != Tid)
		err("data item expected, e.g. Int32(1) or \"text\"");
	if (iskw(Wzero)) {
		expect(Tlparen);
		if (next() != Tint || tnum < 0)
			err("zero(N) needs a byte count");
		d->type = DZ;
		d->isstr = 0;
		d->isref = 0;
		memset(&d->u, 0, sizeof d->u);
		d->u.num = tnum;
		expect(Trparen);
		cb(d);
		return;
	}
	k = typecls(tstr, &tyn, ntyp);
	switch (k) {
	case Ksb: case Kub: d->type = DB; break;
	case Ksh: case Kuh: d->type = DH; break;
	case Kw: case Ks: d->type = DW; break;
	case Kl: case Kd: d->type = DL; break;
	default:
		err("invalid data item type %s", tstr);
	}
	expect(Tlparen);
	if (peek() == Trparen)
		err("data item needs at least one value");
	for (;;) {
		parsedatval(d, k, cb);
		if (peek() != Tcomma)
			break;
		next();
	}
	expect(Trparen);
}

static void
parsedat(void cb(Dat *), Lnk *lnk)
{
	char *name;
	int t;
	Dat d;

	if (next() != Tid)
		err("data name expected");
	name = strf(PFn, "%s", tstr);
	t = next();
	lnk->align = 8;
	if (t == Tid && iskw(Walign)) {
		if (next() != Tint)
			err("alignment expected");
		if (tnum <= 0 || tnum > CHAR_MAX
		|| (tnum & (tnum-1)) != 0)
			err("invalid alignment");
		lnk->align = tnum;
		t = next();
	}
	if (t != Teq)
		err("= expected after data name");
	if (lnk->thread && T.windows)
		err("thread-local data is not supported on %s", T.name);
	d.type = DStart;
	d.name = name;
	d.lnk = lnk;
	cb(&d);

	if (peek() == Tlbrack) {
		next();
		while (peek() != Trbrack) {
			parsedatitem(&d, cb);
			if (peek() != Tcomma)
				break;
			next();
		}
		expect(Trbrack);
	} else
		parsedatitem(&d, cb);
	t = next();
	if (t == Teof)
		thead = Teof;
	else if (t != Tnl)
		err("newline expected after data definition");
	d.type = DEnd;
	cb(&d);
}

static int
parselnk(Lnk *lnk)
{
	int t, w, haslnk;

	for (haslnk=0;; haslnk=1) {
		t = nextnl();
		if (t == Teof) {
			if (haslnk)
				err("definition expected after linkage");
			return Teof;
		}
		if (t != Tid)
			err("top-level definition expected");
		w = namefind(tstr);
		switch (w) {
		case Wexport:
			lnk->export = 1;
			break;
		case Wthread:
			lnk->thread = 1;
			break;
		case Wcommon:
			lnk->common = 1;
			break;
		case Wsection:
			if (lnk->sec)
				err("only one section allowed");
			if (next() != Tstr)
				err("section \"name\" expected");
			lnk->sec = strf(PFn, "%s", tstr);
			if (peek() == Tstr) {
				next();
				lnk->secf = strf(PFn, "%s", tstr);
			}
			break;
		default:
			if (w == Wfunction && lnk->thread)
				err("only data may have thread linkage");
			if (haslnk && w != Wdata && w != Wfunction)
				err("only data and function have linkage");
			return w;
		}
	}
}

/* celý vstup do paměti; lexer pak čte bajty bez volání stdio */
static void
readall(FILE *f)
{
	uint64_t cap, n;

	cap = 1 << 16;
	src = emalloc(cap);
	slen = 0;
	for (;;) {
		if (cap - slen < 4096 + 2) {
			cap *= 2;
			src = realloc(src, cap);
			if (!src)
				die("out of memory");
		}
		n = fread(&src[slen], 1, cap - slen - 2, f);
		if (n == 0)
			break;
		slen += n;
	}
	src[slen] = 0;
	src[slen+1] = 0;
	spos = 0;
}

void
parse(FILE *f, char *path, void dbgfile(char *), void data(Dat *), void func(Fn *))
{
	Lnk lnk;
	uint n;
	int t, definition, irseen, abiseen;

	lexinit();
	readall(f);
	inpath = path;
	lnum = 1;
	depth = 0;
	thead = Txxx;
	buffered = 0;
	ntyp = 0;
	typ = vnew(0, sizeof typ[0], PHeap);
	definition = irseen = abiseen = 0;
	for (;;) {
		lnk = (Lnk){0};
		switch (parselnk(&lnk)) {
		default:
			err("top-level definition expected");
		case Wirversion:
		case Wruntimeabi:
			/* Only header metadata is handled here. It does not certify
			 * an externally linked runtime or change generated code. */
			if (definition)
				err("version headers must precede definitions");
			if (iskw(Wirversion)) {
				if (irseen)
					err("duplicate ir_version header");
				irseen = 1;
				expect(Tstr);
				if (strcmp(tstr, "\"" LUXIR_IR_VERSION "\"") != 0)
					err("unsupported IR version %s; supported: " LUXIR_IR_VERSION, tstr);
			} else {
				if (abiseen)
					err("duplicate runtime_abi header");
				abiseen = 1;
				expect(Tstr);
				if (strcmp(tstr, "\"" LUXIR_RUNTIME_ABI "\"") != 0)
					err("unsupported runtime ABI %s; supported: " LUXIR_RUNTIME_ABI, tstr);
			}
			t = next();
			if (t == Teof)
				thead = Teof;
			else if (t != Tnl)
				err("newline expected after version header");
			break;
		case Wfile:
			definition = 1;
			expect(Tstr);
			dbgfile(tstr);
			t = next();
			if (t == Teof)
				thead = Teof;
			else if (t != Tnl)
				err("newline expected after file");
			break;
		case Wfunction:
			definition = 1;
			lnk.align = 16;
			func(parsefn(&lnk));
			break;
		case Wdata:
			definition = 1;
			parsedat(data, &lnk);
			break;
		case Wtype:
			definition = 1;
			parsetyp();
			break;
		case Teof:
			for (n=0; n<ntyp; n++) {
				free(typ[n].name);
				if (typ[n].nunion)
					vfree(typ[n].fields);
			}
			vfree(typ);
			free(src);
			src = 0;
			return;
		}
	}
}

/* --- ladicí výpis ve tvaru LuxIR --- */

static void
printcon(Con *c, FILE *f)
{
	switch (c->type) {
	case CUndef:
		break;
	case CAddr:
		if (c->sym.type & SExt)
			fprintf(f, "extern(");
		if (c->sym.type & SThr)
			fprintf(f, "thread(");
		fprintf(f, "%s", str(c->sym.id));
		if (c->sym.type & SThr)
			fprintf(f, ")");
		if (c->sym.type & SExt)
			fprintf(f, ")");
		if (c->bits.i)
			fprintf(f, "%+"PRIi64, c->bits.i);
		break;
	case CBits:
		if (c->flt == 1)
			fprintf(f, "Float32(%f)", c->bits.s);
		else if (c->flt == 2)
			fprintf(f, "Float64(%lf)", c->bits.d);
		else
			fprintf(f, "%"PRIi64, c->bits.i);
		break;
	}
}

void
printref(Ref r, Fn *fn, FILE *f)
{
	int i;
	Mem *m;

	switch (rtype(r)) {
	case RTmp:
		if (r.val < Tmp0)
			fprintf(f, "R%d", r.val);
		else
			fprintf(f, "%s", fn->tmp[r.val].name);
		break;
	case RCon:
		if (req(r, UNDEF))
			fprintf(f, "UNDEF");
		else
			printcon(&fn->con[r.val], f);
		break;
	case RSlot:
		fprintf(f, "S%d", rsval(r));
		break;
	case RCall:
		fprintf(f, "%04x", r.val);
		break;
	case RType:
		fprintf(f, "%s", typ[r.val].name);
		break;
	case RMem:
		i = 0;
		m = &fn->mem[r.val];
		fputc('[', f);
		if (m->offset.type != CUndef) {
			printcon(&m->offset, f);
			i = 1;
		}
		if (!req(m->base, R)) {
			if (i)
				fprintf(f, " + ");
			printref(m->base, fn, f);
			i = 1;
		}
		if (!req(m->index, R)) {
			if (i)
				fprintf(f, " + ");
			fprintf(f, "%d * ", m->scale);
			printref(m->index, fn, f);
		}
		fputc(']', f);
		break;
	case RInt:
		fprintf(f, "%d", rsval(r));
		break;
	case -1:
		fprintf(f, "R");
		break;
	}
}

void
printfn(Fn *fn, FILE *f)
{
	static char *ktoa[] = {"Int32", "Int64", "Float32", "Float64"};
	static char *jtoa[NJmp] = {
	#define X(j) [J##j] = #j,
		JMPS(X)
	#undef X
	};
	Blk *b;
	Phi *p;
	Ins *i;
	uint n;

	fprintf(f, "function %s():\n", fn->name);
	for (b=fn->start; b; b=b->link) {
		if (b->name[0])
			fprintf(f, "%s:\n", b->name);
		else
			fprintf(f, "# vstupní blok\n");
		for (p=b->phi; p; p=p->link) {
			fprintf(f, "    %s(", ktoa[p->cls]);
			printref(p->to, fn, f);
			fprintf(f, ") = phi(");
			assert(p->narg);
			for (n=0;; n++) {
				fprintf(f, "%s: ", p->blk[n]->name);
				printref(p->arg[n], fn, f);
				if (n == p->narg-1) {
					fprintf(f, ")\n");
					break;
				} else
					fprintf(f, ", ");
			}
		}
		for (i=b->ins; i<&b->ins[b->nins]; i++) {
			fprintf(f, "    ");
			if (!req(i->to, R)) {
				fprintf(f, "%s(", ktoa[i->cls]);
				printref(i->to, fn, f);
				fprintf(f, ") = ");
			}
			assert(optab[i->op].name);
			fprintf(f, "%s", optab[i->op].name);
			if (req(i->to, R))
				switch (i->op) {
				case Oarg:
				case Oswap:
				case Oxcmp:
				case Oacmp:
				case Oacmn:
				case Oafcmp:
				case Oxtest:
				case Oxdiv:
				case Oxidiv:
					fprintf(f, ".%s", ktoa[i->cls]);
				}
			fprintf(f, "(");
			if (!req(i->arg[0], R))
				printref(i->arg[0], fn, f);
			if (!req(i->arg[1], R)) {
				fprintf(f, ", ");
				printref(i->arg[1], fn, f);
			}
			fprintf(f, ")\n");
		}
		switch (b->jmp.type) {
		case Jret0:
		case Jretsb:
		case Jretub:
		case Jretsh:
		case Jretuh:
		case Jretw:
		case Jretl:
		case Jrets:
		case Jretd:
		case Jretc:
			fprintf(f, "    return");
			if (b->jmp.type != Jret0 || !req(b->jmp.arg, R)) {
				fprintf(f, " ");
				printref(b->jmp.arg, fn, f);
			}
			fprintf(f, "\n");
			break;
		case Jhlt:
			fprintf(f, "    halt\n");
			break;
		case Jjmp:
			if (b->s1 != b->link)
				fprintf(f, "    goto %s\n", b->s1->name);
			break;
		case Jjnz:
			fprintf(f, "    if ");
			printref(b->jmp.arg, fn, f);
			fprintf(f, " goto %s else %s\n", b->s1->name, b->s2->name);
			break;
		default:
			fprintf(f, "    %s ", jtoa[b->jmp.type]);
			assert(b->s1 && b->s2);
			fprintf(f, "%s, %s\n", b->s1->name, b->s2->name);
			break;
		}
	}
	fprintf(f, "end\n");
}

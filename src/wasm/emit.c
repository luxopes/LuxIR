#include "../all.h"

/* WebAssembly (wasm32, textový formát pro wat2wasm).
 *
 * Hodnoty mají třídy jako jinde (Kw, Kl, Ks, Kd); adresy jsou 64bitová
 * slova a před přístupem do lineární paměti se zkrátí na 32 bitů. Všechny
 * funkce mají jednotné ABI: N parametrů i64 a výsledek i64 (Kw rozšířené,
 * Ks/Kd přeložené bitově). Nepřímá volání jdou přes tabulku podle počtu
 * argumentů.
 *
 * Řízení toku: smyčka s br_table přes všechny bloky (pořadí rpo); skok na
 * následující blok je propad. Dočasné hodnoty jsou wasm lokály. Každá
 * hodnota Kl se při definici uloží také do rámce na stínovém zásobníku
 * v lineární paměti (globál $sp), aby ji konzervativní GC viděl jako kořen;
 * GC nehýbe objekty, takže se po volání nic nenačítá zpět.
 *
 * Adresy symbolů jsou globály $a.<jméno>: data a tabulka funkcí se skládají
 * až na konci modulu (wasm_emitfin), takže nezáleží na pořadí definic.
 * Symboly, které LuxIR nedefinuje (data vrstvy architektury), dodá okolní
 * text modulu. Výstup je obsah (module ...) bez obalu.
 *
 * Prolog s rámcem kontroluje mez stínového zásobníku (globál
 * $__stack_limit): při přetečení volá $__stack_overflow (dodá okolní text,
 * nevrací se) místo přepsání paměti pod zásobníkem.
 *
 * S volbou -g (wasmthreads, vlákna podle wasi-threads) je modul určený pro
 * více instancí nad jednou sdílenou pamětí: paměť dodá okolní text (import
 * shared, nejméně $__memory_pages stránek), data jsou pasivní a do paměti
 * je zapíše jen $__wasm_init_memory (volá ji start hlavního vlákna) a na
 * vstupu funkce a na každém zpětném skoku je bezpečný bod pro GC: je-li
 * slovo na adrese $__safepoint_flag nenulové, volá se $__safepoint.
 */

enum {
	DataBase = 4096,     /* pod tím data vrstvy architektury */
	StackBytes = 8 << 20 /* stínový zásobník */
};

typedef struct WRef WRef;
typedef struct WSym WSym;

struct WRef {   /* odkaz v datech: posun, symbol, přičtená hodnota, šířka */
	int64_t at;
	uint32_t sym;
	int64_t add;
	int size;
};

struct WSym {
	uint32_t id;
	char isdata;
	char isfunc;
	char used;   /* adresa použitá v kódu nebo datech */
	int64_t addr; /* data: adresa; funkce: index v tabulce */
};

static uchar *dbuf;
static uint64_t dlen, dcap;
static WRef *drefs;
static uint ndrefs;
static WSym *wsyms;
static uint nwsyms;
static int arities[64];

static WSym *
wsym(uint32_t id)
{
	uint n;

	for (n=0; n<nwsyms; n++)
		if (wsyms[n].id == id)
			return &wsyms[n];
	if (!wsyms)
		wsyms = vnew(0, sizeof wsyms[0], PHeap);
	vgrow(&wsyms, ++nwsyms);
	wsyms[nwsyms-1].id = id;
	wsyms[nwsyms-1].isdata = 0;
	wsyms[nwsyms-1].isfunc = 0;
	wsyms[nwsyms-1].used = 0;
	wsyms[nwsyms-1].addr = 0;
	return &wsyms[nwsyms-1];
}

static void
dput(int c)
{
	if (dlen == dcap) {
		dcap = dcap ? 2 * dcap : 4096;
		dbuf = realloc(dbuf, dcap);
		if (!dbuf)
			die("out of memory");
	}
	dbuf[dlen++] = c;
}

static void
dputn(int64_t v, int size)
{
	int n;

	for (n=0; n<size; n++)
		dput((v >> (8 * n)) & 0xff);
}

/* Řetězec z lexeru: v uvozovkách, escape \n \r \t \" \\ a \NNN. */
static void
dputstr(char *s)
{
	int c;

	for (s++; *s && *s != '"'; s++) {
		if (*s != '\\') {
			dput((uchar)*s);
			continue;
		}
		s++;
		switch (*s) {
		case 'n': dput('\n'); break;
		case 'r': dput('\r'); break;
		case 't': dput('\t'); break;
		case '"': dput('"'); break;
		case '\\': dput('\\'); break;
		default:
			c = (s[0] - '0') * 64 + (s[1] - '0') * 8 + (s[2] - '0');
			dput(c);
			s += 2;
			break;
		}
	}
}

void
wasm_emitdat(Dat *d)
{
	static int size[] = {[DB] = 1, [DH] = 2, [DW] = 4, [DL] = 8};
	int64_t n, align;
	WSym *s;

	switch (d->type) {
	case DStart:
		align = d->lnk->align ? d->lnk->align : 8;
		while ((DataBase + dlen) % align)
			dput(0);
		s = wsym(intern(d->name));
		s->isdata = 1;
		s->addr = DataBase + dlen;
		break;
	case DEnd:
		break;
	case DZ:
		for (n=0; n<d->u.num; n++)
			dput(0);
		break;
	default:
		if (d->isstr) {
			if (d->type != DB)
				err("strings only supported for 'b' currently");
			dputstr(d->u.str);
		} else if (d->isref) {
			if (!drefs)
				drefs = vnew(0, sizeof drefs[0], PHeap);
			vgrow(&drefs, ++ndrefs);
			drefs[ndrefs-1].at = dlen;
			drefs[ndrefs-1].sym = intern(d->u.ref.name);
			drefs[ndrefs-1].add = d->u.ref.off;
			drefs[ndrefs-1].size = size[d->type];
			wsym(drefs[ndrefs-1].sym)->used = 1;
			dputn(0, size[d->type]);
		} else
			dputn(d->u.num, size[d->type]);
		break;
	}
}

/* Stav emitace funkce. */
static Fn *fn;
static FILE *out;
static int *slotof;    /* posun hodnoty Kl v rámci, nebo -1 */
static int *allocof;   /* posun oblasti alloc v rámci */
static int framesize;
static int nextblk;    /* index bloku, který následuje v pořadí */
static int curblk;     /* index právě vypisovaného bloku */

static char *ktype[] = {[Kw] = "i32", [Kl] = "i64", [Ks] = "f32", [Kd] = "f64"};

static char *
symname(uint32_t id)
{
	char *s;

	s = str(id);
	return s;
}

/* Převod hodnoty na zásobníku z třídy from na třídu to (stejné šířky
 * nebo w/l; bitová reinterpretace jen přes cast). */
static void
convert(int from, int to)
{
	if (from == to)
		return;
	if (from == Kw && to == Kl)
		fputs("\ti64.extend_i32_u\n", out);
	else if (from == Kl && to == Kw)
		fputs("\ti32.wrap_i64\n", out);
	else
		die("wasm: cannot convert class %d to %d", from, to);
}

/* Hodnota ref třídy k na zásobník. */
static void
emitref(Ref r, int k)
{
	Con *c;
	Tmp *t;

	switch (rtype(r)) {
	case RTmp:
		t = &fn->tmp[r.val];
		fprintf(out, "\tlocal.get $t%d\n", r.val);
		convert(t->cls, k);
		break;
	case RCon:
		c = &fn->con[r.val];
		if (c->type == CAddr) {
			wsym(c->sym.id)->used = 1;
			fprintf(out, "\tglobal.get $a.%s\n", symname(c->sym.id));
			if (c->bits.i)
				fprintf(out, "\ti64.const %"PRId64"\n\ti64.add\n", c->bits.i);
			convert(Kl, k);
		} else if (k == Ks)
			fprintf(out, "\ti32.const %d\n\tf32.reinterpret_i32\n", (int32_t)c->bits.i);
		else if (k == Kd)
			fprintf(out, "\ti64.const %"PRId64"\n\tf64.reinterpret_i64\n", c->bits.i);
		else if (k == Kw)
			fprintf(out, "\ti32.const %d\n", (int32_t)c->bits.i);
		else
			fprintf(out, "\ti64.const %"PRId64"\n", c->bits.i);
		break;
	default:
		die("wasm: unexpected operand");
	}
}

/* Adresa (Kl) jako i32 pro přístup do paměti. */
static void
emitaddr(Ref r)
{
	emitref(r, Kl);
	fputs("\ti32.wrap_i64\n", out);
}

/* Výsledek ze zásobníku do dočasné hodnoty to; Kl také do rámce. */
static void
define(Ref to)
{
	fprintf(out, "\tlocal.set $t%d\n", to.val);
	if (slotof[to.val] >= 0)
		fprintf(out, "\tlocal.get $fp\n\tlocal.get $t%d\n"
			"\ti64.store offset=%d\n", to.val, slotof[to.val]);
}

static char *
prefix(int k)
{
	return ktype[k];
}

static void
binop(Ins *i, char *name)
{
	emitref(i->arg[0], i->cls);
	emitref(i->arg[1], i->cls);
	fprintf(out, "\t%s.%s\n", prefix(i->cls), name);
	define(i->to);
}

/* Porovnání: třída operandů z operace, výsledek i32 (Kl rozšířený). */
static void
compare(Ins *i)
{
	static char *iname[] = {"eq", "ne", "ge_s", "gt_s", "le_s", "lt_s",
		"ge_u", "gt_u", "le_u", "lt_u"};
	static char *fname[] = {"eq", "ge", "gt", "le", "lt", "ne"};
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
	if (KBASE(k) == 0) {
		emitref(i->arg[0], k);
		emitref(i->arg[1], k);
		fprintf(out, "\t%s.%s\n", prefix(k), iname[c]);
	} else if (c < 6) {
		emitref(i->arg[0], k);
		emitref(i->arg[1], k);
		fprintf(out, "\t%s.%s\n", prefix(k), fname[c]);
	} else {
		/* co: oba operandy nejsou NaN; cuo: aspoň jeden je */
		emitref(i->arg[0], k);
		emitref(i->arg[0], k);
		fprintf(out, "\t%s.eq\n", prefix(k));
		emitref(i->arg[1], k);
		emitref(i->arg[1], k);
		fprintf(out, "\t%s.eq\n\ti32.and\n", prefix(k));
		if (c == 7)
			fputs("\ti32.eqz\n", out);
	}
	convert(Kw, i->cls);
	define(i->to);
}

static void
load(Ins *i)
{
	static char *name[] = {"load8_s", "load8_u", "load16_s", "load16_u",
		"load32_s", "load32_u"};
	int n;

	emitaddr(i->arg[0]);
	if (i->op == Oload) {
		fprintf(out, "\t%s.load\n", prefix(i->cls));
	} else {
		n = i->op - Oloadsb;
		if (i->cls == Kw && n >= 4)
			fputs("\ti32.load\n", out);
		else
			fprintf(out, "\t%s.%s\n", prefix(i->cls), name[n]);
	}
	define(i->to);
}

static void
store(Ins *i)
{
	static char *name[] = {"i32.store8", "i32.store16", "i32.store",
		"i64.store", "f32.store", "f64.store"};
	static int cls[] = {Kw, Kw, Kw, Kl, Ks, Kd};
	int n;

	n = i->op - Ostoreb;
	emitaddr(i->arg[1]);
	emitref(i->arg[0], cls[n]);
	fprintf(out, "\t%s\n", name[n]);
}

static void
extend(Ins *i)
{
	switch (i->op) {
	case Oextsb:
		emitref(i->arg[0], Kw);
		fputs("\ti32.extend8_s\n", out);
		if (i->cls == Kl)
			fputs("\ti64.extend_i32_s\n", out);
		break;
	case Oextub:
		emitref(i->arg[0], Kw);
		fputs("\ti32.const 255\n\ti32.and\n", out);
		convert(Kw, i->cls);
		break;
	case Oextsh:
		emitref(i->arg[0], Kw);
		fputs("\ti32.extend16_s\n", out);
		if (i->cls == Kl)
			fputs("\ti64.extend_i32_s\n", out);
		break;
	case Oextuh:
		emitref(i->arg[0], Kw);
		fputs("\ti32.const 65535\n\ti32.and\n", out);
		convert(Kw, i->cls);
		break;
	case Oextsw:
		emitref(i->arg[0], Kw);
		if (i->cls == Kl)
			fputs("\ti64.extend_i32_s\n", out);
		break;
	case Oextuw:
		emitref(i->arg[0], Kw);
		convert(Kw, i->cls);
		break;
	}
	define(i->to);
}

/* Hodnota z ABI (i64) do třídy k a obráceně. */
static void
fromabi(int k)
{
	if (k == Kw)
		fputs("\ti32.wrap_i64\n", out);
	else if (k == Ks)
		fputs("\ti32.wrap_i64\n\tf32.reinterpret_i32\n", out);
	else if (k == Kd)
		fputs("\tf64.reinterpret_i64\n", out);
}

static void
toabi(int k)
{
	if (k == Kw)
		fputs("\ti64.extend_i32_u\n", out);
	else if (k == Ks)
		fputs("\ti32.reinterpret_f32\n\ti64.extend_i32_u\n", out);
	else if (k == Kd)
		fputs("\ti64.reinterpret_f64\n", out);
}

static void
call(Ins *i, Ins *args, int nargs)
{
	Con *c;
	int n;

	for (n=0; n<nargs; n++) {
		if (args[n].op == Oargv)
			continue;
		if (!INRANGE(args[n].op, Oarg, Oarguh))
			die("wasm: aggregate arguments are not supported");
		emitref(args[n].arg[0], args[n].cls);
		toabi(args[n].cls);
	}
	n = 0;
	for (; nargs > 0; nargs--)
		if (args[nargs-1].op != Oargv)
			n++;
	if (rtype(i->arg[0]) == RCon && fn->con[i->arg[0].val].type == CAddr) {
		c = &fn->con[i->arg[0].val];
		if (c->bits.i)
			die("wasm: call to an offset symbol");
		fprintf(out, "\tcall $%s\n", symname(c->sym.id));
	} else {
		if (n >= 64)
			die("wasm: too many arguments");
		arities[n] = 1;
		emitaddr(i->arg[0]);
		fprintf(out, "\tcall_indirect $T (type $f%d)\n", n);
	}
	if (req(i->to, R))
		fputs("\tdrop\n", out);
	else {
		fromabi(i->cls);
		define(i->to);
	}
}

static void
emitins(Ins *i)
{
	switch (i->op) {
	case Onop:
	case Odbgloc:
		break;
	case Ocopy:
		emitref(i->arg[0], i->cls);
		define(i->to);
		break;
	case Oadd: binop(i, "add"); break;
	case Osub: binop(i, "sub"); break;
	case Omul: binop(i, "mul"); break;
	case Oand: binop(i, "and"); break;
	case Oor: binop(i, "or"); break;
	case Oxor: binop(i, "xor"); break;
	case Odiv: binop(i, KBASE(i->cls) ? "div" : "div_s"); break;
	case Orem: binop(i, "rem_s"); break;
	case Oudiv: binop(i, "div_u"); break;
	case Ourem: binop(i, "rem_u"); break;
	case Osar:
	case Oshr:
	case Oshl:
		emitref(i->arg[0], i->cls);
		emitref(i->arg[1], i->cls);
		fprintf(out, "\t%s.%s\n", prefix(i->cls),
			i->op == Osar ? "shr_s" : i->op == Oshr ? "shr_u" : "shl");
		define(i->to);
		break;
	case Oneg:
		if (KBASE(i->cls) == 0) {
			fprintf(out, "\t%s.const 0\n", prefix(i->cls));
			emitref(i->arg[0], i->cls);
			fprintf(out, "\t%s.sub\n", prefix(i->cls));
		} else {
			emitref(i->arg[0], i->cls);
			fprintf(out, "\t%s.neg\n", prefix(i->cls));
		}
		define(i->to);
		break;
	case Oexts:
		emitref(i->arg[0], Ks);
		fputs("\tf64.promote_f32\n", out);
		define(i->to);
		break;
	case Otruncd:
		emitref(i->arg[0], Kd);
		fputs("\tf32.demote_f64\n", out);
		define(i->to);
		break;
	case Ostosi:
	case Ostoui:
	case Odtosi:
	case Odtoui:
		/* nasycený převod: wasm by jinak u NaN a přetečení skončil */
		emitref(i->arg[0], (i->op == Ostosi || i->op == Ostoui) ? Ks : Kd);
		fprintf(out, "\t%s.trunc_sat_%s_%c\n", prefix(i->cls),
			(i->op == Ostosi || i->op == Ostoui) ? "f32" : "f64",
			(i->op == Ostosi || i->op == Odtosi) ? 's' : 'u');
		define(i->to);
		break;
	case Oswtof:
	case Ouwtof:
		emitref(i->arg[0], Kw);
		fprintf(out, "\t%s.convert_i32_%c\n", prefix(i->cls),
			i->op == Oswtof ? 's' : 'u');
		define(i->to);
		break;
	case Osltof:
	case Oultof:
		emitref(i->arg[0], Kl);
		fprintf(out, "\t%s.convert_i64_%c\n", prefix(i->cls),
			i->op == Osltof ? 's' : 'u');
		define(i->to);
		break;
	case Ocast:
		switch (i->cls) {
		case Kw:
			emitref(i->arg[0], Ks);
			fputs("\ti32.reinterpret_f32\n", out);
			break;
		case Kl:
			emitref(i->arg[0], Kd);
			fputs("\ti64.reinterpret_f64\n", out);
			break;
		case Ks:
			emitref(i->arg[0], Kw);
			fputs("\tf32.reinterpret_i32\n", out);
			break;
		case Kd:
			emitref(i->arg[0], Kl);
			fputs("\tf64.reinterpret_i64\n", out);
			break;
		}
		define(i->to);
		break;
	case Oalloc4:
	case Oalloc8:
	case Oalloc16:
		fprintf(out, "\tlocal.get $fp\n\ti64.extend_i32_u\n"
			"\ti64.const %d\n\ti64.add\n", allocof[i->to.val]);
		define(i->to);
		break;
	case Oblit1:
		break;
	case Oblit0:
		/* blit0 (zdroj, cíl) a za ním blit1 s velikostí */
		emitaddr(i->arg[1]);
		emitaddr(i->arg[0]);
		fprintf(out, "\ti32.const %d\n\tmemory.copy\n", abs(rsval((i+1)->arg[0])));
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
			die("wasm: unsupported operation %s", optab[i->op].name);
		break;
	}
}

/* Kopie phi pro hranu b -> s: všechny hodnoty na zásobník, pak přiřazení
 * v obráceném pořadí (paralelní kopie). */
static void
phicopies(Blk *b, Blk *s)
{
	Phi *p;
	uint n;
	int cnt;
	Phi *list[256];

	cnt = 0;
	for (p=s->phi; p; p=p->link) {
		for (n=0; n<p->narg; n++)
			if (p->blk[n] == b)
				break;
		if (n == p->narg)
			die("wasm: missing phi argument");
		emitref(p->arg[n], p->cls);
		if (cnt == 256)
			die("wasm: too many phis");
		list[cnt++] = p;
	}
	while (cnt > 0) {
		cnt--;
		define(list[cnt]->to);
	}
}

static void
safepoint()
{
	if (!wasmthreads)
		return;
	fputs("\tglobal.get $__safepoint_flag\n\ti32.wrap_i64\n\ti32.load\n"
		"\tif\n\tcall $__safepoint\n\tend\n", out);
}

static void
jump(Blk *b, Blk *s)
{
	phicopies(b, s);
	if ((int)s->id == nextblk)
		return;
	if ((int)s->id <= curblk)
		safepoint();
	fprintf(out, "\ti32.const %u\n\tlocal.set $bb\n\tbr $L\n", s->id);
}

static void
epilogue()
{
	fprintf(out, "\tlocal.get $fp\n\ti32.const %d\n\ti32.add\n"
		"\tglobal.set $sp\n", framesize);
}

void
wasm_emitfn(Fn *f, FILE *o)
{
	Blk *b;
	Ins *i, *args;
	uint n;
	int t, npar, off, sz;

	fn = f;
	out = o;
	/* bloky podle rpo */
	for (n=0; n<fn->nblk; n++)
		fn->rpo[n]->id = n;
	wsym(intern(fn->name))->isfunc = 1;
	slotof = alloc(fn->ntmp * sizeof slotof[0]);
	allocof = alloc(fn->ntmp * sizeof allocof[0]);
	off = 0;
	for (t=0; t<fn->ntmp; t++) {
		slotof[t] = -1;
		allocof[t] = 0;
		if (t >= Tmp0 && fn->tmp[t].cls == Kl) {
			slotof[t] = off;
			off += 8;
		}
	}
	npar = 0;
	for (n=0; n<fn->nblk; n++)
		for (i=fn->rpo[n]->ins; i<&fn->rpo[n]->ins[fn->rpo[n]->nins]; i++) {
			if (ispar(i->op)) {
				if (!INRANGE(i->op, Opar, Oparuh))
					die("wasm: aggregate parameters are not supported");
				npar++;
			}
			if (isalloc(i->op)) {
				if (rtype(i->arg[0]) != RCon)
					die("wasm: dynamic alloc is not supported");
				off = (off + 15) & ~15;
				allocof[i->to.val] = off;
				sz = fn->con[i->arg[0].val].bits.i;
				off += (sz + 7) & ~7;
			}
		}
	framesize = (off + 15) & ~15;
	if (fn->vararg)
		die("wasm: variadic functions are not supported");
	fprintf(out, "(func $%s", fn->name);
	for (n=0; n<(uint)npar; n++)
		fputs(" (param i64)", out);
	fputs(" (result i64)\n", out);
	fputs("\t(local $fp i32) (local $bb i32)\n", out);
	for (t=Tmp0; t<fn->ntmp; t++)
		fprintf(out, "\t(local $t%d %s)\n", t, ktype[fn->tmp[t].cls < 0 ? Kl : fn->tmp[t].cls]);
	/* rámec na stínovém zásobníku; starý obsah GC jen podrží objekty
	 * navíc jako u nativního zásobníku */
	fprintf(out, "\tglobal.get $sp\n\ti32.const %d\n\ti32.sub\n"
		"\tlocal.tee $fp\n\tglobal.set $sp\n", framesize);
	if (framesize)
		fputs("\tlocal.get $fp\n\tglobal.get $__stack_limit\n\ti32.lt_u\n"
			"\tif\n\tcall $__stack_overflow\n\tunreachable\n\tend\n", out);
	safepoint();
	npar = 0;
	for (i=fn->start->ins; i<&fn->start->ins[fn->start->nins]; i++)
		if (ispar(i->op)) {
			fprintf(out, "\tlocal.get %d\n", npar++);
			if (i->op == Oparsb || i->op == Oparub || i->op == Oparsh || i->op == Oparuh)
				fromabi(Kw);
			else
				fromabi(i->cls);
			define(i->to);
		}
	fputs("\tloop $L\n", out);
	for (n=fn->nblk; n>0; n--)
		fprintf(out, "\tblock $B%u\n", n-1);
	fputs("\tlocal.get $bb\n\tbr_table", out);
	for (n=0; n<fn->nblk; n++)
		fprintf(out, " $B%u", n);
	fputs("\n", out);
	for (n=0; n<fn->nblk; n++) {
		b = fn->rpo[n];
		fprintf(out, "\tend\n");
		nextblk = n + 1;
		curblk = n;
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
			fputs("\tunreachable\n", out);
			break;
		case Jret0:
			epilogue();
			fputs("\ti64.const 0\n\treturn\n", out);
			break;
		case Jretw:
		case Jretl:
		case Jrets:
		case Jretd:
		case Jretsb:
		case Jretub:
		case Jretsh:
		case Jretuh:
			t = b->jmp.type == Jretl ? Kl : b->jmp.type == Jrets ? Ks
				: b->jmp.type == Jretd ? Kd : Kw;
			emitref(b->jmp.arg, t);
			toabi(t);
			epilogue();
			fputs("\treturn\n", out);
			break;
		case Jjmp:
			jump(b, b->s1);
			break;
		case Jjnz:
			emitref(b->jmp.arg, fn->tmp[b->jmp.arg.val].cls == Kl && rtype(b->jmp.arg) == RTmp ? Kl : Kw);
			if (rtype(b->jmp.arg) == RTmp && fn->tmp[b->jmp.arg.val].cls == Kl)
				fputs("\ti64.const 0\n\ti64.ne\n", out);
			fputs("\tif\n", out);
			nextblk = -1;
			jump(b, b->s1);
			fputs("\telse\n", out);
			nextblk = n + 1;
			jump(b, b->s2);
			fputs("\tend\n", out);
			break;
		default:
			die("wasm: unsupported jump %d", b->jmp.type);
		}
	}
	fputs("\tend\n\tunreachable\n)\n", out);
}

static void
quoteput(FILE *f, uchar c)
{
	if (c >= 32 && c < 127 && c != '"' && c != '\\')
		fputc(c, f);
	else
		fprintf(f, "\\%02x", c);
}

void
wasm_emitfin(FILE *f)
{
	uint n;
	int64_t v, idx, stack, pages;
	WSym *s;
	WRef *r;
	int k;

	/* indexy funkcí v tabulce (0 = žádná funkce) */
	idx = 1;
	for (n=0; n<nwsyms; n++)
		if (wsyms[n].isfunc && wsyms[n].used)
			wsyms[n].addr = idx++;
	for (n=0; n<ndrefs; n++) {
		r = &drefs[n];
		s = wsym(r->sym);
		if (!s->isdata && !s->isfunc)
			die("wasm: data refers to undefined symbol %s", str(r->sym));
		v = s->addr + r->add;
		for (k=0; k<r->size; k++)
			dbuf[r->at + k] = (v >> (8 * k)) & 0xff;
	}
	if (wasmthreads) {
		fputs("(data $__data \"", f);
		for (n=0; n<dlen; n++)
			quoteput(f, dbuf[n]);
		fputs("\")\n", f);
		fprintf(f, "(func $__wasm_init_memory\n\ti32.const %d\n\ti32.const 0\n"
			"\ti32.const %"PRId64"\n\tmemory.init $__data\n\tdata.drop $__data\n)\n",
			DataBase, (int64_t)dlen);
	} else if (dlen) {
		fprintf(f, "(data (i32.const %d) \"", DataBase);
		for (n=0; n<dlen; n++)
			quoteput(f, dbuf[n]);
		fputs("\")\n", f);
	}
	for (n=0; n<nwsyms; n++) {
		s = &wsyms[n];
		if (s->used && (s->isdata || s->isfunc))
			fprintf(f, "(global $a.%s i64 (i64.const %"PRId64"))\n", str(s->id), s->addr);
	}
	fprintf(f, "(table $T %"PRId64" funcref)\n", idx);
	if (idx > 1) {
		fputs("(elem (i32.const 1) func", f);
		for (n=0; n<nwsyms; n++)
			if (wsyms[n].isfunc && wsyms[n].used)
				fprintf(f, " $%s", str(wsyms[n].id));
		fputs(")\n", f);
	}
	for (k=0; k<64; k++)
		if (arities[k]) {
			fprintf(f, "(type $f%d (func", k);
			for (n=0; n<(uint)k; n++)
				fputs(" (param i64)", f);
			fputs(" (result i64)))\n", f);
		}
	stack = (DataBase + dlen + 15 + StackBytes) & ~15;
	pages = (stack + 65535) / 65536 + 1;
	fprintf(f, "(global $__stack_top i32 (i32.const %"PRId64"))\n", stack);
	fprintf(f, "(global $sp (mut i32) (i32.const %"PRId64"))\n", stack);
	fprintf(f, "(global $__stack_limit (mut i32) (i32.const %"PRId64"))\n", stack - StackBytes);
	fprintf(f, "(global $__heap_base i64 (i64.const %"PRId64"))\n", pages * 65536);
	if (wasmthreads)
		fprintf(f, "(global $__memory_pages i32 (i32.const %"PRId64"))\n", pages);
	else
		fprintf(f, "(memory (export \"memory\") %"PRId64")\n", pages);
}

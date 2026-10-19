#include "as.h"

/* Kodér RISC-V (RV64IMAFD, Zicsr, Zifencei) pro vestavěný assembler:
 * instrukce, které vypisuje emitor rv64 LuxIR a vrstvy architektury
 * runtime LSL, kódování a rozpis pseudoinstrukcí (li, la/lla, call,
 * loady a story ze symbolu, větve s aliasem) jako GNU as s -march=rv64g
 * -mno-relax: bez komprese a bez relokací R_RISCV_RELAX.
 *
 * Jako GNU as: relativní odkazy na místní návěští .L téže sekce se
 * vyřeší, na ostatní symboly zůstává relokace (u větví a skoků
 * s předběžnou hodnotou v poli); %pcrel_lo míří na návěští „.L0 “
 * u příslušného auipc. Podmíněná větev má krátkou formu (4 bajty,
 * dosah ±4 KiB) a dlouhou (obrácená větev přes jal). */

enum {
	ENone,
	EBranch,  /* B-typ: imm13 */
	EJal,     /* J-typ: imm21 */
	ECall,    /* auipc + jalr: R_RISCV_CALL_PLT */
	EPcHi,    /* auipc: R_RISCV_PCREL_HI20 */
	EPcLoI,   /* I-typ: R_RISCV_PCREL_LO12_I */
	EPcLoS,   /* S-typ: R_RISCV_PCREL_LO12_S */
	EHi,      /* lui %hi: R_RISCV_HI20 */
	ELoI,     /* %lo v I-typu: R_RISCV_LO12_I */
	ELoS,     /* %lo v S-typu: R_RISCV_LO12_S */
};

typedef struct AOp AOp;
struct AOp {
	int kind;      /* 0 nic, 1 registr x, 2 registr f, 3 výraz, 4 paměť imm(reg), 5 slovo (csr, zaokrouhlení, fence) */
	int reg;
	AExpr e;       /* výraz nebo posun paměti */
	int mod;       /* 0 nic, 1 %hi, 2 %lo, 3 %pcrel_hi */
	char word[32];
};

static char *curmnem;

static void
rerr(char *msg)
{
	aerr("%s: %s", curmnem, msg);
}

/* ---- registry a slova ---- */

static char *xabi[32] = {
	"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
	"s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
	"a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
	"s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

static char *fabi[32] = {
	"ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7",
	"fs0", "fs1", "fa0", "fa1", "fa2", "fa3", "fa4", "fa5",
	"fa6", "fa7", "fs2", "fs3", "fs4", "fs5", "fs6", "fs7",
	"fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11",
};

/* registr podle jména: kind 1/2 a číslo, 0 není registr */
static int
regname(char *w, int *kind, int *reg)
{
	int n;
	char *end;

	if (strcmp(w, "fp") == 0) {
		*kind = 1;
		*reg = 8;
		return 1;
	}
	if ((w[0] == 'x' || w[0] == 'f') && isdigit((uchar)w[1])) {
		n = strtol(w + 1, &end, 10);
		if (*end == 0 && n >= 0 && n < 32) {
			*kind = w[0] == 'x' ? 1 : 2;
			*reg = n;
			return 1;
		}
	}
	for (n=0; n<32; n++) {
		if (strcmp(w, xabi[n]) == 0) {
			*kind = 1;
			*reg = n;
			return 1;
		}
		if (strcmp(w, fabi[n]) == 0) {
			*kind = 2;
			*reg = n;
			return 1;
		}
	}
	return 0;
}

static struct {
	char *name;
	int num;
} csrs[] = {
	{"fflags", 0x001}, {"frm", 0x002}, {"fcsr", 0x003},
	{"cycle", 0xc00}, {"time", 0xc01}, {"instret", 0xc02},
	{"sstatus", 0x100}, {"sie", 0x104}, {"stvec", 0x105}, {"scounteren", 0x106},
	{"senvcfg", 0x10a}, {"sscratch", 0x140}, {"sepc", 0x141}, {"scause", 0x142},
	{"stval", 0x143}, {"sip", 0x144}, {"stimecmp", 0x14d}, {"satp", 0x180},
	{"mvendorid", 0xf11}, {"marchid", 0xf12}, {"mimpid", 0xf13}, {"mhartid", 0xf14},
	{"mstatus", 0x300}, {"misa", 0x301}, {"medeleg", 0x302}, {"mideleg", 0x303},
	{"mie", 0x304}, {"mtvec", 0x305}, {"mcounteren", 0x306}, {"menvcfg", 0x30a},
	{"mcountinhibit", 0x320}, {"mscratch", 0x340}, {"mepc", 0x341}, {"mcause", 0x342},
	{"mtval", 0x343}, {"mip", 0x344},
	{"pmpcfg0", 0x3a0}, {"pmpcfg2", 0x3a2},
	{"pmpaddr0", 0x3b0}, {"pmpaddr1", 0x3b1}, {"pmpaddr2", 0x3b2}, {"pmpaddr3", 0x3b3},
	{"pmpaddr4", 0x3b4}, {"pmpaddr5", 0x3b5}, {"pmpaddr6", 0x3b6}, {"pmpaddr7", 0x3b7},
	{"mcycle", 0xb00}, {"minstret", 0xb02},
	{0, 0},
};

static char *rmnames[] = {"rne", "rtz", "rdn", "rup", "rmm", 0, 0, "dyn"};

/* slovo operandu: csr, zaokrouhlení nebo množina fence */
static int
isword(char *w)
{
	int n;
	char *p;

	for (n=0; csrs[n].name; n++)
		if (strcmp(w, csrs[n].name) == 0)
			return 1;
	for (n=0; n<8; n++)
		if (rmnames[n] && strcmp(w, rmnames[n]) == 0)
			return 1;
	if (*w == 0)
		return 0;
	for (p=w; *p; p++)
		if (!strchr("iorw", *p))
			return 0;
	return 1;
}

/* ---- operandy ---- */

static void
parseop(char *s, AOp *o)
{
	char *p, *q, *end, w[32];
	int k, r, n;

	memset(o, 0, sizeof *o);
	o->e.sym = -1;
	p = s;
	aspace(&p);
	/* modifikátor %hi(sym), %lo(sym), %pcrel_hi(sym) [(reg)] */
	if (*p == '%') {
		p++;
		q = p;
		while (identchar((uchar)*q))
			q++;
		n = q - p;
		if (n == 2 && strncmp(p, "hi", 2) == 0)
			o->mod = 1;
		else if (n == 2 && strncmp(p, "lo", 2) == 0)
			o->mod = 2;
		else if (n == 8 && strncmp(p, "pcrel_hi", 8) == 0)
			o->mod = 3;
		else
			rerr("unsupported relocation modifier");
		p = q;
		aspace(&p);
		if (*p != '(')
			rerr("'(' expected");
		p++;
		if (!aexpr(&p, &o->e) || o->e.sym < 0)
			rerr("symbol expected");
		aspace(&p);
		if (*p != ')')
			rerr("')' expected");
		p++;
		aspace(&p);
		o->kind = 3;
		if (*p == '(') {
			q = strchr(p, ')');
			if (!q)
				rerr("')' expected");
			n = q - p - 1;
			if (n <= 0 || n >= (int)sizeof w)
				rerr("register expected");
			memcpy(w, p + 1, n);
			w[n] = 0;
			if (!regname(w, &k, &r) || k != 1)
				rerr("register expected");
			o->kind = 4;
			o->reg = r;
			p = q + 1;
			aspace(&p);
		}
		if (*p)
			rerr("junk after operand");
		return;
	}
	/* paměť: [výraz](reg) */
	q = strchr(p, '(');
	if (q && strchr(q, ')') && !(q > p && q[-1] == ' ' && 0)) {
		/* výraz před závorkou nesmí být sám uzávorkovaný výraz: (1+2)
		 * není paměť; paměť poznáme podle registru v závorce */
		end = strchr(q, ')');
		n = end - q - 1;
		if (n > 0 && n < (int)sizeof w) {
			memcpy(w, q + 1, n);
			w[n] = 0;
			if (regname(w, &k, &r) && k == 1) {
				end++;
				aspace(&end);
				if (*end)
					rerr("junk after memory operand");
				o->kind = 4;
				o->reg = r;
				if (q > p) {
					*q = 0;
					if (!aexpr(&p, &o->e))
						rerr("offset expected");
					aspace(&p);
					if (*p)
						rerr("invalid memory offset");
				}
				return;
			}
		}
	}
	/* registr nebo slovo */
	q = p;
	while (identchar((uchar)*q) || *q == '.')
		q++;
	n = q - p;
	end = q;
	aspace(&end);
	if (n > 0 && n < (int)sizeof w && *end == 0) {
		memcpy(w, p, n);
		w[n] = 0;
		if (regname(w, &k, &r)) {
			o->kind = k;
			o->reg = r;
			return;
		}
		if (isword(w)) {
			o->kind = 5;
			strcpy(o->word, w);
			return;
		}
	}
	if (!aexpr(&p, &o->e))
		rerr("invalid operand");
	aspace(&p);
	if (*p)
		rerr("junk after operand");
	o->kind = 3;
}

/* rozdělí operandy čárkami mimo závorky */
static int
parseops(char *s, AOp *o, int max)
{
	int n, depth;
	char *p, *start;

	n = 0;
	aspace(&s);
	if (!*s)
		return 0;
	start = s;
	depth = 0;
	for (p=s; ; p++) {
		if (*p == '(')
			depth++;
		else if (*p == ')')
			depth--;
		else if ((*p == ',' && depth == 0) || *p == 0) {
			if (n >= max)
				rerr("too many operands");
			if (*p) {
				*p = 0;
				parseop(start, &o[n++]);
				start = p + 1;
			} else {
				parseop(start, &o[n++]);
				break;
			}
		}
	}
	return n;
}

/* ---- kódování ---- */

static void
put32(uint32_t w)
{
	uchar *b;

	b = aemit(4);
	if (!b)
		rerr("instruction in a nobits section");
	b[0] = w;
	b[1] = w >> 8;
	b[2] = w >> 16;
	b[3] = w >> 24;
}

static void
put32fix(uint32_t w, int kind, int64_t pcoff, int enc, AExpr *e, int relsym)
{
	AItem *it;
	uint off;

	put32(w);
	it = &asecs[acursec].item[asecs[acursec].nitem - 1];
	off = it->size - 4;
	afixrel(off, 4, kind, pcoff, enc, e, relsym);
}

static uint32_t
rfmt(int f7, int rs2, int rs1, int f3, int rd, int op)
{
	return ((uint32_t)f7 << 25) | ((uint32_t)rs2 << 20) | ((uint32_t)rs1 << 15) | ((uint32_t)f3 << 12) | ((uint32_t)rd << 7) | op;
}

static uint32_t
ifmt(int64_t imm, int rs1, int f3, int rd, int op)
{
	return ((uint32_t)(imm & 0xfff) << 20) | ((uint32_t)rs1 << 15) | ((uint32_t)f3 << 12) | ((uint32_t)rd << 7) | op;
}

static uint32_t
sfmt(int64_t imm, int rs2, int rs1, int f3, int op)
{
	return ((uint32_t)((imm >> 5) & 0x7f) << 25) | ((uint32_t)rs2 << 20) | ((uint32_t)rs1 << 15) | ((uint32_t)f3 << 12) | ((uint32_t)(imm & 0x1f) << 7) | op;
}

static uint32_t
bimm(int64_t v)
{
	return ((uint32_t)((v >> 12) & 1) << 31) | ((uint32_t)((v >> 5) & 0x3f) << 25)
		| ((uint32_t)((v >> 1) & 0xf) << 8) | ((uint32_t)((v >> 11) & 1) << 7);
}

static uint32_t
jimm(int64_t v)
{
	return ((uint32_t)((v >> 20) & 1) << 31) | ((uint32_t)((v >> 1) & 0x3ff) << 21)
		| ((uint32_t)((v >> 11) & 1) << 20) | ((uint32_t)((v >> 12) & 0xff) << 12);
}

static uint32_t
ufmt(int64_t imm20, int rd, int op)
{
	return ((uint32_t)(imm20 & 0xfffff) << 12) | ((uint32_t)rd << 7) | op;
}

static int
fits12(int64_t v)
{
	return v >= -2048 && v <= 2047;
}

static int
xreg(AOp *o)
{
	if (o->kind != 1)
		rerr("integer register expected");
	return o->reg;
}

static int
freg(AOp *o)
{
	if (o->kind != 2)
		rerr("floating-point register expected");
	return o->reg;
}

static int64_t
constant(AOp *o)
{
	if (o->kind != 3 || o->e.sym >= 0)
		rerr("constant expected");
	return o->e.add;
}

static int64_t
imm12(AOp *o)
{
	int64_t v;

	v = constant(o);
	if (!fits12(v))
		rerr("immediate out of range");
	return v;
}

static int
rmode(AOp *o, int no, int idx)
{
	int n;

	if (no <= idx)
		return 7;
	if (o[idx].kind != 5)
		rerr("rounding mode expected");
	for (n=0; n<8; n++)
		if (rmnames[n] && strcmp(o[idx].word, rmnames[n]) == 0)
			return n;
	rerr("unknown rounding mode");
	return 7;
}

static int
csrnum(AOp *o)
{
	int n;
	int64_t v;

	if (o->kind == 5) {
		for (n=0; csrs[n].name; n++)
			if (strcmp(o->word, csrs[n].name) == 0)
				return csrs[n].num;
		rerr("unknown csr");
	}
	v = constant(o);
	if (v < 0 || v > 0xfff)
		rerr("csr number out of range");
	return v;
}

static int
fencemask(char *w)
{
	int m;

	m = 0;
	for (; *w; w++)
		m |= *w == 'i' ? 8 : *w == 'o' ? 4 : *w == 'r' ? 2 : 1;
	return m;
}

/* li: konstanta jako GNU as (load_const): addi / lui+addiw, jinak
 * rekurzivně horní část, slli a addi */
static void
loadconst(int rd, int64_t v, int top)
{
	int64_t lo, hi;
	int shift;

	if (fits12(v)) {
		/* li s 12bitovou konstantou je alias addi; uvnitř rozkladu
		 * GNU as (load_const) vzniká addiw */
		put32(ifmt(v, 0, 0, rd, top ? 0x13 : 0x1b));
		return;
	}
	lo = ((v & 0xfff) ^ 0x800) - 0x800;
	hi = v - lo;
	if (v != (int64_t)(int32_t)v) {
		shift = 12;
		while (((hi >> shift) & 1) == 0)
			shift++;
		loadconst(rd, hi >> shift, 0);
		put32(ifmt(shift, rd, 1, rd, 0x13));
		if (lo != 0)
			put32(ifmt(lo, rd, 0, rd, 0x13));
		return;
	}
	if (hi != 0)
		put32(ufmt((uint32_t)hi >> 12, rd, 0x37));
	if (lo != 0 || hi == 0)
		put32(ifmt(lo, hi != 0 ? rd : 0, 0, rd, 0x1b));
}

/* auipc + instrukce s %pcrel_lo: návěští .L0 u auipc je symbolem
 * relokace dolní části */
static void
pcrelpair(int tmp, AExpr *e, uint32_t lo, int loenc)
{
	int lab;

	lab = apcrellabel();
	put32fix(ufmt(0, tmp, 0x17), FPcrel, 0, EPcHi, e, -1);
	put32fix(lo, FPcrel, 4, loenc, e, lab);
}

static void
branch(int f3, int rs1, int rs2, AOp *target)
{
	if (target->kind != 3 || target->e.sym < 0)
		rerr("branch target expected");
	ajump((int)(((uint32_t)rs2 << 20) | ((uint32_t)rs1 << 15) | ((uint32_t)f3 << 12) | 0x63), &target->e);
}

static void
jump(int rd, AOp *target)
{
	if (target->kind != 3 || target->e.sym < 0)
		rerr("jump target expected");
	put32fix(ufmt(0, rd, 0x6f), FBranch, 0, EJal, &target->e, -1);
}

static void
callinsn(int rd, AOp *target)
{
	if (target->kind != 3 || target->e.sym < 0)
		rerr("call target expected");
	put32fix(ufmt(0, rd, 0x17), FPcrel, 0, ECall, &target->e, -1);
	put32(ifmt(0, rd, 0, rd, 0x67));
}

/* ---- tabulky ---- */

static struct {
	char *name;
	int f7, f3, op;
	int immop;   /* operace s immediate (addi…), 0 není */
} rops[] = {
	{"add", 0, 0, 0x33, 0x13}, {"sub", 0x20, 0, 0x33, 0}, {"sll", 0, 1, 0x33, 0x13},
	{"slt", 0, 2, 0x33, 0x13}, {"sltu", 0, 3, 0x33, 0x13}, {"xor", 0, 4, 0x33, 0x13},
	{"srl", 0, 5, 0x33, 0x13}, {"sra", 0x20, 5, 0x33, 0x13}, {"or", 0, 6, 0x33, 0x13},
	{"and", 0, 7, 0x33, 0x13},
	{"addw", 0, 0, 0x3b, 0x1b}, {"subw", 0x20, 0, 0x3b, 0}, {"sllw", 0, 1, 0x3b, 0x1b},
	{"srlw", 0, 5, 0x3b, 0x1b}, {"sraw", 0x20, 5, 0x3b, 0x1b},
	{"mul", 1, 0, 0x33, 0}, {"mulh", 1, 1, 0x33, 0}, {"mulhsu", 1, 2, 0x33, 0},
	{"mulhu", 1, 3, 0x33, 0}, {"div", 1, 4, 0x33, 0}, {"divu", 1, 5, 0x33, 0},
	{"rem", 1, 6, 0x33, 0}, {"remu", 1, 7, 0x33, 0},
	{"mulw", 1, 0, 0x3b, 0}, {"divw", 1, 4, 0x3b, 0}, {"divuw", 1, 5, 0x3b, 0},
	{"remw", 1, 6, 0x3b, 0}, {"remuw", 1, 7, 0x3b, 0},
	{0, 0, 0, 0, 0},
};

static struct {
	char *name;
	int f3, op;
} iops[] = {
	{"addi", 0, 0x13}, {"slti", 2, 0x13}, {"sltiu", 3, 0x13}, {"xori", 4, 0x13},
	{"ori", 6, 0x13}, {"andi", 7, 0x13}, {"addiw", 0, 0x1b},
	{0, 0, 0},
};

static struct {
	char *name;
	int f3, op, fp;
} loads[] = {
	{"lb", 0, 0x03, 0}, {"lh", 1, 0x03, 0}, {"lw", 2, 0x03, 0}, {"ld", 3, 0x03, 0},
	{"lbu", 4, 0x03, 0}, {"lhu", 5, 0x03, 0}, {"lwu", 6, 0x03, 0},
	{"flw", 2, 0x07, 1}, {"fld", 3, 0x07, 1},
	{0, 0, 0, 0},
};

static struct {
	char *name;
	int f3, op, fp;
} stores[] = {
	{"sb", 0, 0x23, 0}, {"sh", 1, 0x23, 0}, {"sw", 2, 0x23, 0}, {"sd", 3, 0x23, 0},
	{"fsw", 2, 0x27, 1}, {"fsd", 3, 0x27, 1},
	{0, 0, 0, 0},
};

static struct {
	char *name;
	int f3, swap;   /* swap: operandy prohozené (bgt = blt b,a) */
	int zero;       /* 1: rs2 = x0 (beqz), 2: rs1 = x0 (blez) */
} branches[] = {
	{"beq", 0, 0, 0}, {"bne", 1, 0, 0}, {"blt", 4, 0, 0}, {"bge", 5, 0, 0},
	{"bltu", 6, 0, 0}, {"bgeu", 7, 0, 0},
	{"bgt", 4, 1, 0}, {"ble", 5, 1, 0}, {"bgtu", 6, 1, 0}, {"bleu", 7, 1, 0},
	{"beqz", 0, 0, 1}, {"bnez", 1, 0, 1}, {"bltz", 4, 0, 1}, {"bgez", 5, 0, 1},
	{"blez", 5, 0, 2}, {"bgtz", 4, 0, 2},
	{0, 0, 0, 0},
};

static struct {
	char *name;
	int f5;
} amos[] = {
	{"lr", 2}, {"sc", 3}, {"amoswap", 1}, {"amoadd", 0}, {"amoxor", 4}, {"amoand", 12},
	{"amoor", 8}, {"amomin", 16}, {"amomax", 20}, {"amominu", 24}, {"amomaxu", 28},
	{0, 0},
};

/* FP operace tří registrů: f7 pro .s (.d = +1), zaokrouhlení ano/ne */
static struct {
	char *name;
	int f7, f3, rm;
} fops3[] = {
	{"fadd", 0x00, 0, 1}, {"fsub", 0x04, 0, 1}, {"fmul", 0x08, 0, 1}, {"fdiv", 0x0c, 0, 1},
	{"fsgnj", 0x10, 0, 0}, {"fsgnjn", 0x10, 1, 0}, {"fsgnjx", 0x10, 2, 0},
	{"fmin", 0x14, 0, 0}, {"fmax", 0x14, 1, 0},
	{0, 0, 0, 0},
};

/* ---- instrukce ---- */

static int
rv64insn(char *mnem, char *ops)
{
	AOp o[4];
	int no, n, rd, rs1, rs2, f3, f7, rm, aq, rl, d;
	int64_t v;
	char *suf, *dot;
	char base[32];

	curmnem = mnem;
	no = parseops(ops, o, 4);
	if (strlen(mnem) >= sizeof base)
		return 0;
	strcpy(base, mnem);

	/* R-typ a alias s immediate */
	for (n=0; rops[n].name; n++)
		if (strcmp(mnem, rops[n].name) == 0) {
			if (no != 3)
				rerr("three operands expected");
			rd = xreg(&o[0]);
			rs1 = xreg(&o[1]);
			if (o[2].kind == 3) {
				if (!rops[n].immop)
					rerr("register expected");
				v = constant(&o[2]);
				if (rops[n].f3 == 1 || rops[n].f3 == 5) {
					/* posun: slli/srli/srai (6 bitů), -w (5 bitů) */
					d = rops[n].op == 0x3b ? 5 : 6;
					if (v < 0 || v >= (1 << d))
						rerr("shift amount out of range");
					put32(ifmt(v | (rops[n].f7 == 0x20 ? 0x400 : 0), rs1, rops[n].f3, rd, rops[n].immop));
				} else {
					if (!fits12(v))
						rerr("immediate out of range");
					put32(ifmt(v, rs1, rops[n].f3, rd, rops[n].immop));
				}
				return 1;
			}
			rs2 = xreg(&o[2]);
			put32(rfmt(rops[n].f7, rs2, rs1, rops[n].f3, rd, rops[n].op));
			return 1;
		}
	for (n=0; iops[n].name; n++)
		if (strcmp(mnem, iops[n].name) == 0) {
			if (no != 3)
				rerr("three operands expected");
			rd = xreg(&o[0]);
			rs1 = xreg(&o[1]);
			if (o[2].kind == 3 && o[2].mod == 2) {
				put32fix(ifmt(0, rs1, iops[n].f3, rd, iops[n].op), FAbs, 0, ELoI, &o[2].e, -1);
				return 1;
			}
			if (o[2].mod)
				rerr("unsupported modifier here");
			put32(ifmt(imm12(&o[2]), rs1, iops[n].f3, rd, iops[n].op));
			return 1;
		}
	if (strcmp(mnem, "slli") == 0 || strcmp(mnem, "srli") == 0 || strcmp(mnem, "srai") == 0
	|| strcmp(mnem, "slliw") == 0 || strcmp(mnem, "srliw") == 0 || strcmp(mnem, "sraiw") == 0) {
		if (no != 3)
			rerr("three operands expected");
		rd = xreg(&o[0]);
		rs1 = xreg(&o[1]);
		v = constant(&o[2]);
		d = mnem[4] == 'w' ? 5 : 6;
		if (v < 0 || v >= (1 << d))
			rerr("shift amount out of range");
		f3 = mnem[1] == 'l' ? 1 : 5;
		put32(ifmt(v | (mnem[2] == 'a' ? 0x400 : 0), rs1, f3, rd, d == 5 ? 0x1b : 0x13));
		return 1;
	}

	/* loady: imm(reg), (reg), symbol [, tmp] */
	for (n=0; loads[n].name; n++)
		if (strcmp(mnem, loads[n].name) == 0) {
			if (no < 2 || no > 3)
				rerr("two operands expected");
			rd = loads[n].fp ? freg(&o[0]) : xreg(&o[0]);
			if (o[1].kind == 4) {
				if (no != 2)
					rerr("two operands expected");
				if (o[1].mod == 2) {
					put32fix(ifmt(0, o[1].reg, loads[n].f3, rd, loads[n].op), FAbs, 0, ELoI, &o[1].e, -1);
					return 1;
				}
				if (o[1].mod || o[1].e.sym >= 0 || !fits12(o[1].e.add))
					rerr("offset out of range");
				put32(ifmt(o[1].e.add, o[1].reg, loads[n].f3, rd, loads[n].op));
				return 1;
			}
			if (o[1].kind != 3 || o[1].e.sym < 0)
				rerr("memory operand or symbol expected");
			if (loads[n].fp) {
				if (no != 3)
					rerr("temporary register expected");
				rs1 = xreg(&o[2]);
			} else {
				if (no == 3)
					rerr("two operands expected");
				rs1 = rd;
			}
			pcrelpair(rs1, &o[1].e, ifmt(0, rs1, loads[n].f3, rd, loads[n].op), EPcLoI);
			return 1;
		}
	for (n=0; stores[n].name; n++)
		if (strcmp(mnem, stores[n].name) == 0) {
			if (no < 2 || no > 3)
				rerr("two operands expected");
			rs2 = stores[n].fp ? freg(&o[0]) : xreg(&o[0]);
			if (o[1].kind == 4) {
				if (no != 2)
					rerr("two operands expected");
				if (o[1].mod == 2) {
					put32fix(sfmt(0, rs2, o[1].reg, stores[n].f3, stores[n].op), FAbs, 0, ELoS, &o[1].e, -1);
					return 1;
				}
				if (o[1].mod || o[1].e.sym >= 0 || !fits12(o[1].e.add))
					rerr("offset out of range");
				put32(sfmt(o[1].e.add, rs2, o[1].reg, stores[n].f3, stores[n].op));
				return 1;
			}
			if (o[1].kind != 3 || o[1].e.sym < 0 || no != 3)
				rerr("memory operand or symbol and temporary expected");
			rs1 = xreg(&o[2]);
			pcrelpair(rs1, &o[1].e, sfmt(0, rs2, rs1, stores[n].f3, stores[n].op), EPcLoS);
			return 1;
		}

	/* větve */
	for (n=0; branches[n].name; n++)
		if (strcmp(mnem, branches[n].name) == 0) {
			if (branches[n].zero) {
				if (no != 2)
					rerr("two operands expected");
				rs1 = xreg(&o[0]);
				if (branches[n].zero == 1)
					branch(branches[n].f3, rs1, 0, &o[1]);
				else
					branch(branches[n].f3, 0, rs1, &o[1]);
				return 1;
			}
			if (no != 3)
				rerr("three operands expected");
			rs1 = xreg(&o[0]);
			rs2 = xreg(&o[1]);
			if (branches[n].swap)
				branch(branches[n].f3, rs2, rs1, &o[2]);
			else
				branch(branches[n].f3, rs1, rs2, &o[2]);
			return 1;
		}

	/* skoky a volání */
	if (strcmp(mnem, "j") == 0) {
		if (no != 1)
			rerr("one operand expected");
		jump(0, &o[0]);
		return 1;
	}
	if (strcmp(mnem, "jal") == 0) {
		if (no == 1)
			jump(1, &o[0]);
		else if (no == 2)
			jump(xreg(&o[0]), &o[1]);
		else
			rerr("one or two operands expected");
		return 1;
	}
	if (strcmp(mnem, "jr") == 0) {
		if (no != 1)
			rerr("one operand expected");
		put32(ifmt(0, xreg(&o[0]), 0, 0, 0x67));
		return 1;
	}
	if (strcmp(mnem, "jalr") == 0) {
		if (no == 1) {
			if (o[0].kind == 4)
				put32(ifmt(imm12(&o[0]) * 0 + (fits12(o[0].e.add) ? o[0].e.add : 0), o[0].reg, 0, 1, 0x67));
			else
				put32(ifmt(0, xreg(&o[0]), 0, 1, 0x67));
		} else if (no == 2) {
			rd = xreg(&o[0]);
			if (o[1].kind == 4) {
				if (o[1].e.sym >= 0 || !fits12(o[1].e.add))
					rerr("offset out of range");
				put32(ifmt(o[1].e.add, o[1].reg, 0, rd, 0x67));
			} else
				put32(ifmt(0, xreg(&o[1]), 0, rd, 0x67));
		} else if (no == 3)
			put32(ifmt(imm12(&o[2]), xreg(&o[1]), 0, xreg(&o[0]), 0x67));
		else
			rerr("one to three operands expected");
		return 1;
	}
	if (strcmp(mnem, "ret") == 0) {
		put32(ifmt(0, 1, 0, 0, 0x67));
		return 1;
	}
	if (strcmp(mnem, "call") == 0) {
		if (no != 1)
			rerr("one operand expected");
		callinsn(1, &o[0]);
		return 1;
	}
	if (strcmp(mnem, "tail") == 0) {
		if (no != 1)
			rerr("one operand expected");
		if (o[0].kind != 3 || o[0].e.sym < 0)
			rerr("call target expected");
		put32fix(ufmt(0, 6, 0x17), FPcrel, 0, ECall, &o[0].e, -1);
		put32(ifmt(0, 6, 0, 0, 0x67));
		return 1;
	}

	/* konstanty a adresy */
	if (strcmp(mnem, "li") == 0) {
		if (no != 2)
			rerr("two operands expected");
		loadconst(xreg(&o[0]), constant(&o[1]), 1);
		return 1;
	}
	if (strcmp(mnem, "la") == 0 || strcmp(mnem, "lla") == 0) {
		if (no != 2)
			rerr("two operands expected");
		rd = xreg(&o[0]);
		if (o[1].kind != 3 || o[1].e.sym < 0)
			rerr("symbol expected");
		pcrelpair(rd, &o[1].e, ifmt(0, rd, 0, rd, 0x13), EPcLoI);
		return 1;
	}
	if (strcmp(mnem, "lui") == 0 || strcmp(mnem, "auipc") == 0) {
		if (no != 2)
			rerr("two operands expected");
		rd = xreg(&o[0]);
		if (o[1].kind == 3 && o[1].mod == 1 && mnem[0] == 'l') {
			put32fix(ufmt(0, rd, 0x37), FAbs, 0, EHi, &o[1].e, -1);
			return 1;
		}
		if (o[1].kind == 3 && o[1].mod == 3 && mnem[0] == 'a') {
			put32fix(ufmt(0, rd, 0x17), FPcrel, 0, EPcHi, &o[1].e, -1);
			return 1;
		}
		if (o[1].mod)
			rerr("unsupported modifier here");
		v = constant(&o[1]);
		if (v < -0x80000 || v > 0xfffff)
			rerr("immediate out of range");
		put32(ufmt(v, rd, mnem[0] == 'l' ? 0x37 : 0x17));
		return 1;
	}

	/* pseudoinstrukce dvou registrů */
	if (strcmp(mnem, "mv") == 0 || strcmp(mnem, "not") == 0 || strcmp(mnem, "neg") == 0
	|| strcmp(mnem, "negw") == 0 || strcmp(mnem, "sext.w") == 0 || strcmp(mnem, "seqz") == 0
	|| strcmp(mnem, "snez") == 0 || strcmp(mnem, "sltz") == 0 || strcmp(mnem, "sgtz") == 0
	|| strcmp(mnem, "zext.b") == 0 || strcmp(mnem, "zext.h") == 0 || strcmp(mnem, "zext.w") == 0
	|| strcmp(mnem, "sext.b") == 0 || strcmp(mnem, "sext.h") == 0) {
		if (no != 2)
			rerr("two operands expected");
		rd = xreg(&o[0]);
		rs1 = xreg(&o[1]);
		if (strcmp(mnem, "mv") == 0)
			put32(ifmt(0, rs1, 0, rd, 0x13));
		else if (strcmp(mnem, "not") == 0)
			put32(ifmt(-1, rs1, 4, rd, 0x13));
		else if (strcmp(mnem, "neg") == 0)
			put32(rfmt(0x20, rs1, 0, 0, rd, 0x33));
		else if (strcmp(mnem, "negw") == 0)
			put32(rfmt(0x20, rs1, 0, 0, rd, 0x3b));
		else if (strcmp(mnem, "sext.w") == 0)
			put32(ifmt(0, rs1, 0, rd, 0x1b));
		else if (strcmp(mnem, "seqz") == 0)
			put32(ifmt(1, rs1, 3, rd, 0x13));
		else if (strcmp(mnem, "snez") == 0)
			put32(rfmt(0, rs1, 0, 3, rd, 0x33));
		else if (strcmp(mnem, "sltz") == 0)
			put32(rfmt(0, 0, rs1, 2, rd, 0x33));
		else if (strcmp(mnem, "sgtz") == 0)
			put32(rfmt(0, rs1, 0, 2, rd, 0x33));
		else if (strcmp(mnem, "zext.b") == 0)
			put32(ifmt(255, rs1, 7, rd, 0x13));
		else {
			/* bez Zbb: slli a srli/srai o 64 - šířka */
			d = mnem[5] == 'b' ? 56 : mnem[5] == 'h' ? 48 : 32;
			put32(ifmt(d, rs1, 1, rd, 0x13));
			put32(ifmt(d | (mnem[0] == 's' ? 0x400 : 0), rd, 5, rd, 0x13));
		}
		return 1;
	}
	if (strcmp(mnem, "nop") == 0) {
		put32(0x13);
		return 1;
	}

	/* systém */
	if (strcmp(mnem, "ecall") == 0) { put32(0x73); return 1; }
	if (strcmp(mnem, "ebreak") == 0) { put32(0x00100073); return 1; }
	if (strcmp(mnem, "wfi") == 0) { put32(0x10500073); return 1; }
	if (strcmp(mnem, "mret") == 0) { put32(0x30200073); return 1; }
	if (strcmp(mnem, "sret") == 0) { put32(0x10200073); return 1; }
	if (strcmp(mnem, "fence.i") == 0) { put32(0x0000100f); return 1; }
	if (strcmp(mnem, "fence") == 0) {
		if (no == 0)
			put32(0x0ff0000f);
		else if (no == 2 && o[0].kind == 5 && o[1].kind == 5)
			put32(((uint32_t)fencemask(o[0].word) << 24) | ((uint32_t)fencemask(o[1].word) << 20) | 0x0f);
		else
			rerr("fence sets expected");
		return 1;
	}
	if (strcmp(mnem, "sfence.vma") == 0) {
		rs1 = no > 0 ? xreg(&o[0]) : 0;
		rs2 = no > 1 ? xreg(&o[1]) : 0;
		if (no > 2)
			rerr("at most two operands expected");
		put32(rfmt(0x09, rs2, rs1, 0, 0, 0x73));
		return 1;
	}
	if (strncmp(mnem, "csr", 3) == 0) {
		suf = mnem + 3;
		if (strcmp(suf, "r") == 0) {
			if (no != 2)
				rerr("two operands expected");
			put32(ifmt(csrnum(&o[1]), 0, 2, xreg(&o[0]), 0x73));
			return 1;
		}
		if (strcmp(suf, "w") == 0 || strcmp(suf, "s") == 0 || strcmp(suf, "c") == 0) {
			if (no != 2)
				rerr("two operands expected");
			f3 = *suf == 'w' ? 1 : *suf == 's' ? 2 : 3;
			put32(ifmt(csrnum(&o[0]), xreg(&o[1]), f3, 0, 0x73));
			return 1;
		}
		if (strcmp(suf, "wi") == 0 || strcmp(suf, "si") == 0 || strcmp(suf, "ci") == 0) {
			if (no != 2)
				rerr("two operands expected");
			f3 = *suf == 'w' ? 5 : *suf == 's' ? 6 : 7;
			v = constant(&o[1]);
			if (v < 0 || v > 31)
				rerr("immediate out of range");
			put32(ifmt(csrnum(&o[0]), v, f3, 0, 0x73));
			return 1;
		}
		if (strcmp(suf, "rw") == 0 || strcmp(suf, "rs") == 0 || strcmp(suf, "rc") == 0) {
			if (no != 3)
				rerr("three operands expected");
			f3 = suf[1] == 'w' ? 1 : suf[1] == 's' ? 2 : 3;
			put32(ifmt(csrnum(&o[1]), xreg(&o[2]), f3, xreg(&o[0]), 0x73));
			return 1;
		}
		if (strcmp(suf, "rwi") == 0 || strcmp(suf, "rsi") == 0 || strcmp(suf, "rci") == 0) {
			if (no != 3)
				rerr("three operands expected");
			f3 = suf[1] == 'w' ? 5 : suf[1] == 's' ? 6 : 7;
			v = constant(&o[2]);
			if (v < 0 || v > 31)
				rerr("immediate out of range");
			put32(ifmt(csrnum(&o[1]), v, f3, xreg(&o[0]), 0x73));
			return 1;
		}
		return 0;
	}
	if (strcmp(mnem, "rdtime") == 0 || strcmp(mnem, "rdcycle") == 0 || strcmp(mnem, "rdinstret") == 0) {
		if (no != 1)
			rerr("one operand expected");
		put32(ifmt(mnem[2] == 't' ? 0xc01 : mnem[2] == 'c' ? 0xc00 : 0xc02, 0, 2, xreg(&o[0]), 0x73));
		return 1;
	}
	if (strcmp(mnem, "frcsr") == 0 || strcmp(mnem, "frrm") == 0 || strcmp(mnem, "frflags") == 0) {
		if (no != 1)
			rerr("one operand expected");
		put32(ifmt(mnem[2] == 'c' ? 3 : mnem[2] == 'r' ? 2 : 1, 0, 2, xreg(&o[0]), 0x73));
		return 1;
	}
	if (strcmp(mnem, "fscsr") == 0 || strcmp(mnem, "fsrm") == 0 || strcmp(mnem, "fsflags") == 0) {
		n = mnem[2] == 'c' ? 3 : mnem[2] == 'r' ? 2 : 1;
		if (no == 1)
			put32(ifmt(n, xreg(&o[0]), 1, 0, 0x73));
		else if (no == 2)
			put32(ifmt(n, xreg(&o[1]), 1, xreg(&o[0]), 0x73));
		else
			rerr("one or two operands expected");
		return 1;
	}

	/* atomické: jméno.w|d[.aq|.rl|.aqrl] */
	dot = strchr(mnem, '.');
	if (dot) {
		n = dot - mnem;
		if (n < (int)sizeof base) {
			memcpy(base, mnem, n);
			base[n] = 0;
		}
		for (n=0; amos[n].name; n++)
			if (strcmp(base, amos[n].name) == 0) {
				suf = dot + 1;
				if (*suf == 'w')
					f3 = 2;
				else if (*suf == 'd')
					f3 = 3;
				else
					rerr("unknown width");
				suf++;
				aq = rl = 0;
				if (strcmp(suf, ".aq") == 0)
					aq = 1;
				else if (strcmp(suf, ".rl") == 0)
					rl = 1;
				else if (strcmp(suf, ".aqrl") == 0)
					aq = rl = 1;
				else if (*suf)
					rerr("unknown ordering suffix");
				f7 = (amos[n].f5 << 2) | (aq << 1) | rl;
				if (amos[n].f5 == 2) {
					if (no != 2 || o[1].kind != 4 || o[1].e.sym >= 0 || o[1].e.add != 0)
						rerr("lr: rd, (rs1) expected");
					put32(rfmt(f7, 0, o[1].reg, f3, xreg(&o[0]), 0x2f));
				} else {
					if (no != 3 || o[2].kind != 4 || o[2].e.sym >= 0 || o[2].e.add != 0)
						rerr("rd, rs2, (rs1) expected");
					put32(rfmt(f7, xreg(&o[1]), o[2].reg, f3, xreg(&o[0]), 0x2f));
				}
				return 1;
			}
	}

	/* plovoucí řádka: jméno.s|d */
	if (dot && (strcmp(dot, ".s") == 0 || strcmp(dot, ".d") == 0)) {
		d = dot[1] == 'd';
		for (n=0; fops3[n].name; n++)
			if (strcmp(base, fops3[n].name) == 0) {
				if (no < 3 || no > 3 + fops3[n].rm)
					rerr("three operands expected");
				rm = fops3[n].rm ? rmode(o, no, 3) : fops3[n].f3;
				put32(rfmt(fops3[n].f7 + d, freg(&o[2]), freg(&o[1]), rm, freg(&o[0]), 0x53));
				return 1;
			}
		if (strcmp(base, "fsqrt") == 0) {
			if (no < 2 || no > 3)
				rerr("two operands expected");
			put32(rfmt(0x2c + d, 0, freg(&o[1]), rmode(o, no, 2), freg(&o[0]), 0x53));
			return 1;
		}
		if (strcmp(base, "fmv") == 0 || strcmp(base, "fneg") == 0 || strcmp(base, "fabs") == 0) {
			if (no != 2)
				rerr("two operands expected");
			f3 = base[1] == 'm' ? 0 : base[1] == 'n' ? 1 : 2;
			rs1 = freg(&o[1]);
			put32(rfmt(0x10 + d, rs1, rs1, f3, freg(&o[0]), 0x53));
			return 1;
		}
		if (strcmp(base, "feq") == 0 || strcmp(base, "flt") == 0 || strcmp(base, "fle") == 0
		|| strcmp(base, "fgt") == 0 || strcmp(base, "fge") == 0) {
			if (no != 3)
				rerr("three operands expected");
			f3 = base[1] == 'e' ? 2 : (base[1] == 'l' && base[2] == 't') || base[2] == 't' ? 1 : 0;
			if (base[1] == 'g')
				put32(rfmt(0x50 + d, freg(&o[1]), freg(&o[2]), f3, xreg(&o[0]), 0x53));
			else
				put32(rfmt(0x50 + d, freg(&o[2]), freg(&o[1]), f3, xreg(&o[0]), 0x53));
			return 1;
		}
		if (strcmp(base, "fclass") == 0) {
			if (no != 2)
				rerr("two operands expected");
			put32(rfmt(0x70 + d, 0, freg(&o[1]), 1, xreg(&o[0]), 0x53));
			return 1;
		}
		if (strcmp(base, "fmadd") == 0 || strcmp(base, "fmsub") == 0
		|| strcmp(base, "fnmsub") == 0 || strcmp(base, "fnmadd") == 0) {
			if (no < 4 || no > 4)
				rerr("four operands expected");
			n = base[1] == 'm' ? (base[2] == 'a' ? 0x43 : 0x47) : (base[2] == 'm' && base[3] == 's' ? 0x4b : 0x4f);
			put32(((uint32_t)freg(&o[3]) << 27) | ((uint32_t)d << 25) | ((uint32_t)freg(&o[2]) << 20)
				| ((uint32_t)freg(&o[1]) << 15) | (7u << 12) | ((uint32_t)freg(&o[0]) << 7) | n);
			return 1;
		}
	}
	/* fmv.x.w, fmv.w.x, fmv.x.d, fmv.d.x; fcvt.*.* */
	if (strncmp(mnem, "fmv.", 4) == 0) {
		if (no != 2)
			rerr("two operands expected");
		if (strcmp(mnem, "fmv.x.w") == 0)
			put32(rfmt(0x70, 0, freg(&o[1]), 0, xreg(&o[0]), 0x53));
		else if (strcmp(mnem, "fmv.x.d") == 0)
			put32(rfmt(0x71, 0, freg(&o[1]), 0, xreg(&o[0]), 0x53));
		else if (strcmp(mnem, "fmv.w.x") == 0)
			put32(rfmt(0x78, 0, xreg(&o[1]), 0, freg(&o[0]), 0x53));
		else if (strcmp(mnem, "fmv.d.x") == 0)
			put32(rfmt(0x79, 0, xreg(&o[1]), 0, freg(&o[0]), 0x53));
		else
			return 0;
		return 1;
	}
	if (strncmp(mnem, "fcvt.", 5) == 0) {
		char *to, *from;
		int tf, ff;   /* 0 w, 1 wu, 2 l, 3 lu; 4 s, 5 d */
		static char *kinds[] = {"w", "wu", "l", "lu", "s", "d"};

		if (no < 2 || no > 3)
			rerr("two operands expected");
		to = mnem + 5;
		from = strchr(to, '.');
		if (!from)
			return 0;
		n = from - to;
		if (n >= (int)sizeof base)
			return 0;
		memcpy(base, to, n);
		base[n] = 0;
		from++;
		tf = ff = -1;
		for (n=0; n<6; n++) {
			if (strcmp(base, kinds[n]) == 0)
				tf = n;
			if (strcmp(from, kinds[n]) == 0)
				ff = n;
		}
		if (tf < 0 || ff < 0)
			return 0;
		if (tf <= 3 && ff >= 4) {
			/* do celého čísla: f7 0x60 (.s) / 0x61 (.d), rs2 = druh */
			put32(rfmt(0x60 + (ff == 5), tf, freg(&o[1]), rmode(o, no, 2), xreg(&o[0]), 0x53));
			return 1;
		}
		if (tf >= 4 && ff <= 3) {
			/* z celého čísla: f7 0x68/0x69; z 32bitového do double přesně
			 * (zaokrouhlení 0 jako GNU as) */
			d = tf == 5;
			rm = d && ff <= 1 ? 0 : rmode(o, no, 2);
			put32(rfmt(0x68 + d, ff, xreg(&o[1]), rm, freg(&o[0]), 0x53));
			return 1;
		}
		if (tf == 4 && ff == 5) {
			put32(rfmt(0x20, 1, freg(&o[1]), rmode(o, no, 2), freg(&o[0]), 0x53));
			return 1;
		}
		if (tf == 5 && ff == 4) {
			put32(rfmt(0x21, 0, freg(&o[1]), 0, freg(&o[0]), 0x53));
			return 1;
		}
		return 0;
	}
	return 0;
}

/* ---- skoky (IJump): krátká větev nebo obrácená větev + jal ---- */

static uint
rvjsize(AItem *i)
{
	return i->jlong ? 8 : 4;
}

/* d je od začátku větve (relaxmode 1) */
static int
rvjfits(AItem *i, int64_t d)
{
	(void)i;
	return d >= -4096 && d < 4096;
}

static void
putw(uchar *b, uint32_t w)
{
	b[0] = w;
	b[1] = w >> 8;
	b[2] = w >> 16;
	b[3] = w >> 24;
}

static void
rvjemit(AItem *i, uchar *b, int64_t d, int r)
{
	uint32_t w;
	int64_t v;

	w = (uint32_t)i->jop;
	v = d + (int64_t)i->size;   /* od začátku větve */
	if (!i->jlong) {
		if (!r)
			aerr("short branch to an unresolved target");
		if (v & 1)
			aerr("branch target is not aligned");
		putw(b, w | bimm(v));
		return;
	}
	/* obrácená podmínka přes následující jal */
	putw(b, (w ^ (1u << 12)) | bimm(8));
	if (r) {
		v -= 4;
		if (v & 1)
			aerr("jump target is not aligned");
		if (v < -(1 << 20) || v >= (1 << 20))
			aerr("jump out of range");
		putw(b + 4, 0x6f | jimm(v));
	} else
		putw(b + 4, 0x6f);
}

static int
rvjenc(AItem *i)
{
	return i->jlong ? EJal : EBranch;
}

static void
rvnops(uchar *b, uint n)
{
	uint k;

	if (n % 4)
		aerr("alignment padding in code is not a multiple of 4");
	for (k=0; k<n; k+=4)
		putw(b + k, 0x13);
}

static int
rvreloc(AFix *f)
{
	switch (f->enc) {
	case ENone:
		if (f->kind == FAbs)
			return f->size == 8 ? 2 : f->size == 4 ? 1 : 0;
		return 0;
	case EBranch: return 16;
	case EJal: return 17;
	case ECall: return 19;
	case EPcHi: return 23;
	case EPcLoI: return 24;
	case EPcLoS: return 25;
	case EHi: return 26;
	case ELoI: return 27;
	case ELoS: return 28;
	}
	return 0;
}

static uint32_t
getw(uchar *b)
{
	return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void
rvfixenc(uchar *b, AFix *f, int64_t v)
{
	uint32_t w;
	int64_t hi, lo;

	w = getw(b);
	switch (f->enc) {
	case EBranch:
		if (v & 1)
			aerr("branch target is not aligned");
		if (v < -4096 || v >= 4096)
			aerr("branch out of range");
		putw(b, w | bimm(v));
		break;
	case EJal:
		if (v & 1)
			aerr("jump target is not aligned");
		if (v < -(1 << 20) || v >= (1 << 20))
			aerr("jump out of range");
		putw(b, w | jimm(v));
		break;
	case ECall:
	case EPcHi:
	case EPcLoI:
	case EPcLoS:
		hi = (v + 0x800) >> 12;
		lo = v - (hi << 12);
		if (hi < -(1 << 19) || hi >= (1 << 19))
			aerr("pc-relative address out of range");
		if (f->enc == ECall) {
			putw(b, w | ((uint32_t)(hi & 0xfffff) << 12));
			putw(b + 4, getw(b + 4) | ((uint32_t)(lo & 0xfff) << 20));
		} else if (f->enc == EPcHi)
			putw(b, w | ((uint32_t)(hi & 0xfffff) << 12));
		else if (f->enc == EPcLoI)
			putw(b, w | ((uint32_t)(lo & 0xfff) << 20));
		else
			putw(b, w | ((uint32_t)((lo >> 5) & 0x7f) << 25) | ((uint32_t)(lo & 0x1f) << 7));
		break;
	default:
		aerr("cannot resolve relocation locally");
	}
}

/* předběžná hodnota pole s relokací: bez kontroly dosahu (GNU as);
 * u větví a skoků S + A - P, u %hi/%lo S + A (S = posun v sekci) */
static void
rvtentative(uchar *b, AFix *f, int64_t v)
{
	uint32_t w;
	int64_t hi, lo;

	w = getw(b);
	hi = (v + 0x800) >> 12;
	lo = v - (hi << 12);
	switch (f->enc) {
	case EBranch: putw(b, w | bimm(v)); break;
	case EJal: putw(b, w | jimm(v)); break;
	case EHi: putw(b, w | ((uint32_t)(hi & 0xfffff) << 12)); break;
	case ELoI: putw(b, w | ((uint32_t)(lo & 0xfff) << 20)); break;
	case ELoS: putw(b, w | ((uint32_t)((lo >> 5) & 0x7f) << 25) | ((uint32_t)(lo & 0x1f) << 7)); break;
	}
}

AArch ARV64 = {
	.name = "rv64",
	.machine = 243,
	.elf64 = 1,
	.insn = rv64insn,
	.jsize = rvjsize,
	.jfits = rvjfits,
	.jemit = rvjemit,
	.jenc = rvjenc,
	.tentative = rvtentative,
	.nops = rvnops,
	.reloc = rvreloc,
	.fixenc = rvfixenc,
	.resolveglobal = 0,
	.minalign = 1,   /* zarovnání sekce jen z direktiv (GNU as pro RISC-V) */
	.comment = '#',
	.wordsize = 4,
	.keeplocalsym = 2,
	.relaxmode = 1,
	.localonly = 1,
	.eflags = 4,   /* EF_RISCV_FLOAT_ABI_DOUBLE */
	.padcode = 1,
	.textalign = 4,
};

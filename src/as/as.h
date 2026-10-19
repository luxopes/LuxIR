#include "../all.h"
#include <ctype.h>
#include <stdarg.h>

/* Vestavěný assembler LuxIR: čte dialekt GNU as, který vypisují emitory
 * LuxIR a ručně psané vrstvy architektury runtime LSL, a zapíše
 * relokovatelný objekt ELF. Jádro (as.c) zná direktivy, symboly, sekce,
 * výrazy, zarovnání a relaxaci skoků; kodér architektury (x86.c, …)
 * rozebere instrukci a zapíše její bajty s opravami (fixup).
 *
 * Obsah sekce je posloupnost položek: bajty s opravami, skok na návěští
 * s krátkou a dlouhou formou (relaxace) a zarovnání. Návěští ukazuje na
 * začátek položky. Po rozvržení se každá oprava buď vyřeší na hodnotu,
 * nebo z ní vznikne relokace, jako to dělá GNU as: relativní odkaz na
 * místní symbol téže sekce se vyřeší, na globální zůstane relokace
 * (s výjimkou skoků), absolutní odkaz je vždy relokace. */

typedef struct AExpr AExpr;
typedef struct AFix AFix;
typedef struct AItem AItem;
typedef struct ASec ASec;
typedef struct ASymbol ASymbol;
typedef struct AArch AArch;

enum {
	SAlloc = 1,
	SWrite = 2,
	SExec = 4,
	SNobits = 8,
	STls = 16,
	SNote = 32,
};

/* hodnota: sym + add (sym: index symbolu nebo -1) */
struct AExpr {
	int sym;
	int64_t add;
};

enum {
	FAbs,    /* absolutní hodnota symbolu */
	FPcrel,  /* relativně k místu opravy: S + A - P */
	FBranch, /* FPcrel pro skoky a volání (relokace PLT32 na x86-64) */
};

struct AFix {
	uint off;      /* posun v položce */
	int size;      /* 1, 2, 4, 8 bajtů */
	int kind;
	int64_t pcoff; /* k A u FPcrel: záporný počet bajtů za polem do konce instrukce, -velikost pole */
	int enc;       /* 0 prosté pole; jinak kódování v instrukci (architektura) */
	AExpr e;
};

enum {
	IData,
	IJump,
	IAlign,
};

struct AItem {
	int kind;
	uint64_t off;   /* posun v sekci po rozvržení */
	uint size;
	uchar *data;    /* IData */
	uint cap;
	AFix *fix;
	uint nfix;
	int jop;        /* IJump: operace (architektura) */
	int jlong;      /* 1 = dlouhá forma */
	AExpr target;
	uint align;     /* IAlign */
	int nops;       /* výplň instrukcemi nop (kód), jinak nuly */
	int line;
};

struct ASec {
	char *name;
	int flags;
	int type;       /* sh_type */
	int align;
	AItem *item;
	uint nitem;
	uint64_t size;
	int symidx;     /* symbol sekce (-1 bez něj) */
	int elfidx;
	int relaidx;
	uint nrel;
};

enum {
	TNone,
	TFunc,
	TObject,
};

struct ASymbol {
	char *name;
	int sec;        /* -1 nedefinovaný */
	uint item;      /* položka, na jejímž začátku leží */
	uint64_t val;   /* posun v sekci po rozvržení */
	char global;
	char weak;
	char defined;
	char type;
	char local;     /* .L…: nejde do tabulky symbolů */
	char hassize;
	AExpr size;     /* velikost: size - hodnota sizesub (je-li >= 0) */
	int sizesub;
	int64_t common; /* .comm: zarovnání (symbol SHN_COMMON), jinak 0 */
};

/* kodér architektury */
struct AArch {
	char *name;
	int machine;      /* e_machine */
	int elf64;
	/* rozbor a zápis instrukce (mnemonika, zbytek řádku); 0 = neznámá */
	int (*insn)(char *mnem, char *ops);
	/* IJump: velikost formy, dosah krátké formy a zápis */
	uint (*jsize)(AItem *);
	int (*jfits)(AItem *, int64_t disp);
	void (*jemit)(AItem *, uchar *, int64_t disp, int resolved);
	/* výplň zarovnání v kódu */
	void (*nops)(uchar *, uint n);
	/* typ relokace ELF pro opravu (0 = nelze) */
	int (*reloc)(AFix *);
	/* zápis vyřešené hodnoty do instrukce pro enc != 0 (0 = jen prostá pole) */
	void (*fixenc)(uchar *, AFix *, int64_t);
	/* skoky na globální symbol téže sekce se vyřeší (x86 jako GNU as),
	 * jinak zůstane relokace (arm64) */
	int resolveglobal;
	/* nejmenší zarovnání sekce s instrukcemi */
	int minalign;
	/* znak komentáře do konce řádku vedle //: '#' na x86, žádný na arm64 */
	int comment;
	/* velikost .word: 2 na x86, 4 na arm64 (.xword 8) */
	int wordsize;
	/* relokace na místní funkce (.type function) míří na symbol, ne na
	 * sekci (GNU as pro arm64); .L a datové symboly vždy přes sekci */
	int keeplocalsym;
};

/* as.c */
extern AArch *A;
extern ASec *asecs;
extern uint nasecs;
extern ASymbol *asyms;
extern uint nasyms;
extern int acursec;
extern int aline;
void aerr(char *, ...);
void awarn(char *, ...);
char *aword(char **ps);
void aspace(char **ps);
int aexpr(char **ps, AExpr *e);
int anumber(char **ps, int64_t *v);
int asymidx(char *name);
uchar *aemit(uint n);
void afix(uint off, int size, int kind, int64_t pcoff, AExpr *e);
void afixenc(uint off, int size, int kind, int64_t pcoff, int enc, AExpr *e);
void ajump(int jop, AExpr *e);
int assemble(char **paths, int npaths, char *out, AArch *arch);

/* x86.c */
extern AArch AX86_64;
extern AArch AI386;

/* arm64.c */
extern AArch AARM64;

#include "all.h"
#include "config.h"
#include "version.h"
#include "as/as.h"
#include "ld/ld.h"
#include <ctype.h>
#include <getopt.h>

Target T;
int pic;
int wasmthreads;
char *tmpasm;

/* Vnitřní chyba (die): dočasný assembler režimu -x nesmí zůstat. */
void
luxir_abort(void)
{
	if (tmpasm)
		luxir_unlink(tmpasm);
	abort();
}

char debug['Z'+1] = {
	['P'] = 0, /* parsing */
	['M'] = 0, /* memory optimization */
	['N'] = 0, /* ssa construction */
	['C'] = 0, /* copy elimination */
	['G'] = 0, /* gvn/gcm */
	['K'] = 0, /* if-conversion */
	['A'] = 0, /* abi lowering */
	['I'] = 0, /* instruction selection */
	['L'] = 0, /* liveness */
	['S'] = 0, /* spilling */
	['R'] = 0, /* reg. allocation */
};

extern Target T_amd64_sysv;
extern Target T_amd64_apple;
extern Target T_amd64_win;
extern Target T_arm64;
extern Target T_arm64_apple;
extern Target T_arm64_win;
extern Target T_rv64;
extern Target T_wasm32;
extern Target T_c32;
extern Target T_i386;
extern Target T_arm;

static Target *tlist[] = {
	&T_amd64_sysv,
	&T_amd64_apple,
	&T_amd64_win,
	&T_arm64,
	&T_arm64_apple,
	&T_arm64_win,
	&T_rv64,
	&T_wasm32,
	&T_c32,
	&T_i386,
	&T_arm,
	0
};
static FILE *outf;
static int dbg;

static void
data(Dat *d)
{
	if (dbg)
		return;
	if (T.cgen) {
		/* data jako C až na konci modulu */
		c_emitdat(d);
		if (d->type == DEnd)
			freeall();
		return;
	}
	if (T.wasm) {
		/* data se skládají až na konci modulu */
		wasm_emitdat(d);
		if (d->type == DEnd)
			freeall();
		return;
	}
	emitdat(d, outf);
	if (d->type == DEnd) {
		fputs("/* end data */\n\n", outf);
		freeall();
	}
}

static void
func(Fn *fn)
{
	uint n;

	if (dbg)
		fprintf(stderr, "**** Function %s ****", fn->name);
	if (debug['P']) {
		fprintf(stderr, "\n> After parsing:\n");
		printfn(fn, stderr);
	}
	T.abi0(fn);
	fillcfg(fn);
	filluse(fn);
	promote(fn);
	filluse(fn);
	ssa(fn);
	filluse(fn);
	ssacheck(fn);
	fillalias(fn);
	loadopt(fn);
	filluse(fn);
	fillalias(fn);
	coalesce(fn);
	filluse(fn);
	filldom(fn);
	ssacheck(fn);
	gvn(fn);
	fillcfg(fn);
	simplcfg(fn);
	filluse(fn);
	filldom(fn);
	gcm(fn);
	filluse(fn);
	ssacheck(fn);
	if (T.cansel) {
		ifconvert(fn);
		fillcfg(fn);
		filluse(fn);
		filldom(fn);
		ssacheck(fn);
	}
	if (T.cgen) {
		/* C: kód přímo z SSA (c/emit.c) */
		if (!dbg)
			T.emitfn(fn, outf);
		freeall();
		return;
	}
	if (T.wasm) {
		/* WebAssembly: kód přímo z SSA (wasm/emit.c) */
		if (!dbg) {
			T.emitfn(fn, outf);
			fprintf(outf, ";; end function %s\n\n", fn->name);
		}
		freeall();
		return;
	}
	T.abi1(fn);
	simpl(fn);
	fillcfg(fn);
	filluse(fn);
	T.isel(fn);
	fillcfg(fn);
	filllive(fn);
	fillloop(fn);
	fillcost(fn);
	spill(fn);
	rega(fn);
	fillcfg(fn);
	simpljmp(fn);
	fillcfg(fn);
	assert(fn->rpo[0] == fn->start);
	for (n=0;; n++)
		if (n == fn->nblk-1) {
			fn->rpo[n]->link = 0;
			break;
		} else
			fn->rpo[n]->link = fn->rpo[n+1];
	if (!dbg) {
		T.emitfn(fn, outf);
		fprintf(outf, "/* end function %s */\n\n", fn->name);
	} else
		fprintf(stderr, "\n");
	freeall();
}

static void
dbgfile(char *fn)
{
	emitdbgfile(fn, outf);
}

/* Režim -x: z assembleru udělá spustitelný soubor s runtime podle
 * hlavičky runtime_abi. Assembler je vestavěný (as/), linker zatím GNU ld
 * z pevné systémové cesty (nehledá se v PATH). */
static char *
systool(char *primary, char *fallback, char *label)
{
	if (luxir_exists(primary))
		return primary;
	if (luxir_exists(fallback))
		return fallback;
	fprintf(stderr, "luxir: trusted %s executable is required\n", label);
	return 0;
}

static int
findruntime(char *buf, int size, char *rtdir)
{
	char *dir;

	if (rtdir) {
		snprintf(buf, size, "%s/lsl-0/amd64_sysv.o", rtdir);
		return luxir_exists(buf);
	}
	dir = luxir_exedir();
	if (!dir)
		return 0;
	snprintf(buf, size, "%s/luxir-runtime/lsl-0/amd64_sysv.o", dir);
	if (luxir_exists(buf))
		return 1;
	snprintf(buf, size, "%s/../lib/luxir/lsl-0/amd64_sysv.o", dir);
	if (luxir_exists(buf))
		return 1;
	/* C reference src/luxir ve vývojovém checkoutu (make runtime-lsl). */
	snprintf(buf, size, "%s/../build/luxir-runtime/lsl-0/amd64_sysv.o", dir);
	return luxir_exists(buf);
}

static int
tool(char **argv, char *stage)
{
	int status;

	status = luxir_run(argv);
	if (status != 0)
		fprintf(stderr, "luxir: %s failed (status %d)\n", stage, status);
	return status;
}

static int
linkexe(char *out, char *asmpath, char *rtdir)
{
	char obj[4096], rt[4096];
	char *ld, *argv[24];
	int status;

	status = 1;
	snprintf(obj, sizeof obj, "%s.o", out);
	if (rtabi != RTlsl0)
		fprintf(stderr, "luxir: -x needs a runtime_abi \"" LUXIR_RUNTIME_ABI_LSL "\" header\n");
	else if (strcmp(T.name, "amd64_sysv") != 0)
		fprintf(stderr, "luxir: runtime " LUXIR_RUNTIME_ABI_LSL " is available only for amd64_sysv\n");
	else if (!findruntime(rt, sizeof rt, rtdir))
		fprintf(stderr, "luxir: runtime " LUXIR_RUNTIME_ABI_LSL " object is missing (luxir-runtime/lsl-0/amd64_sysv.o)\n");
	else {
		/* vestavěný assembler; linker je zatím GNU ld */
		ld = systool("/usr/bin/ld", "/bin/ld", "linker");
		if (ld)
			status = assemble(&asmpath, 1, obj, &AX86_64);
		if (ld && status == 0) {
			/* Statický PIE bez dynamického loaderu. -z text zakáže
			 * absolutní relokace ve spustitelných stránkách. */
			argv[0] = ld;
			argv[1] = "-nostdlib";
			argv[2] = "-static";
			argv[3] = "-pie";
			argv[4] = "--no-dynamic-linker";
			argv[5] = "-z";
			argv[6] = "text";
			argv[7] = "-z";
			argv[8] = "noexecstack";
			argv[9] = "--build-id=none";
			argv[10] = "-s";
			argv[11] = "-o";
			argv[12] = out;
			argv[13] = obj;
			argv[14] = rt;
			argv[15] = 0;
			status = tool(argv, "linker");
		}
	}
	luxir_unlink(asmpath);
	luxir_unlink(obj);
	if (status != 0)
		luxir_unlink(out);
	return status != 0;
}

int
main(int ac, char *av[])
{
	Target **t;
	FILE *inf, *hf;
	char *f, *sep, *out, *rtdir;
	char asmpath[4096];
	int c, exe, asmode, ldmode, pipe, nobj, k;
	char *entry, **objs, objpath[4096];

	objs = 0;
	nobj = 0;

	T = Deftgt;
	asmode = 0;
	ldmode = 0;
	entry = 0;
	outf = stdout;
	out = 0;
	rtdir = 0;
	exe = 0;
	/* Long query is deliberately standalone: never mistake an -o operand
	 * or an input filename for an option, and never truncate an output. */
	if (ac == 2 && strcmp(av[1], "--version") == 0) {
		puts(LUXIR_VERSION_TEXT);
		exit(0);
	}
	while ((c = getopt(ac, av, "hpgxalVd:o:t:L:e:")) != -1)
		switch (c) {
		case 'p':
			pic = 1;
			break;
		case 'g':
			wasmthreads = 1;
			break;
		case 'V':
			puts(LUXIR_VERSION_TEXT);
			exit(0);
		case 'd':
			for (; *optarg; optarg++)
				if (isalpha(*optarg)) {
					debug[toupper(*optarg)] = 1;
					dbg = 1;
				}
			break;
		case 'o':
			if (strcmp(optarg, "-") != 0) {
				outf = fopen(optarg, "w");
				if (!outf) {
					fprintf(stderr, "cannot open '%s'\n", optarg);
					exit(1);
				}
				out = optarg;
				c_outpath = optarg;
			} else
				out = 0;
			break;
		case 'x':
			exe = 1;
			break;
		case 'a':
			asmode = 1;
			break;
		case 'l':
			ldmode = 1;
			break;
		case 'e':
			entry = optarg;
			break;
		case 'L':
			rtdir = optarg;
			break;
		case 't':
			if (strcmp(optarg, "?") == 0) {
				puts(T.name);
				exit(0);
			}
			for (t=tlist;; t++) {
				if (!*t) {
					fprintf(stderr, "unknown target '%s'\n", optarg);
					exit(1);
				}
				if (strcmp(optarg, (*t)->name) == 0) {
					T = **t;
					break;
				}
			}
			break;
		case 'h':
		default:
			hf = c != 'h' ? stderr : stdout;
			fprintf(hf, "%s [OPTIONS] {file.lir, -}\n", av[0]);
			fprintf(hf, "\t%-11s prints this help\n", "-h");
			fprintf(hf, "\t%-11s prints IR/runtime protocol revisions (standalone)\n", "--version");
			fprintf(hf, "\t%-11s output to file\n", "-o file");
			fprintf(hf, "\t%-11s link an executable with the runtime named by runtime_abi\n", "-x");
			fprintf(hf, "\t%-11s assemble file.s into an ELF object (architecture of -t)\n", "-a");
			fprintf(hf, "\t%-11s with -l: compile .lir, assemble and link with the objects in one run\n", "-a -l");
			fprintf(hf, "\t%-11s link ELF objects into a static executable (-p: PIE, -e: entry)\n", "-l");
			fprintf(hf, "\t%-11s runtime directory for -x (default next to luxir)\n", "-L dir");
			fprintf(hf, "\t%-11s PC-relative symbol addresses (rv64 static PIE)\n", "-p");
			fprintf(hf, "\t%-11s wasm32: threads (shared memory, GC safepoints)\n", "-g");
			fprintf(hf, "\t%-11s generate for a target among:\n", "-t <target>");
			fprintf(hf, "\t%-11s ", "");
			for (t=tlist, sep=""; *t; t++, sep=", ") {
				fprintf(hf, "%s%s", sep, (*t)->name);
				if (*t == &Deftgt)
					fputs(" (default)", hf);
			}
			fprintf(hf, "\n");
			fprintf(hf, "\t%-11s dump debug information\n", "-d <flags>");
			exit(c != 'h');
		}

	/* -a -l: překlad .lir, sestavení a slinkování v jednom běhu (bez dvou
	 * dalších startů procesu): vstupy .lir se přeloží do OUT.s a sestaví
	 * do OUT.o, ostatní vstupy jsou objekty pro linker */
	pipe = asmode && ldmode;
	if (ldmode && !pipe) {
		/* vestavěný linker: objekty ELF architektury cíle */
		int is64, mach;
		if (!out || exe || asmode || dbg || !av[optind]) {
			fprintf(stderr, "luxir: -l needs -o file and input objects\n");
			exit(1);
		}
		fclose(outf);
		if (strncmp(T.name, "amd64", 5) == 0) {
			is64 = 1;
			mach = 62;
		} else if (strcmp(T.name, "i386") == 0) {
			is64 = 0;
			mach = 3;
		} else if (strcmp(T.name, "arm64") == 0) {
			is64 = 1;
			mach = 183;
		} else if (strcmp(T.name, "rv64") == 0) {
			is64 = 1;
			mach = 243;
		} else if (strcmp(T.name, "arm") == 0) {
			is64 = 0;
			mach = 40;
		} else {
			fprintf(stderr, "luxir: no built-in linker for %s\n", T.name);
			exit(1);
		}
		exit(link(av + optind, ac - optind, out, is64, mach, pic, entry));
	}

	if (asmode && !pipe) {
		/* vestavěný assembler: vstup je .s pro architekturu cíle */
		AArch *arch;
		if (!out || exe || dbg || !av[optind]) {
			fprintf(stderr, "luxir: -a needs -o file and input files\n");
			exit(1);
		}
		fclose(outf);
		if (strncmp(T.name, "amd64", 5) == 0)
			arch = &AX86_64;
		else if (strcmp(T.name, "i386") == 0)
			arch = &AI386;
		else if (strcmp(T.name, "arm64") == 0)
			arch = &AARM64;
		else if (strcmp(T.name, "rv64") == 0)
			arch = &ARV64;
		else if (strcmp(T.name, "arm") == 0)
			arch = &AARM;
		else {
			fprintf(stderr, "luxir: no built-in assembler for %s\n", T.name);
			exit(1);
		}
		exit(assemble(av + optind, ac - optind, out, arch));
	}

	if (pipe) {
		if (!out || exe || dbg || !av[optind]) {
			fprintf(stderr, "luxir: -a -l needs -o file, a .lir input and objects\n");
			exit(1);
		}
		fclose(outf);
		luxir_unlink(out);
		snprintf(asmpath, sizeof asmpath, "%s.s", out);
		snprintf(objpath, sizeof objpath, "%s.o", out);
		outf = fopen(asmpath, "w");
		if (!outf) {
			fprintf(stderr, "cannot open '%s'\n", asmpath);
			exit(1);
		}
		tmpasm = asmpath;
		/* objekty pro linker: vstupy bez přípony .lir */
		objs = emalloc((ac - optind + 2) * sizeof objs[0]);
		nobj = 0;
		objs[nobj++] = objpath;
		for (k=optind; k<ac; k++) {
			size_t n = strlen(av[k]);
			if (n < 4 || strcmp(av[k] + n - 4, ".lir") != 0)
				objs[nobj++] = av[k];
		}
	}
	if (exe) {
		/* Assembler jde do OUT.s; OUT zůstává až pro linker. */
		if (!out || dbg) {
			fprintf(stderr, "luxir: -x needs -o file and no -d\n");
			exit(1);
		}
		fclose(outf);
		luxir_unlink(out);
		snprintf(asmpath, sizeof asmpath, "%s.s", out);
		outf = fopen(asmpath, "w");
		if (!outf) {
			fprintf(stderr, "cannot open '%s'\n", asmpath);
			exit(1);
		}
		tmpasm = asmpath;
	}

	do {
		f = av[optind];
		if (pipe && f) {
			size_t n = strlen(f);
			if (n < 4 || strcmp(f + n - 4, ".lir") != 0)
				continue;
		}
		if (!f || strcmp(f, "-") == 0) {
			inf = stdin;
			f = "-";
		} else {
			inf = fopen(f, "r");
			if (!inf) {
				fprintf(stderr, "cannot open '%s'\n", f);
				exit(1);
			}
		}
		parse(inf, f, dbgfile, data, func);
		fclose(inf);
	} while (++optind < ac);

	if (!dbg)
		T.emitfin(outf);

	if (exe) {
		/* Jméno dočasného souboru se nesmí dostat do binárky. */
		fputs("\n.file \"luxir\"\n", outf);
		fclose(outf);
		tmpasm = 0;
		exit(linkexe(out, asmpath, rtdir));
	}
	if (pipe) {
		AArch *arch;
		int is64, mach, rc;
		fputs("\n.file \"luxir\"\n", outf);
		fclose(outf);
		if (strncmp(T.name, "amd64", 5) == 0) {
			arch = &AX86_64;
			is64 = 1;
			mach = 62;
		} else if (strcmp(T.name, "i386") == 0) {
			arch = &AI386;
			is64 = 0;
			mach = 3;
		} else if (strcmp(T.name, "arm64") == 0) {
			arch = &AARM64;
			is64 = 1;
			mach = 183;
		} else if (strcmp(T.name, "rv64") == 0) {
			arch = &ARV64;
			is64 = 1;
			mach = 243;
		} else if (strcmp(T.name, "arm") == 0) {
			arch = &AARM;
			is64 = 0;
			mach = 40;
		} else {
			fprintf(stderr, "luxir: no built-in assembler and linker for %s\n", T.name);
			exit(1);
		}
		rc = assemble(&tmpasm, 1, objpath, arch);
		luxir_unlink(asmpath);
		tmpasm = 0;
		if (rc == 0)
			rc = link(objs, nobj, out, is64, mach, pic, entry);
		luxir_unlink(objpath);
		exit(rc);
	}
	exit(0);
}

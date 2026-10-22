#include "../all.h"
#include <stdarg.h>

/* Vestavěný linker (ld.c): statické spustitelné soubory ELF z objektů. */
int link(char **inputs, int ninputs, char *out, int is64, int mach, int ispie, int isbare, char *entry);
/* pe.c: program PE32+ pro Windows z objektů COFF a souborů .def */
int linkpe(char **inputs, int ninputs, char *out, int mach, char *entry);
/* macho.c: spustitelný Mach-O pro macOS z objektů a stubu knihovny .tbd */
int linkmacho(char **inputs, int ninputs, char *out, int arm, char *entry);
/* -M: mapa rozvržení pro testy (Mach-O) */
extern char *ldmap;

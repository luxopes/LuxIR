#include "../all.h"
#include <stdarg.h>

/* Vestavěný linker (ld.c): statické spustitelné soubory ELF z objektů. */
int link(char **inputs, int ninputs, char *out, int is64, int mach, int ispie, int isbare, char *entry);
/* pe.c: program PE32+ pro Windows z objektů COFF a souborů .def */
int linkpe(char **inputs, int ninputs, char *out, int mach, char *entry);

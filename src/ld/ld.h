#include "../all.h"
#include <stdarg.h>

/* Vestavěný linker (ld.c): statické spustitelné soubory ELF z objektů. */
int link(char **inputs, int ninputs, char *out, int is64, int mach, int ispie, char *entry);

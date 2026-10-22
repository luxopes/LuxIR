#ifndef LUXIR_VERSION_H
#define LUXIR_VERSION_H

/* Protocol revisions, independent of the upstream QBE release number. */
#define LUXIR_RELEASE_VERSION "0.4.0"
#define LUXIR_IR_VERSION "1"
#define LUXIR_RUNTIME_ABI "none"
/* Runtime LSL dodávaný s LuxIR (runtime/lsl/, doc/lsl-runtime.md). Verze 0:
 * rozhraní převzaté z LSL beze změny, ještě není stabilní. */
#define LUXIR_RUNTIME_ABI_LSL "lsl-0"
#define LUXIR_RUNTIME_ABIS LUXIR_RUNTIME_ABI ", " LUXIR_RUNTIME_ABI_LSL
#define LUXIR_VERSION_TEXT "LuxIR " LUXIR_RELEASE_VERSION " (IR " LUXIR_IR_VERSION "; runtime ABI " LUXIR_RUNTIME_ABIS ")"

#endif

#ifndef LUXIR_VERSION_H
#define LUXIR_VERSION_H

/* Protocol revisions, independent of the upstream QBE release number. */
#define LUXIR_RELEASE_VERSION "0.1.2-dev"
#define LUXIR_IR_VERSION "1"
#define LUXIR_RUNTIME_ABI "none"
#define LUXIR_RUNTIME_ABI_LIST "none, 1"
#define LUXIR_RUNTIME_ABI_SYMBOL "luxrt_abi_v1_le64_explicit"
#define LUXIR_VERSION_TEXT "LuxIR " LUXIR_RELEASE_VERSION " (IR " LUXIR_IR_VERSION "; runtime ABI " LUXIR_RUNTIME_ABI_LIST ")"

#endif

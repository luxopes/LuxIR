#include "abi.h"
#include "managed.h"
const uint64_t luxrt_abi_v1_le64_explicit[4] = {UINT64_C(0x31545258554c), 1, 64, 1};
uint64_t luxrt_abi_revision(void) { return 1; }
uint64_t luxrt_heap_bytes(void) { return sizeof(luxrt_heap); }
uint64_t luxrt_roots_bytes(void) { return sizeof(luxrt_roots); }
uint64_t luxrt_descriptor_bytes(void) { return sizeof(luxrt_descriptor); }

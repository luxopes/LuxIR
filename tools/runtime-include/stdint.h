#ifndef LUXRT_CODEGEN_STDINT_H
#define LUXRT_CODEGEN_STDINT_H
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long int64_t;
typedef unsigned long uint64_t;
typedef unsigned long uintptr_t;
#define UINT64_C(x) x##UL
#define INT64_C(x) x##L
#define UINT64_MAX 18446744073709551615UL
#define INT64_MAX 9223372036854775807L
#define INT64_MIN (-9223372036854775807L-1)
#endif

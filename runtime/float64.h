#ifndef LUXRT_FLOAT64_H
#define LUXRT_FLOAT64_H
#include <stdint.h>
/* Locale-free Float64 bits <-> shortest text. Grammar: optional +/-, digits
 * with at most one dot, optional e/E exponent with optional sign. No spaces,
 * separators or hex; lower-case inf/infinity/nan accepted. Overflow -> inf,
 * underflow -> signed zero, round-to-nearest ties-even independent of fenv.
 * Parse: 0 / -22 syntax / -34 unreasonable span. Output unchanged on error.
 * Format: bytes written / -28 capacity; no NUL; output unchanged on error.
 * Format follows Python repr thresholds; NaN always 'nan', signed zero kept.
 */
int32_t luxrt_parse_float64(const void *text, uint64_t size, uint64_t *bits);
int64_t luxrt_format_float64(uint64_t bits, void *buffer, uint64_t capacity);
#endif

/* Pomocné funkce překladače C pro 32bitové cíle (c32), bez libgcc a
 * compiler-rt: 64bitové dělení, převody mezi int64 a float/double, paměťové
 * funkce a varianty ARM EABI. Psané jen s operacemi, které clang a gcc na
 * 32bitových cílech generují přímo (64bitové posuny, sčítání a porovnání),
 * aby se samy nevolaly. Převody desetinných čísel mimo rozsah jsou nasycené
 * (jako lux_dtos* v kódu z LuxIR). */
#include <stdint.h>
#include <stddef.h>

#define LUX_RT __attribute__((used))

LUX_RT uint64_t
__udivmoddi4(uint64_t a, uint64_t b, uint64_t *rem)
{
	uint64_t q, r;
	int n;

	if (b == 0) {
		if (rem)
			*rem = a;
		return UINT64_MAX;
	}
	if ((b >> 32) == 0 && (a >> 32) == 0) {
		uint32_t x = (uint32_t)a, y = (uint32_t)b;
		if (rem)
			*rem = x % y;
		return x / y;
	}
	q = 0;
	r = 0;
	for (n=63; n>=0; n--) {
		r = (r << 1) | ((a >> n) & 1);
		if (r >= b) {
			r -= b;
			q |= (uint64_t)1 << n;
		}
	}
	if (rem)
		*rem = r;
	return q;
}

LUX_RT uint64_t __udivdi3(uint64_t a, uint64_t b) { return __udivmoddi4(a, b, 0); }
LUX_RT uint64_t __umoddi3(uint64_t a, uint64_t b) { uint64_t r; __udivmoddi4(a, b, &r); return r; }

LUX_RT int64_t
__divmoddi4(int64_t a, int64_t b, int64_t *rem)
{
	uint64_t ua, ub, q, r;

	ua = a < 0 ? -(uint64_t)a : (uint64_t)a;
	ub = b < 0 ? -(uint64_t)b : (uint64_t)b;
	q = __udivmoddi4(ua, ub, &r);
	if ((a < 0) != (b < 0))
		q = -q;
	if (a < 0)
		r = -r;
	if (rem)
		*rem = (int64_t)r;
	return (int64_t)q;
}

LUX_RT int64_t __divdi3(int64_t a, int64_t b) { return __divmoddi4(a, b, 0); }
LUX_RT int64_t __moddi3(int64_t a, int64_t b) { int64_t r; __divmoddi4(a, b, &r); return r; }

/* int64 -> double: horní a dolní půlka jsou přesné, sčítání zaokrouhlí
 * jednou (správné zaokrouhlení). */
LUX_RT double
__floatundidf(uint64_t a)
{
	return (double)(uint32_t)(a >> 32) * 4294967296.0 + (double)(uint32_t)a;
}

LUX_RT double
__floatdidf(int64_t a)
{
	return a < 0 ? -__floatundidf(-(uint64_t)a) : __floatundidf((uint64_t)a);
}

/* float přes double by zaokrouhlil dvakrát; 64 bitů se nejdřív zkrátí na
 * 32 platných se zachovaným lepivým bitem. */
LUX_RT float
__floatundisf(uint64_t a)
{
	int shift;

	if ((a >> 32) == 0)
		return (float)(uint32_t)a;
	shift = 0;
	while ((a >> shift) >= ((uint64_t)1 << 32))
		shift++;
	{
		uint64_t kept = a >> shift;
		if (a & (((uint64_t)1 << shift) - 1))
			kept |= 1;
		return (float)(uint32_t)kept * (float)((uint64_t)1 << shift);
	}
}

LUX_RT float
__floatdisf(int64_t a)
{
	return a < 0 ? -__floatundisf(-(uint64_t)a) : __floatundisf((uint64_t)a);
}

LUX_RT uint64_t
__fixunsdfdi(double v)
{
	uint32_t hi;

	if (!(v > 0))
		return 0;
	if (v >= 18446744073709551616.0)
		return UINT64_MAX;
	if (v < 4294967296.0)
		return (uint32_t)v;
	hi = (uint32_t)(v / 4294967296.0);
	return ((uint64_t)hi << 32) | (uint32_t)(v - (double)hi * 4294967296.0);
}

LUX_RT int64_t
__fixdfdi(double v)
{
	if (v != v)
		return 0;
	if (v <= -9223372036854775808.0)
		return INT64_MIN;
	if (v >= 9223372036854775808.0)
		return INT64_MAX;
	return v < 0 ? -(int64_t)__fixunsdfdi(-v) : (int64_t)__fixunsdfdi(v);
}

LUX_RT uint64_t __fixunssfdi(float v) { return __fixunsdfdi(v); }
LUX_RT int64_t __fixsfdi(float v) { return __fixdfdi(v); }

LUX_RT void *
memcpy(void *d, const void *s, size_t n)
{
	unsigned char *x = d;
	const unsigned char *y = s;

	while (n--)
		*x++ = *y++;
	return d;
}

LUX_RT void *
memmove(void *d, const void *s, size_t n)
{
	unsigned char *x = d;
	const unsigned char *y = s;

	if (x < y)
		while (n--)
			*x++ = *y++;
	else
		while (n--)
			x[n] = y[n];
	return d;
}

LUX_RT void *
memset(void *d, int c, size_t n)
{
	unsigned char *x = d;

	while (n--)
		*x++ = (unsigned char)c;
	return d;
}

LUX_RT int
memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x < *y ? -1 : 1;
	return 0;
}

#if defined(__arm__)
/* ARM EABI: převody a 64bitové dělení s výsledkem v r0-r3. Pomocné funkce
 * __aeabi_* předávají float a double vždy podle základního AAPCS (v r0-r3),
 * i na cíli s hard-float ABI. */
#define LUX_AAPCS __attribute__((used, pcs("aapcs")))
LUX_AAPCS double __aeabi_l2d(int64_t a) { return __floatdidf(a); }
LUX_AAPCS double __aeabi_ul2d(uint64_t a) { return __floatundidf(a); }
LUX_AAPCS float __aeabi_l2f(int64_t a) { return __floatdisf(a); }
LUX_AAPCS float __aeabi_ul2f(uint64_t a) { return __floatundisf(a); }
LUX_AAPCS int64_t __aeabi_d2lz(double v) { return __fixdfdi(v); }
LUX_AAPCS uint64_t __aeabi_d2ulz(double v) { return __fixunsdfdi(v); }
LUX_AAPCS int64_t __aeabi_f2lz(float v) { return __fixdfdi(v); }
LUX_AAPCS uint64_t __aeabi_f2ulz(float v) { return __fixunsdfdi(v); }
LUX_RT void *__aeabi_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
LUX_RT void *__aeabi_memcpy4(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
LUX_RT void *__aeabi_memcpy8(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
LUX_RT void *__aeabi_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
LUX_RT void __aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }
LUX_RT void __aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
LUX_RT void __aeabi_memclr4(void *d, size_t n) { memset(d, 0, n); }
LUX_RT void __aeabi_memclr8(void *d, size_t n) { memset(d, 0, n); }

LUX_RT unsigned
__aeabi_uidiv(unsigned a, unsigned b)
{
	unsigned q = 0, r = 0;
	int n;

	if (!b)
		return 0xffffffffu;
	for (n=31; n>=0; n--) {
		r = (r << 1) | ((a >> n) & 1);
		if (r >= b) {
			r -= b;
			q |= 1u << n;
		}
	}
	return q;
}

LUX_RT int
__aeabi_idiv(int a, int b)
{
	unsigned ua = a < 0 ? -(unsigned)a : (unsigned)a;
	unsigned ub = b < 0 ? -(unsigned)b : (unsigned)b;
	unsigned q = __aeabi_uidiv(ua, ub);
	return (a < 0) != (b < 0) ? -(int)q : (int)q;
}

/* uidivmod a idivmod vrací podíl v r0 a zbytek v r1 */
__asm__(
	".text\n.globl __aeabi_uidivmod\n.type __aeabi_uidivmod,%function\n"
	"__aeabi_uidivmod:\n push {r0, r1, r4, lr}\n bl __aeabi_uidiv\n pop {r1, r2}\n"
	" mul r3, r0, r2\n sub r1, r1, r3\n pop {r4, pc}\n"
	".globl __aeabi_idivmod\n.type __aeabi_idivmod,%function\n"
	"__aeabi_idivmod:\n push {r0, r1, r4, lr}\n bl __aeabi_idiv\n pop {r1, r2}\n"
	" mul r3, r0, r2\n sub r1, r1, r3\n pop {r4, pc}\n"
	/* uldivmod/ldivmod: (r0:r1) / (r2:r3), podíl v r0:r1, zbytek v r2:r3 */
	".globl __aeabi_uldivmod\n.type __aeabi_uldivmod,%function\n"
	"__aeabi_uldivmod:\n push {r4, lr}\n sub sp, sp, #16\n add r4, sp, #8\n str r4, [sp]\n"
	" bl __udivmoddi4\n ldr r2, [sp, #8]\n ldr r3, [sp, #12]\n add sp, sp, #16\n pop {r4, pc}\n"
	".globl __aeabi_ldivmod\n.type __aeabi_ldivmod,%function\n"
	"__aeabi_ldivmod:\n push {r4, lr}\n sub sp, sp, #16\n add r4, sp, #8\n str r4, [sp]\n"
	" bl __divmoddi4\n ldr r2, [sp, #8]\n ldr r3, [sp, #12]\n add sp, sp, #16\n pop {r4, pc}\n");
#endif

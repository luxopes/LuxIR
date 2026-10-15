/* Samostatný runtime testů c32 v Linuxu (qemu-user): _start, exit, write a
 * printf s formáty, které používají tests/run (%d %i %u %x %ld %lld %llu
 * %llx %s %c %f s přesností). Bez libc.
 *
 * Konvence c32: ukazatele z LuxIR jsou 64bitová slova (třída Kl), proto
 * printf bere formát i argumenty %s jako uint64_t. */
#include <stdarg.h>
#include <stdint.h>

static long
sys3(long n, long a, long b, long c)
{
#if defined(__riscv)
	register long a7 __asm__("a7") = n;
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	register long a2 __asm__("a2") = c;
	__asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
	return a0;
#elif defined(__arm__)
	register long r7 __asm__("r7") = n;
	register long r0 __asm__("r0") = a;
	register long r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c;
	__asm__ volatile("svc 0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory");
	return r0;
#elif defined(__i386__)
	long r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
	return r;
#else
#error unsupported architecture
#endif
}

#if defined(__riscv)
enum { SYS_write = 64, SYS_exit = 94 };
#elif defined(__arm__)
enum { SYS_write = 4, SYS_exit = 248 };
#else
enum { SYS_write = 4, SYS_exit = 252 };
#endif

static char obuf[4096];
static int olen;

static void
flush(void)
{
	int done = 0;
	while (done < olen) {
		long n = sys3(SYS_write, 1, (long)(obuf + done), olen - done);
		if (n <= 0)
			break;
		done += n;
	}
	olen = 0;
}

static void
put(char c)
{
	if (olen == (int)sizeof obuf)
		flush();
	obuf[olen++] = c;
}

static void
putu(uint64_t v, int base, int width)
{
	char d[24];
	int n = 0;

	do {
		d[n++] = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);
	while (n < width)
		d[n++] = '0';
	while (n)
		put(d[--n]);
}

static void
putf(double v, int prec)
{
	uint64_t whole, frac, scale;
	int n;

	if (v != v) {
		put('n'); put('a'); put('n');
		return;
	}
	if (v < 0) {
		put('-');
		v = -v;
	}
	if (v > 1.8e19) {
		put('i'); put('n'); put('f');
		return;
	}
	scale = 1;
	for (n=0; n<prec; n++)
		scale *= 10;
	whole = (uint64_t)v;
	frac = (uint64_t)((v - (double)whole) * (double)scale + 0.5);
	if (frac >= scale) {
		whole++;
		frac -= scale;
	}
	putu(whole, 10, 1);
	if (prec) {
		put('.');
		putu(frac, 10, prec);
	}
}

int
printf(uint64_t format, ...)
{
	const char *f = (const char *)(uintptr_t)format;
	va_list ap;
	int longs, prec;
	int64_t s;
	uint64_t u;

	va_start(ap, format);
	for (; *f; f++) {
		if (*f != '%') {
			put(*f);
			continue;
		}
		f++;
		prec = 6;
		if (*f == '.') {
			prec = 0;
			for (f++; *f >= '0' && *f <= '9'; f++)
				prec = prec * 10 + (*f - '0');
		}
		longs = 0;
		while (*f == 'l') {
			longs++;
			f++;
		}
		switch (*f) {
		case 'd':
		case 'i':
			s = longs >= 2 ? va_arg(ap, int64_t) : longs ? va_arg(ap, long) : va_arg(ap, int);
			if (s < 0) {
				put('-');
				putu(-(uint64_t)s, 10, 1);
			} else
				putu((uint64_t)s, 10, 1);
			break;
		case 'u':
		case 'x':
			u = longs >= 2 ? va_arg(ap, uint64_t) : longs ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
			putu(u, *f == 'x' ? 16 : 10, 1);
			break;
		case 'f':
			putf(va_arg(ap, double), prec);
			break;
		case 's':
			for (const char *p = (const char *)(uintptr_t)va_arg(ap, uint64_t); *p; p++)
				put(*p);
			break;
		case 'c':
			put((char)va_arg(ap, int));
			break;
		default:
			put(*f);
			break;
		}
	}
	va_end(ap);
	return 0;
}

int puts(uint64_t text)
{
	const char *s = (const char *)(uintptr_t)text;

	for (; *s; s++)
		put(*s);
	put('\n');
	return 0;
}

int main(int, char **);

__attribute__((used, noreturn)) void
lux_start(long *sp)
{
	int status = main((int)sp[0], (char **)(sp + 1));
	flush();
	sys3(SYS_exit, status, 0, 0);
	for (;;)
		;
}

#if defined(__riscv)
__asm__(".text\n.globl _start\n_start:\n mv a0, sp\n andi sp, sp, -16\n call lux_start\n");
#elif defined(__arm__)
__asm__(".text\n.globl _start\n_start:\n mov r0, sp\n bic sp, sp, #15\n bl lux_start\n");
#else
__asm__(".text\n.globl _start\n_start:\n xor %ebp, %ebp\n mov %esp, %eax\n and $-16, %esp\n sub $12, %esp\n push %eax\n call lux_start\n");
#endif

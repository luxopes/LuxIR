# Vstup dynamicky linkovaného testu (tests/dyn_check.py): ld.so předá
# původní zásobník jádra; main(argc, argv) z programu LuxIR, konec přes
# exit z glibc (vyprázdní stdio).
	.text
	.globl _start
_start:
	xor %ebp, %ebp
	mov (%rsp), %rdi
	lea 8(%rsp), %rsi
	and $-16, %rsp
	call main
	mov %eax, %edi
	call exit
	ud2

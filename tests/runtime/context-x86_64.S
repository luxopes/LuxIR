.text
.globl context_fixture
.type context_fixture,@function
context_fixture:
 push %r12
 push %r13
 push %r14
 mov %rdi,%r13
 mov %rsi,%r14
 mov $4096,%edi
 call luxrt_os_map
 mov %rax,%r12
 movq $42,(%r12)
 movabs $0x5a5a5a5a5a5a5a5a,%rax
 xor %r12,%rax
 mov %rax,(%r14)
 xor %eax,%eax
 mov %r13,%rdi
 call luxrt_capture_regs
 pop %r14
 pop %r13
 pop %r12
 ret
.size context_fixture,.-context_fixture
.section .note.GNU-stack,"",@progbits

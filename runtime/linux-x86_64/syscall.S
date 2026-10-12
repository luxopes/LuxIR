.text
.globl luxrt_syscall6
.type luxrt_syscall6,@function
luxrt_syscall6:
    endbr64
    mov %rdi,%rax
    mov %rsi,%rdi
    mov %rdx,%rsi
    mov %rcx,%rdx
    mov %r8,%r10
    mov %r9,%r8
    mov 8(%rsp),%r9
    syscall
    ret
.size luxrt_syscall6,.-luxrt_syscall6
.section .note.GNU-stack,"",@progbits

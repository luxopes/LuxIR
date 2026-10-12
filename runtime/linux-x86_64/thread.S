.text
.globl luxrt_thread_clone
.type luxrt_thread_clone,@function
# SysV fn=rdi, arg=rsi, aligned stack end=rdx, shared tid slot=rcx.
luxrt_thread_clone:
    endbr64
    sub $16,%rdx
    mov %rdi,(%rdx)
    mov %rsi,8(%rdx)
    mov %rdx,%rsi
    mov %rcx,%rdx
    mov %rcx,%r10
    xor %r8d,%r8d
    mov $0x350f00,%edi
    mov $56,%eax
    syscall
    test %rax,%rax
    jz .Lthread_child
    ret
.Lthread_child:
    pop %rax
    pop %rdi
    call *%rax
    xor %edi,%edi
    mov $60,%eax
    syscall
    ud2
.size luxrt_thread_clone,.-luxrt_thread_clone
.section .note.GNU-stack,"",@progbits

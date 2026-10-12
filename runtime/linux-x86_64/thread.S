.text
.globl luxrt_thread_clone
.type luxrt_thread_clone,@function
luxrt_thread_clone:
    endbr64
    xor %r8d,%r8d
    mov $0x350f00,%r9d
    jmp .Lthread_clone
.size luxrt_thread_clone,.-luxrt_thread_clone
.globl luxrt_thread_clone_tls
.type luxrt_thread_clone_tls,@function
luxrt_thread_clone_tls:
    endbr64
    mov $0x3d0f00,%r9d
.Lthread_clone:
    sub $16,%rdx
    mov %rdi,(%rdx)
    mov %rsi,8(%rdx)
    mov %rdx,%rsi
    mov %rcx,%rdx
    mov %rcx,%r10
    mov %r9,%rdi
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
.size luxrt_thread_clone_tls,.-luxrt_thread_clone_tls
.section .note.GNU-stack,"",@progbits

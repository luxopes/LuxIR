.text
.globl luxrt_tls_set
.type luxrt_tls_set,@function
luxrt_tls_set:
    endbr64
    mov %rdi,%rsi
    mov $4098,%edi
    mov $158,%eax
    syscall
    ret
.size luxrt_tls_set,.-luxrt_tls_set
.globl luxrt_tls_get
.type luxrt_tls_get,@function
luxrt_tls_get:
    endbr64
    sub $16,%rsp
    mov %rsp,%rsi
    mov $4099,%edi
    mov $158,%eax
    syscall
    test %rax,%rax
    jnz .Ltls_get_error
    mov (%rsp),%rax
.Ltls_get_error:
    add $16,%rsp
    ret
.size luxrt_tls_get,.-luxrt_tls_get
.section .note.GNU-stack,"",@progbits

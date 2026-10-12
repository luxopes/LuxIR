.text
.globl luxrt_atomic_load32
.type luxrt_atomic_load32,@function
luxrt_atomic_load32:
    endbr64
    mov (%rdi),%eax
    ret
.size luxrt_atomic_load32,.-luxrt_atomic_load32
.globl luxrt_atomic_store32
.type luxrt_atomic_store32,@function
luxrt_atomic_store32:
    endbr64
    xchg %esi,(%rdi)
    ret
.size luxrt_atomic_store32,.-luxrt_atomic_store32
.globl luxrt_atomic_add32
.type luxrt_atomic_add32,@function
luxrt_atomic_add32:
    endbr64
    mov %esi,%eax
    lock xadd %eax,(%rdi)
    ret
.size luxrt_atomic_add32,.-luxrt_atomic_add32
.globl luxrt_atomic_swap32
.type luxrt_atomic_swap32,@function
luxrt_atomic_swap32:
    endbr64
    mov %esi,%eax
    xchg %eax,(%rdi)
    ret
.size luxrt_atomic_swap32,.-luxrt_atomic_swap32
.globl luxrt_atomic_cas32
.type luxrt_atomic_cas32,@function
luxrt_atomic_cas32:
    endbr64
    mov %esi,%eax
    lock cmpxchg %edx,(%rdi)
    ret
.size luxrt_atomic_cas32,.-luxrt_atomic_cas32
.globl luxrt_atomic_load64
.type luxrt_atomic_load64,@function
luxrt_atomic_load64:
    endbr64
    mov (%rdi),%rax
    ret
.size luxrt_atomic_load64,.-luxrt_atomic_load64
.globl luxrt_atomic_store64
.type luxrt_atomic_store64,@function
luxrt_atomic_store64:
    endbr64
    xchg %rsi,(%rdi)
    ret
.size luxrt_atomic_store64,.-luxrt_atomic_store64
.globl luxrt_atomic_add64
.type luxrt_atomic_add64,@function
luxrt_atomic_add64:
    endbr64
    mov %rsi,%rax
    lock xadd %rax,(%rdi)
    ret
.size luxrt_atomic_add64,.-luxrt_atomic_add64
.globl luxrt_atomic_swap64
.type luxrt_atomic_swap64,@function
luxrt_atomic_swap64:
    endbr64
    mov %rsi,%rax
    xchg %rax,(%rdi)
    ret
.size luxrt_atomic_swap64,.-luxrt_atomic_swap64
.globl luxrt_atomic_cas64
.type luxrt_atomic_cas64,@function
luxrt_atomic_cas64:
    endbr64
    mov %rsi,%rax
    lock cmpxchg %rdx,(%rdi)
    ret
.size luxrt_atomic_cas64,.-luxrt_atomic_cas64
.globl luxrt_atomic_fence
.type luxrt_atomic_fence,@function
luxrt_atomic_fence:
    endbr64
    mfence
    ret
.size luxrt_atomic_fence,.-luxrt_atomic_fence
.section .note.GNU-stack,"",@progbits

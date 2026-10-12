.text
.globl luxrt_atomic_load32
.type luxrt_atomic_load32,%function
luxrt_atomic_load32:
    dmb ish
    ldar w0,[x0]
    dmb ish
    ret
.size luxrt_atomic_load32,.-luxrt_atomic_load32
.globl luxrt_atomic_store32
.type luxrt_atomic_store32,%function
luxrt_atomic_store32:
    dmb ish
    stlr w1,[x0]
    dmb ish
    ret
.size luxrt_atomic_store32,.-luxrt_atomic_store32
.globl luxrt_atomic_add32
.type luxrt_atomic_add32,%function
luxrt_atomic_add32:
    dmb ish
    mov x3,x0
.Ladd32_retry:
    ldaxr w0,[x3]
    add w4,w0,w1
    stlxr w5,w4,[x3]
    cbnz w5,.Ladd32_retry
    dmb ish
    ret
.size luxrt_atomic_add32,.-luxrt_atomic_add32
.globl luxrt_atomic_swap32
.type luxrt_atomic_swap32,%function
luxrt_atomic_swap32:
    dmb ish
    mov x3,x0
.Lswap32_retry:
    ldaxr w0,[x3]
    mov w4,w1
    stlxr w5,w4,[x3]
    cbnz w5,.Lswap32_retry
    dmb ish
    ret
.size luxrt_atomic_swap32,.-luxrt_atomic_swap32
.globl luxrt_atomic_cas32
.type luxrt_atomic_cas32,%function
luxrt_atomic_cas32:
    dmb ish
    mov x3,x0
.Lcas32_retry:
    ldaxr w0,[x3]
    cmp w0,w1
    b.ne .Lcas32_fail
    mov w4,w2
    stlxr w5,w4,[x3]
    cbnz w5,.Lcas32_retry
    dmb ish
    ret
.Lcas32_fail:
    clrex
    dmb ish
    ret
.size luxrt_atomic_cas32,.-luxrt_atomic_cas32
.globl luxrt_atomic_load64
.type luxrt_atomic_load64,%function
luxrt_atomic_load64:
    dmb ish
    ldar x0,[x0]
    dmb ish
    ret
.size luxrt_atomic_load64,.-luxrt_atomic_load64
.globl luxrt_atomic_store64
.type luxrt_atomic_store64,%function
luxrt_atomic_store64:
    dmb ish
    stlr x1,[x0]
    dmb ish
    ret
.size luxrt_atomic_store64,.-luxrt_atomic_store64
.globl luxrt_atomic_add64
.type luxrt_atomic_add64,%function
luxrt_atomic_add64:
    dmb ish
    mov x3,x0
.Ladd64_retry:
    ldaxr x0,[x3]
    add x4,x0,x1
    stlxr w5,x4,[x3]
    cbnz w5,.Ladd64_retry
    dmb ish
    ret
.size luxrt_atomic_add64,.-luxrt_atomic_add64
.globl luxrt_atomic_swap64
.type luxrt_atomic_swap64,%function
luxrt_atomic_swap64:
    dmb ish
    mov x3,x0
.Lswap64_retry:
    ldaxr x0,[x3]
    mov x4,x1
    stlxr w5,x4,[x3]
    cbnz w5,.Lswap64_retry
    dmb ish
    ret
.size luxrt_atomic_swap64,.-luxrt_atomic_swap64
.globl luxrt_atomic_cas64
.type luxrt_atomic_cas64,%function
luxrt_atomic_cas64:
    dmb ish
    mov x3,x0
.Lcas64_retry:
    ldaxr x0,[x3]
    cmp x0,x1
    b.ne .Lcas64_fail
    mov x4,x2
    stlxr w5,x4,[x3]
    cbnz w5,.Lcas64_retry
    dmb ish
    ret
.Lcas64_fail:
    clrex
    dmb ish
    ret
.size luxrt_atomic_cas64,.-luxrt_atomic_cas64
.globl luxrt_atomic_fence
.type luxrt_atomic_fence,%function
luxrt_atomic_fence:
    dmb ish
    ret
.size luxrt_atomic_fence,.-luxrt_atomic_fence
.section .note.GNU-stack,"",%progbits

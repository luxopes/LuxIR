.text
.globl luxrt_atomic_load32
.type luxrt_atomic_load32,@function
luxrt_atomic_load32:
    fence rw,rw
    lw a0,0(a0)
    fence rw,rw
    ret
.size luxrt_atomic_load32,.-luxrt_atomic_load32
.globl luxrt_atomic_store32
.type luxrt_atomic_store32,@function
luxrt_atomic_store32:
    fence rw,rw
    sw a1,0(a0)
    fence rw,rw
    ret
.size luxrt_atomic_store32,.-luxrt_atomic_store32
.globl luxrt_atomic_add32
.type luxrt_atomic_add32,@function
luxrt_atomic_add32:
    amoadd.w.aqrl a0,a1,(a0)
    ret
.size luxrt_atomic_add32,.-luxrt_atomic_add32
.globl luxrt_atomic_swap32
.type luxrt_atomic_swap32,@function
luxrt_atomic_swap32:
    amoswap.w.aqrl a0,a1,(a0)
    ret
.size luxrt_atomic_swap32,.-luxrt_atomic_swap32
.globl luxrt_atomic_cas32
.type luxrt_atomic_cas32,@function
luxrt_atomic_cas32:
    mv t0,a0
    sext.w a1,a1
.Lcas32_retry:
    lr.w.aqrl a0,(t0)
    bne a0,a1,.Lcas32_done
    sc.w.aqrl t1,a2,(t0)
    bnez t1,.Lcas32_retry
.Lcas32_done:
    ret
.size luxrt_atomic_cas32,.-luxrt_atomic_cas32
.globl luxrt_atomic_load64
.type luxrt_atomic_load64,@function
luxrt_atomic_load64:
    fence rw,rw
    ld a0,0(a0)
    fence rw,rw
    ret
.size luxrt_atomic_load64,.-luxrt_atomic_load64
.globl luxrt_atomic_store64
.type luxrt_atomic_store64,@function
luxrt_atomic_store64:
    fence rw,rw
    sd a1,0(a0)
    fence rw,rw
    ret
.size luxrt_atomic_store64,.-luxrt_atomic_store64
.globl luxrt_atomic_add64
.type luxrt_atomic_add64,@function
luxrt_atomic_add64:
    amoadd.d.aqrl a0,a1,(a0)
    ret
.size luxrt_atomic_add64,.-luxrt_atomic_add64
.globl luxrt_atomic_swap64
.type luxrt_atomic_swap64,@function
luxrt_atomic_swap64:
    amoswap.d.aqrl a0,a1,(a0)
    ret
.size luxrt_atomic_swap64,.-luxrt_atomic_swap64
.globl luxrt_atomic_cas64
.type luxrt_atomic_cas64,@function
luxrt_atomic_cas64:
    mv t0,a0
.Lcas64_retry:
    lr.d.aqrl a0,(t0)
    bne a0,a1,.Lcas64_done
    sc.d.aqrl t1,a2,(t0)
    bnez t1,.Lcas64_retry
.Lcas64_done:
    ret
.size luxrt_atomic_cas64,.-luxrt_atomic_cas64
.globl luxrt_atomic_fence
.type luxrt_atomic_fence,@function
luxrt_atomic_fence:
    fence rw,rw
    ret
.size luxrt_atomic_fence,.-luxrt_atomic_fence
.section .note.GNU-stack,"",@progbits

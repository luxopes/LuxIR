.text
.globl luxrt_tls_set
.type luxrt_tls_set,%function
luxrt_tls_set:
    msr tpidr_el0,x0
    isb
    mov x0,xzr
    ret
.size luxrt_tls_set,.-luxrt_tls_set
.globl luxrt_tls_get
.type luxrt_tls_get,%function
luxrt_tls_get:
    mrs x0,tpidr_el0
    ret
.size luxrt_tls_get,.-luxrt_tls_get
.section .note.GNU-stack,"",%progbits

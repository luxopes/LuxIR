.text
.globl luxrt_tls_set
.type luxrt_tls_set,@function
luxrt_tls_set:
    mv tp,a0
    li a0,0
    ret
.size luxrt_tls_set,.-luxrt_tls_set
.globl luxrt_tls_get
.type luxrt_tls_get,@function
luxrt_tls_get:
    mv a0,tp
    ret
.size luxrt_tls_get,.-luxrt_tls_get
.section .note.GNU-stack,"",@progbits

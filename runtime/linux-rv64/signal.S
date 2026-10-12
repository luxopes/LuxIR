.text
.globl luxrt_sigreturn
.type luxrt_sigreturn,@function
luxrt_sigreturn:
    li a7,139
    ecall
    unimp
.size luxrt_sigreturn,.-luxrt_sigreturn
.section .note.GNU-stack,"",@progbits

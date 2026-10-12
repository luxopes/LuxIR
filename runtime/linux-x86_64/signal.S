.text
.globl luxrt_sigreturn
.type luxrt_sigreturn,@function
luxrt_sigreturn:
    mov $15,%eax
    syscall
    ud2
.size luxrt_sigreturn,.-luxrt_sigreturn
.section .note.GNU-stack,"",@progbits

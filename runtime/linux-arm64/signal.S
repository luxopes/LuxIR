.text
.globl luxrt_sigreturn
.type luxrt_sigreturn,%function
luxrt_sigreturn:
    mov x8,#139
    svc #0
    brk #0
.size luxrt_sigreturn,.-luxrt_sigreturn
.section .note.GNU-stack,"",%progbits

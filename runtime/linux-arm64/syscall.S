.text
.globl luxrt_syscall6
.type luxrt_syscall6,%function
luxrt_syscall6:
    // AAPCS64: number=x0, six syscall arguments=x1..x6.
    mov x8,x0
    mov x0,x1
    mov x1,x2
    mov x2,x3
    mov x3,x4
    mov x4,x5
    mov x5,x6
    svc #0
    ret
.size luxrt_syscall6,.-luxrt_syscall6
.section .note.GNU-stack,"",%progbits

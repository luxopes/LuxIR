.text
.globl luxrt_syscall6
.type luxrt_syscall6,@function
luxrt_syscall6:
    # RV64 psABI: number=a0, six syscall arguments=a1..a6.
    mv a7,a0
    mv a0,a1
    mv a1,a2
    mv a2,a3
    mv a3,a4
    mv a4,a5
    mv a5,a6
    ecall
    ret
.size luxrt_syscall6,.-luxrt_syscall6
.section .note.GNU-stack,"",@progbits

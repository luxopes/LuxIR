.text
.globl luxrt_thread_clone
.type luxrt_thread_clone,@function
luxrt_thread_clone:
    li a4,0
    li a5,0x350f00
    j .Lclone
.size luxrt_thread_clone,.-luxrt_thread_clone
.globl luxrt_thread_clone_tls
.type luxrt_thread_clone_tls,@function
luxrt_thread_clone_tls:
    li a5,0x3d0f00
.Lclone:
    addi a2,a2,-16
    sd a0,0(a2)
    sd a1,8(a2)
    mv a0,a5
    mv a1,a2
    mv a2,a3
    mv a3,a4
    mv a4,a2
    li a7,220
    ecall
    beqz a0,.Lchild
    ret
.Lchild:
    ld t0,0(sp)
    ld a0,8(sp)
    addi sp,sp,16
    jalr t0
    li a0,0
    li a7,93
    ecall
    unimp
.size luxrt_thread_clone_tls,.-luxrt_thread_clone_tls
.section .note.GNU-stack,"",@progbits

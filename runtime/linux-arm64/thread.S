.text
.globl luxrt_thread_clone
.type luxrt_thread_clone,%function
luxrt_thread_clone:
    mov x4,xzr
    mov x5,#0xf00
    movk x5,#0x35,lsl #16
    b .Lclone
.size luxrt_thread_clone,.-luxrt_thread_clone
.globl luxrt_thread_clone_tls
.type luxrt_thread_clone_tls,%function
luxrt_thread_clone_tls:
    mov x5,#0xf00
    movk x5,#0x3d,lsl #16
.Lclone:
    sub x2,x2,#16
    stp x0,x1,[x2]
    mov x0,x5
    mov x1,x2
    mov x2,x3
    mov x3,x4
    mov x4,x2
    mov x8,#220
    svc #0
    cbz x0,.Lchild
    ret
.Lchild:
    ldp x16,x0,[sp],#16
    blr x16
    mov x0,xzr
    mov x8,#93
    svc #0
    brk #0
.size luxrt_thread_clone_tls,.-luxrt_thread_clone_tls
.section .note.GNU-stack,"",%progbits

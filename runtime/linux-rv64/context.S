.text
.globl luxrt_capture_regs
.type luxrt_capture_regs,@function
luxrt_capture_regs:
    sd s0,0(a0)
    sd s1,8(a0)
    sd s2,16(a0)
    sd s3,24(a0)
    sd s4,32(a0)
    sd s5,40(a0)
    sd s6,48(a0)
    sd s7,56(a0)
    sd s8,64(a0)
    sd s9,72(a0)
    sd s10,80(a0)
    sd s11,88(a0)
    fsd fs0,96(a0)
    fsd fs1,104(a0)
    fsd fs2,112(a0)
    fsd fs3,120(a0)
    fsd fs4,128(a0)
    fsd fs5,136(a0)
    fsd fs6,144(a0)
    fsd fs7,152(a0)
    fsd fs8,160(a0)
    fsd fs9,168(a0)
    fsd fs10,176(a0)
    fsd fs11,184(a0)
    sd sp,192(a0)
    mv a0,sp
    ret
.size luxrt_capture_regs,.-luxrt_capture_regs
.globl luxrt_context_words
.type luxrt_context_words,@function
luxrt_context_words:
    li a0,25
    ret
.size luxrt_context_words,.-luxrt_context_words
.globl luxrt_startup_stack_top
.type luxrt_startup_stack_top,@function
luxrt_startup_stack_top:
    ret
.size luxrt_startup_stack_top,.-luxrt_startup_stack_top
.section .note.GNU-stack,"",@progbits

.text
.globl luxrt_capture_regs
.type luxrt_capture_regs,%function
luxrt_capture_regs:
    str x19,[x0,#0]
    str x20,[x0,#8]
    str x21,[x0,#16]
    str x22,[x0,#24]
    str x23,[x0,#32]
    str x24,[x0,#40]
    str x25,[x0,#48]
    str x26,[x0,#56]
    str x27,[x0,#64]
    str x28,[x0,#72]
    str x29,[x0,#80]
    str d8,[x0,#88]
    str d9,[x0,#96]
    str d10,[x0,#104]
    str d11,[x0,#112]
    str d12,[x0,#120]
    str d13,[x0,#128]
    str d14,[x0,#136]
    str d15,[x0,#144]
    mov x1,sp
    str x1,[x0,#152]
    mov x0,x1
    ret
.size luxrt_capture_regs,.-luxrt_capture_regs
.globl luxrt_context_words
.type luxrt_context_words,%function
luxrt_context_words:
    mov x0,#20
    ret
.size luxrt_context_words,.-luxrt_context_words
.globl luxrt_startup_stack_top
.type luxrt_startup_stack_top,%function
luxrt_startup_stack_top:
    ret
.size luxrt_startup_stack_top,.-luxrt_startup_stack_top
.section .note.GNU-stack,"",%progbits

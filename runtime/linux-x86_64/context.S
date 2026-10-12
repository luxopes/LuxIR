.text
.globl luxrt_capture_regs
.type luxrt_capture_regs,@function
luxrt_capture_regs:
    endbr64
    mov %rbx,0(%rdi)
    mov %rbp,8(%rdi)
    mov %r12,16(%rdi)
    mov %r13,24(%rdi)
    mov %r14,32(%rdi)
    mov %r15,40(%rdi)
    lea 8(%rsp),%rax
    mov %rax,48(%rdi)
    ret
.size luxrt_capture_regs,.-luxrt_capture_regs
.globl luxrt_context_words
.type luxrt_context_words,@function
luxrt_context_words:
    mov $7,%eax
    ret
.size luxrt_context_words,.-luxrt_context_words
.globl luxrt_startup_stack_top
.type luxrt_startup_stack_top,@function
luxrt_startup_stack_top:
    mov %rdi,%rax
    ret
.size luxrt_startup_stack_top,.-luxrt_startup_stack_top
.section .note.GNU-stack,"",@progbits

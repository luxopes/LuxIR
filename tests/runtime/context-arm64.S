.text
.globl context_fixture
.type context_fixture,%function
context_fixture:
 stp x19,x20,[sp,#-32]!
 stp x21,x30,[sp,#16]
 mov x20,x0
 mov x21,x1
 mov x0,#4096
 bl luxrt_os_map
 mov x19,x0
 mov x1,#42
 str x1,[x19]
 mov x1,#0x5a5a
 movk x1,#0x5a5a,lsl #16
 movk x1,#0x5a5a,lsl #32
 movk x1,#0x5a5a,lsl #48
 eor x1,x19,x1
 str x1,[x21]
 mov x0,x20
 mov x1,xzr
 bl luxrt_capture_regs
 ldp x21,x30,[sp,#16]
 ldp x19,x20,[sp],#32
 ret
.size context_fixture,.-context_fixture
.section .note.GNU-stack,"",%progbits

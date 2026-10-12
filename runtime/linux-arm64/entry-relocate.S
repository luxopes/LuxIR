.text
.globl _start
.type _start,%function
_start:
    mov x29,xzr
    mov x19,sp
    mov x0,x19
    adrp x1,__ehdr_start
    add x1,x1,:lo12:__ehdr_start
    adrp x2,_DYNAMIC
    add x2,x2,:lo12:_DYNAMIC
    mov x3,#1027
    bl luxrt_startup_relocate
    cbnz x0,.Lfailed
    mov x0,x19
    bl luxrt_entry_main
    b .Lexit
.Lfailed:
    mov x0,#127
.Lexit:
    mov x8,#94
    svc #0
    brk #0
.size _start,.-_start
.section .note.GNU-stack,"",%progbits

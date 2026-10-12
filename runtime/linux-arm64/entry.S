.text
.globl _start
.type _start,%function
_start:
    mov x29,xzr
    mov x30,xzr
    mov x0,sp
    bl luxrt_entry_main
    mov x8,#94
    svc #0
    brk #0
.size _start,.-_start
.section .note.GNU-stack,"",%progbits

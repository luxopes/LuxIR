.text
.globl _start
.type _start,@function
_start:
    mv s0,zero
    mv ra,zero
    mv a0,sp
    call luxrt_entry_main
    li a7,94
    ecall
    unimp
.size _start,.-_start
.section .note.GNU-stack,"",@progbits

.text
.globl _start
.type _start,@function
_start:
    li s0,0
    li ra,0
    .option push
    .option norelax
    lla gp,__global_pointer$
    .option pop
    mv s1,sp
    mv a0,s1
    .option push
    .option norelax
    lla a1,__ehdr_start
    lla a2,_DYNAMIC
    .option pop
    li a3,3
    call luxrt_startup_relocate
    bnez a0,.Lfailed
    mv a0,s1
    call luxrt_entry_main
    j .Lexit
.Lfailed:
    li a0,127
.Lexit:
    li a7,94
    ecall
    unimp
.size _start,.-_start
.section .note.GNU-stack,"",@progbits

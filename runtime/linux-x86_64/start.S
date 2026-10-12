.text
.globl _start
.type _start,@function
_start:
    endbr64
    xor %ebp,%ebp
    and $-16,%rsp
    call luxrt_demo_main
    mov %eax,%edi
    mov $60,%eax
    syscall
    ud2
.size _start,.-_start
.section .note.GNU-stack,"",@progbits

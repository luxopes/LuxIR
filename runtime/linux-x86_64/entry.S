.text
.globl _start
.type _start,@function
_start:
    endbr64
    xor %ebp,%ebp
    mov %rsp,%rdi
    and $-16,%rsp
    call luxrt_entry_main
    mov %eax,%edi
    mov $231,%eax
    syscall
    ud2
.size _start,.-_start
.section .note.GNU-stack,"",@progbits

.text
.globl _start
.type _start,@function
_start:
    endbr64
    xor %ebp,%ebp
    mov %rsp,%rbx
    and $-16,%rsp
    mov %rbx,%rdi
    lea __ehdr_start(%rip),%rsi
    lea _DYNAMIC(%rip),%rdx
    mov $8,%ecx
    call luxrt_startup_relocate
    test %rax,%rax
    jnz .Lfailed
    mov %rbx,%rdi
    call luxrt_entry_main
    mov %eax,%edi
    jmp .Lexit
.Lfailed:
    mov $127,%edi
.Lexit:
    mov $231,%eax
    syscall
    ud2
.size _start,.-_start
.section .note.GNU-stack,"",@progbits

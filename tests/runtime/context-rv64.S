.text
.globl context_fixture
.type context_fixture,@function
context_fixture:
 addi sp,sp,-32
 sd ra,0(sp)
 sd s1,8(sp)
 sd s2,16(sp)
 sd s3,24(sp)
 mv s2,a0
 mv s3,a1
 li a0,4096
 call luxrt_os_map
 mv s1,a0
 li a1,42
 sd a1,0(s1)
 li a1,0x5a5a5a5a5a5a5a5a
 xor a1,s1,a1
 sd a1,0(s3)
 mv a0,s2
 li a1,0
 call luxrt_capture_regs
 ld ra,0(sp)
 ld s1,8(sp)
 ld s2,16(sp)
 ld s3,24(sp)
 addi sp,sp,32
 ret
.size context_fixture,.-context_fixture
.section .note.GNU-stack,"",@progbits

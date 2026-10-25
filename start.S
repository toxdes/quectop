#if defined(__x86_64__)
.text
.global _start
_start:
    xor %ebp, %ebp
    mov %rsp, %rdi
    and $-16, %rsp
    call entry
    ud2
#elif defined(__aarch64__)
.text
.global _start
_start:
    mov x0, sp
    bl entry
    brk 0
#elif defined(__arm__)
.syntax unified
.arch armv6
.arm
.text
.global _start
_start:
    mov r0, sp
    bic sp, sp, #7
    bl entry
    udf #0

/* EABI division returns the remainder in extra registers, unlike C. */
.section .text.__aeabi_uidivmod,"ax",%progbits
.global __aeabi_uidiv
.global __aeabi_uidivmod
__aeabi_uidiv:
__aeabi_uidivmod:
    push {r4, lr}
    sub sp, sp, #8
    mov r2, sp
    bl quectop_udiv32
    ldr r1, [sp]
    add sp, sp, #8
    pop {r4, pc}

.section .text.__aeabi_uldivmod,"ax",%progbits
.global __aeabi_uldivmod
__aeabi_uldivmod:
    push {r4, lr}
    sub sp, sp, #16
    add r4, sp, #8
    str r4, [sp]
    bl quectop_udiv64
    ldr r2, [sp, #8]
    ldr r3, [sp, #12]
    add sp, sp, #16
    pop {r4, pc}
#endif

.section .note.GNU-stack,"",%progbits

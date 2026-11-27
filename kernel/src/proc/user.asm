bits 64
section .text

USER_CODE equ 0x23
USER_DATA equ 0x1B
RFLAGS_IF equ 0x202

; noreturn void enter_user(entry, rsp, argc, argv)
global enter_user
enter_user:
    cli
    mov r8, rdi
    mov r9, rsi
    mov rdi, rdx            ; argc
    mov rsi, rcx            ; argv

    push USER_DATA          ; ss
    push r9                 ; rsp
    push RFLAGS_IF          ; rflags
    push USER_CODE          ; cs
    push r8                 ; rip

    ; don't leak kernel values into user registers
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq

; the built in programs, built from user/
section .rodata
align 16
global prog_hello_start, prog_hello_end
prog_hello_start: incbin "build/user/hello.elf"
prog_hello_end:

align 16
global prog_spin_start, prog_spin_end
prog_spin_start: incbin "build/user/spin.elf"
prog_spin_end:

align 16
global prog_fault_start, prog_fault_end
prog_fault_start: incbin "build/user/fault.elf"
prog_fault_end:

align 16
global prog_guess_start, prog_guess_end
prog_guess_start: incbin "build/user/guess.elf"
prog_guess_end:

bits 64
section .text

USER_CODE equ 0x23
USER_DATA equ 0x1B
RFLAGS_IF equ 0x202

; noreturn void enter_user(entry, rsp, argc, argv, envp)
global enter_user
enter_user:
    cli
    mov r9, rsi
    mov rax, rdi
    mov rdi, rdx            ; argc
    mov rsi, rcx            ; argv
    mov rdx, r8             ; envp

    push USER_DATA          ; ss
    push r9                 ; rsp
    push RFLAGS_IF          ; rflags
    push USER_CODE          ; cs
    push rax                ; rip

    ; don't leak kernel values into user registers
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
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

; noreturn void return_to_user(struct interrupt_frame *f), same layout as isr.asm
global return_to_user
return_to_user:
    cli
    mov rsp, rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

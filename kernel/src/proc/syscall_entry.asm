bits 64

; linux programs use the syscall instruction. it doesn't switch stacks, so
; take the current thread's kernel stack and build the same frame an
; interrupt would, then leave through iretq like every other return to user

USER_CODE equ 0x23
USER_DATA equ 0x1B

section .bss
align 8
user_rsp: resq 1

section .data
global syscall_kstack
syscall_kstack: dq 0

section .text
extern linux_syscall

global syscall_entry
syscall_entry:
    ; interrupts are masked by SFMASK until linux_syscall turns them on
    mov [rel user_rsp], rsp
    mov rsp, [rel syscall_kstack]
    push USER_DATA              ; ss
    push qword [rel user_rsp]   ; rsp
    push r11                    ; rflags
    push USER_CODE              ; cs
    push rcx                    ; rip
    push qword 0                ; error code
    push qword 0x81             ; vector, just a marker
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp
    call linux_syscall
    cli
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

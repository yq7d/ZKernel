bits 64
section .text

extern thread_exit

; void switch_context(uint64_t *old_rsp, uint64_t new_rsp)
; only callee saved registers need saving, the caller already handled the rest
global switch_context
switch_context:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; first thing a new thread runs. r12 = fn, r13 = arg (see thread_create)
global thread_trampoline
thread_trampoline:
    sti
    xor ebp, ebp
    and rsp, -16
    mov rdi, r13
    call r12
    xor edi, edi
    call thread_exit

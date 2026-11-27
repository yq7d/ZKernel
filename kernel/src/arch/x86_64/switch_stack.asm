; noreturn void switch_stack(uint64_t top, void (*fn)(void))

bits 64
section .text

global switch_stack
switch_stack:
    mov rsp, rdi
    xor ebp, ebp            ; backtraces stop here
    call rsi
.hang:
    cli
    hlt
    jmp .hang

bits 64
section .text

extern main
extern exit

; the kernel starts us with rdi = argc, rsi = argv, rsp 16 byte aligned
global _start
_start:
    xor ebp, ebp
    call main
    mov edi, eax
    call exit
.hang:
    jmp .hang

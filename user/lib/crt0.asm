bits 64
section .text

extern main
extern exit
extern environ

; the kernel starts us with rdi = argc, rsi = argv, rdx = envp, rsp 16 byte aligned
global _start
_start:
    xor ebp, ebp
    mov [rel environ], rdx
    call main
    mov edi, eax
    call exit
.hang:
    jmp .hang

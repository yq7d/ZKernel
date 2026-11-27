bits 64
section .text

extern main
extern exit
extern environ

; zos programs carry this note, anything without it is run as a linux program
section .note.zos note alloc noexec nowrite align=4
    dd 4                    ; name size
    dd 4                    ; desc size
    dd 1                    ; type
    db "ZOS", 0
    dd 1                    ; abi version

section .text

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

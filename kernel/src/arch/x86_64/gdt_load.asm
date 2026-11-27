; void gdt_load(const struct gdtr *gdtr, uint16_t code_sel, uint16_t data_sel)

bits 64
section .text

global gdt_load
gdt_load:
    lgdt [rdi]

    mov ds, dx
    mov es, dx
    mov ss, dx
    xor eax, eax
    mov fs, ax
    mov gs, ax

    ; cs can only be reloaded with a far jump/return
    movzx rsi, si
    push rsi
    lea rax, [rel .reloaded]
    push rax
    o64 retf
.reloaded:
    ret

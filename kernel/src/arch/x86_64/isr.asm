bits 64
section .text

extern interrupt_dispatch

isr_common:
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

    ; 22 qwords on the stack, so rsp is still 16-byte aligned here
    mov rdi, rsp
    cld
    call interrupt_dispatch

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

    add rsp, 16             ; vector + error code
    iretq

; stubs for vectors 0-255, dummy error code where the cpu doesn't push one
%assign i 0
%rep 256
isr_stub_%+i:
  %if i == 8 || i == 10 || i == 11 || i == 12 || i == 13 || i == 14 || i == 17 || i == 21 || i == 29 || i == 30
  %else
    push qword 0
  %endif
    push qword i
    jmp isr_common
%assign i i+1
%endrep

section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep

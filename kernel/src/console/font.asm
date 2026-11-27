section .rodata progbits alloc noexec nowrite align=16

global font_psf_start
global font_psf_end

font_psf_start:
    incbin "kernel/fonts/ter-v16n.psf"
font_psf_end:

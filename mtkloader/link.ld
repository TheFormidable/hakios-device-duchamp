ENTRY(_start)

SECTIONS
{
    /* where LK starts the boot image's kernel: platform.h PLAT_KERNEL_PA, and start.S's text_offset, say the same
     * (the code is not position independent). To be read off the phone before the first run (docs/porting-mt6897.md). */
    . = 0x40080000;

    .text : {
        *(.text.boot)
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(16);
    __loader_end = .;
}

/* BSS is cleared at start: it must end before Android's Image, which tools/chainboot.py puts at CHAIN_OFFSET (0x40000) */
ASSERT(__loader_end <= 0x40080000 + 0x40000, "mtkloader: code and BSS reach Android's Image at CHAIN_OFFSET")

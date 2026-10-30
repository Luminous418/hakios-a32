ENTRY(_start)

SECTIONS
{
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
        /* Stacks live with the image, not below it: LK may load us anywhere. */
        . = ALIGN(16);
        __stack_bottom = .;
        . += 0x8000;
        __stack_top = .;
        . = ALIGN(16);
        __fault_stack_bottom = .;
        . += 0x4000;
        __fault_stack_top = .;
        . = ALIGN(16);
        __watch_stack_bottom = .;
        . += 0x4000;
        __watch_stack_top = .;
        . = ALIGN(16);
        __bss_end = .;
    }

    . = ALIGN(16);
    __loader_end = .;
}

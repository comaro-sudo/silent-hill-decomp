ENTRY(entry)
SECTIONS {
    . = 0x81200000;
    .text : { *(.text.entry) *(.text*) *(.rodata*) *(.sdata*) *(.data*) }
}

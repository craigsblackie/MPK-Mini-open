/*
 * Linker script for the field-updatable application.
 *
 * The application slot sits above the program store, outside the region
 * the stock AKAI updater checksums, so an over-the-air update can erase
 * and rewrite it without ever putting the unit's ability to boot at
 * risk. The resident loader validates what is here against the trailer
 * at 0x0800e800 and only then branches to this vector table. See
 * include/otamap.h for the whole layout, and linker/resident.ld for the
 * image that does the branching.
 *
 * SRAM size is 20 KiB, hardware-validated: the firmware runs with MSP
 * initialized at 0x20005000 and live SWD captures show normal stack use
 * immediately below that address.
 */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08008000, LENGTH = 0x6800
  RAM (xrw)   : ORIGIN = 0x20000000, LENGTH = 20K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
/*
 * The resident loader branches straight to the reset vector after
 * loading MSP, with no stack use of its own in between, so unlike the
 * stock updater's handoff this needs no headroom below the true top.
 */
_vector_stack = _estack;

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .data : AT (_etext)
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } >RAM
}

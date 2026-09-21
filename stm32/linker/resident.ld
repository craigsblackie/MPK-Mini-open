/*
 * Linker script for the resident loader/recovery image.
 *
 * Preserve the stock 8 KiB updater at 0x08000000. It validates and
 * jumps through this image's vector table at 0x08002000. The final
 * halfword at 0x080077fe is its additive checksum; the program store
 * begins on the next page at 0x08007800.
 *
 * This is the image the stock updater checksums, so it is installed
 * once over SWD and never rewritten in the field. The application it
 * hands over to lives above the program store and has its own script
 * (app.ld). See include/otamap.h for the whole layout.
 *
 * SRAM size is 20 KiB. In addition to the part-family specification,
 * this is now hardware-validated: the replacement firmware runs with
 * MSP initialized at 0x20005000 and live SWD captures show normal stack
 * use immediately below that address.
 */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08002000, LENGTH = 0x57FE
  CHECKSUM (rx) : ORIGIN = 0x080077FE, LENGTH = 2
  RAM (xrw)   : ORIGIN = 0x20000000, LENGTH = 20K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
/*
 * The retained AKAI updater loads the first vector word into MSP and then
 * pops two words before branching to the reset vector.  Point it eight
 * bytes below the true stack top so that its pop is in bounds and leaves
 * MSP at _estack for this image.
 */
_vector_stack = _estack - 8;

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

  .app_checksum :
  {
    SHORT(0)
  } >CHECKSUM

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

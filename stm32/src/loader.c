#include "loader.h"
#include "app_image.h"
#include "otamap.h"
#include "flash.h"
#include "stm32f102.h"

#define SCB_VTOR (*(volatile uint32_t *)0xe000ed08u)

/* Column 7 of the key/pad matrix carries the panel buttons; bit 1 is
 * TAP TEMPO (transport.c). Columns are one-hot active low on PC7..PC15
 * and rows are active low on PB15..PB8, both as matrix.c documents. */
#define COLUMN_7_MASK 0x4000u
#define ROW_TAP_TEMPO (1u << 1)

/*
 * Wait for the row pull-ups to drag the lines back up through the matrix
 * diodes after selecting a column.
 *
 * This is long -- roughly 400 us once the PLL is running -- which is
 * fine at boot, where it happens once before anything else exists, and
 * is emphatically not fine in a running main loop: USART1 has no
 * receive FIFO, and 400 us is longer than the 320 us a byte takes at
 * 31250 baud, so a poll that blocked here would drop incoming MIDI.
 * Recovery therefore uses loader_panel_select()/loader_panel_rows()
 * below, which never blocks.
 */
static void settle(void)
{
	for (volatile uint32_t i = 0; i < 2000u; i++) {
	}
}

void loader_panel_init(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_IOPBEN | RCC_APB2ENR_IOPCEN;

	/* Same configuration matrix_init() uses, so the application finds
	 * the ports in the state it would have set up anyway. */
	GPIOC->CRL = (GPIOC->CRL & ~(0xfu << 28)) | (0x2u << 28);
	GPIOC->CRH = 0x22222222u;
	GPIOB->CRH = 0x88888888u;
	GPIOB->ODR |= 0xff00u;
	GPIOC->ODR |= 0xff80u;
}

uint8_t loader_read_panel(void)
{
	GPIOC->ODR &= ~COLUMN_7_MASK;
	settle();
	uint8_t rows = (uint8_t)(~(GPIOB->IDR >> 8));
	GPIOC->ODR |= 0xff80u;
	return rows;
}

void loader_panel_select(void)
{
	/* Recovery has no other columns to scan, so column 7 simply stays
	 * selected and the rows are always settled. That turns each poll
	 * into a single register read with no blocking wait -- see
	 * settle() above for why that matters while MIDI is arriving. */
	GPIOC->ODR &= ~COLUMN_7_MASK;
}

uint8_t loader_panel_rows(void)
{
	return (uint8_t)(~(GPIOB->IDR >> 8));
}

bool loader_recovery_forced(void)
{
	loader_panel_init();
	return (loader_read_panel() & ROW_TAP_TEMPO) != 0u;
}

void loader_jump_to_app(void)
{
	uint32_t stack_pointer, entry_point;
	flash_read(APP_SLOT_BASE, &stack_pointer, sizeof stack_pointer);
	flash_read(APP_SLOT_BASE + 4u, &entry_point, sizeof entry_point);

	/* The application's own Reset_Handler sets this too; doing it here
	 * as well means the vector table is already right if an exception
	 * fires between the branch and that assignment. */
	SCB_VTOR = APP_SLOT_BASE;

	__asm volatile (
		"msr msp, %0\n"
		"bx  %1\n"
		:
		: "r"(stack_pointer), "r"(entry_point)
		: "memory");

	while (1) {
	}
}

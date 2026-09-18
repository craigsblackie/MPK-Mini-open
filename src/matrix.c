/*
 * Key/pad matrix scanner for the AD07 mainboard.
 *
 * Reimplementation based on documented, byte-verified behavior from
 * FIRMWARE_ANALYSIS.md (see "Confirmed: key/pad matrix scanner"), not
 * copied from the original decompiled code. The column-select values
 * (PC7..PC15, one-hot) are hardware-wiring facts read from the
 * original firmware's data table, reused here because they describe
 * this specific board's physical PCB trace layout, not any original
 * expression -- the same way a keyboard firmware's matrix pinout is
 * always hardware-specific, factual data.
 *
 * 9 columns driven via GPIOC[15:7] (one-hot, active low), 8 rows read
 * from GPIOB_IDR[15:8]. Debounce: a column's row-byte must read the
 * same value on two consecutive scans before matrix_state[] updates.
 */
#include "matrix.h"
#include "stm32f102.h"

/* PC7..PC15, one bit per column, active low (matches the original
 * firmware's table at flash 0x08006da8 -- confirmed via raw-byte
 * read against the verified dump). */
static const uint16_t column_mask[MATRIX_COLS] = {
	0x0080, 0x0100, 0x0200, 0x0400, 0x0800,
	0x1000, 0x2000, 0x4000, 0x8000,
};

uint8_t matrix_state[MATRIX_COLS];
static uint8_t pending_state[MATRIX_COLS];
static uint8_t stable_count[MATRIX_COLS];

void matrix_init(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_IOPBEN | RCC_APB2ENR_IOPCEN;

	/* GPIOC[15:7]: general-purpose push-pull outputs at 2 MHz. Pin 7
	 * occupies CRL's top nibble; pins 8-15 occupy all of CRH. This is
	 * also the configuration used by the original firmware's GPIO
	 * init (pin mask 0xff80, mode Out_PP, speed 2 MHz). */
	GPIOC->CRL = (GPIOC->CRL & ~(0xFu << 28)) | (0x2u << 28);
	GPIOC->CRH = 0x22222222u;

	/* GPIOB[15:8]: inputs with pull-ups, matching the original's
	 * GPIO_Mode_IPU setup for mask 0xff00. */
	GPIOB->CRH = 0x88888888u;
	GPIOB->ODR |= 0xFF00u;
	GPIOC->ODR |= 0xFF80u; /* all columns idle high (deselected) */

	for (int i = 0; i < MATRIX_COLS; i++) {
		/* Rows are active-low and pulled high. Start in the electrical
		 * idle state so the first debounce pass cannot synthesize an
		 * all-buttons-pressed transition. */
		matrix_state[i] = 0xFFu;
		pending_state[i] = 0xFFu;
		stable_count[i] = 0;
	}
}

void matrix_scan(void)
{
	for (int col = 0; col < MATRIX_COLS; col++) {
		GPIOC->ODR = (GPIOC->ODR | 0xFF80u) & ~column_mask[col];

		for (volatile int d = 0; d < 40; d++) {
		}

		uint8_t rows = (uint8_t)(GPIOB->IDR >> 8);

		if (rows == pending_state[col]) {
			uint8_t previous_count = stable_count[col];
			if (previous_count < 0xF0) {
				stable_count[col] = (uint8_t)(previous_count + 1);
			}
			/* Stock commits key columns after two matching confirmation
			 * scans, but waits for eight on the noisier panel buttons. */
			if ((col < 7 && previous_count == 1) ||
			    (col >= 7 && previous_count == 7)) {
				matrix_state[col] = rows;
			}
		} else {
			pending_state[col] = rows;
			stable_count[col] = 0;
		}
	}

	GPIOC->ODR |= 0xFF80u; /* deselect all columns when done */
}

/* Non-blocking USART1 bridge between the STM32 controller and ESP32-C3. */
#include "midi_uart.h"
#include "stm32f102.h"
#include "sysex.h"

/* An editor "read all programs" answers with five 110-byte SysEx dumps, which
 * the 31250 baud link needs ~180 ms to clear. Size the ring for that whole
 * burst so a reply cannot be truncated mid-message. */
#define UART_TX_SIZE 1024u
#define USART1_BRR_31250_AT_24MHZ 0x0300u

static uint8_t tx_buf[UART_TX_SIZE];
static uint16_t tx_head;
static uint16_t tx_tail;

static void tx_push(uint8_t byte)
{
	uint16_t next = (uint16_t)((tx_head + 1u) & (UART_TX_SIZE - 1u));
	if (next == tx_tail) return;
	tx_buf[tx_head] = byte;
	tx_head = next;
}

void midi_uart_init(void)
{
	tx_head = 0;
	tx_tail = 0;
	RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_USART1EN;

	/* PA9: alternate-function push-pull, 2 MHz. PA10: input pull-up. */
	GPIOA->CRH = (GPIOA->CRH & ~((0xFu << 4) | (0xFu << 8))) |
	             (0xAu << 4) | (0x8u << 8);
	GPIOA->ODR |= (1u << 10);

	USART1->BRR = USART1_BRR_31250_AT_24MHZ;
	USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

void midi_uart_mirror_usb(const uint8_t *events, size_t len)
{
	static const uint8_t cin_bytes[16] = {
		0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1
	};
	for (size_t offset = 0; offset + 4 <= len; offset += 4) {
		uint8_t count = cin_bytes[events[offset] & 0x0fu];
		for (uint8_t i = 0; i < count; i++) tx_push(events[offset + 1u + i]);
	}
}

void midi_uart_editor_toggle(void)
{
	/* 0x7D is the MIDI educational/non-commercial manufacturer ID.  The
	 * ESP consumes this private command instead of forwarding it to BLE. */
	static const uint8_t command[] = {
		0xf0, 0x7d, 'M', 'P', 'K', 0x01, 0xf7
	};
	for (size_t i = 0; i < sizeof(command); i++) tx_push(command[i]);
}

void midi_uart_process(void)
{
	/* Bound RX work so a noisy or disconnected peer cannot starve scanning. */
	for (uint8_t count = 0; count < 16 && (USART1->SR & USART_SR_RXNE); count++) {
		uint32_t status = USART1->SR;
		uint8_t byte = (uint8_t)USART1->DR;
		if ((status & (USART_SR_ORE | USART_SR_NE | USART_SR_FE)) == 0)
			midi_input_byte(byte);
	}
	if (tx_tail != tx_head && (USART1->SR & USART_SR_TXE)) {
		USART1->DR = tx_buf[tx_tail];
		tx_tail = (uint16_t)((tx_tail + 1u) & (UART_TX_SIZE - 1u));
	}
}

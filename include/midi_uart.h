#ifndef MIDI_UART_H
#define MIDI_UART_H

#include <stddef.h>
#include <stdint.h>

/* Bidirectional raw-MIDI bridge for the external ESP32-C3.
 * STM32 PA9/USART1_TX -> ESP GPIO4/UART1_RX
 * STM32 PA10/USART1_RX <- ESP GPIO5/UART1_TX */
void midi_uart_init(void);
void midi_uart_process(void);

/* Mirror one or more four-byte USB-MIDI event packets as raw MIDI bytes. */
void midi_uart_mirror_usb(const uint8_t *events, size_t len);

#endif

/*
 * Reset handler and vector table for STM32F102R8T6.
 *
 * Standard Cortex-M3 startup sequence: copy .data from flash to RAM,
 * zero .bss, call main(). Written fresh against the generic
 * Cortex-M3/STM32F1 startup pattern (the same shape every STM32F1
 * project's startup file has) -- not copied from the original firmware.
 */
#include <stdint.h>

extern uint32_t _handoff_stack;
extern uint32_t _etext, _sdata, _edata, _sbss, _ebss;

#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08u)
#define APP_VECTOR_BASE 0x08002000u

void Reset_Handler(void);
static void Default_Handler(void);
extern int main(void);

void NMI_Handler(void)          __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void)    __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)    __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)          __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)       __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void)      __attribute__((weak, alias("Default_Handler")));
void USB_LP_CAN1_RX0_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

__attribute__((section(".isr_vector")))
const void *const vector_table[] = {
	&_handoff_stack,
	Reset_Handler,
	NMI_Handler,
	HardFault_Handler,
	MemManage_Handler,
	BusFault_Handler,
	UsageFault_Handler,
	0, 0, 0, 0,
	SVC_Handler,
	DebugMon_Handler,
	0,
	PendSV_Handler,
	SysTick_Handler,
	/* USB_LP_CAN1_RX0 is IRQ20 on STM32F102. USB itself is polled, so
	 * this slot remains the default handler unless that changes. */
	[16 + 20] = USB_LP_CAN1_RX0_IRQHandler,
};

static void Default_Handler(void)
{
	while (1) {
	}
}

void Reset_Handler(void)
{
	uint32_t *src, *dst;

	/* The stock updater remains at address zero, so relocate exceptions
	 * before the application enables SysTick or USB interrupts. */
	SCB_VTOR = APP_VECTOR_BASE;

	src = &_etext;
	dst = &_sdata;
	while (dst < &_edata) {
		*dst++ = *src++;
	}

	dst = &_sbss;
	while (dst < &_ebss) {
		*dst++ = 0;
	}

	main();

	while (1) {
	}
}

/*
 * Reset handler and vector table for STM32F102R8T6.
 *
 * Standard Cortex-M3 startup sequence: copy .data from flash to RAM,
 * zero .bss, call main(). Written fresh against the generic
 * Cortex-M3/STM32F1 startup pattern (the same shape every STM32F1
 * project's startup file has) -- not copied from the original firmware.
 *
 * Shared by both images. They differ only in where their vector table
 * lives -- the resident loader/recovery image at 0x08002000 where the
 * stock updater jumps, the application in its own slot -- so the base
 * comes from the build (VECTOR_BASE) and the initial stack value comes
 * from the linker script, which knows whether its caller pops words off
 * that stack before branching.
 */
#include <stdint.h>

#ifndef VECTOR_BASE
#error "VECTOR_BASE must be defined by the build"
#endif

extern uint32_t _vector_stack;
extern uint32_t _etext, _sdata, _edata, _sbss, _ebss;

#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08u)

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
	&_vector_stack,
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

	/* Something else owns the vector table at address zero -- the stock
	 * updater for the resident image, the resident loader for the
	 * application -- so relocate exceptions before enabling SysTick or
	 * USB interrupts. */
	SCB_VTOR = VECTOR_BASE;

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

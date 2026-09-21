/* Software reset, used after an accepted firmware image so the stock
 * updater runs again and hands control to whatever the loader now
 * finds in the application slot. */
#include <stdint.h>

#define SCB_AIRCR (*(volatile uint32_t *)0xe000ed0cu)
#define AIRCR_VECTKEY 0x05fa0000u
#define AIRCR_SYSRESETREQ (1u << 2)

void ota_reset_system(void)
{
	__asm volatile ("dsb" ::: "memory");
	SCB_AIRCR = AIRCR_VECTKEY | AIRCR_SYSRESETREQ;
	__asm volatile ("dsb" ::: "memory");
	while (1) {
	}
}

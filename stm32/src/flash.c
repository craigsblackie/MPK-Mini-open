#include "flash.h"
#include "crc32.h"
#include "stm32f102.h"

#define FLASH_KEY1 0x45670123u
#define FLASH_KEY2 0xcdef89abu
#define FLASH_ERRORS (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)

static void wait_idle(void)
{
	while (FLASH_IF->SR & FLASH_SR_BSY) {
	}
}

void flash_unlock(void)
{
	if (FLASH_IF->CR & FLASH_CR_LOCK) {
		FLASH_IF->KEYR = FLASH_KEY1;
		FLASH_IF->KEYR = FLASH_KEY2;
	}
}

void flash_lock(void)
{
	FLASH_IF->CR |= FLASH_CR_LOCK;
}

bool flash_erase_page(uint32_t address)
{
	/* A page erase takes tens of milliseconds, during which an
	 * interrupt handler that touched flash would corrupt the
	 * operation. Nothing in this firmware does, but masking matches
	 * what program.c already does for the program store and costs
	 * only the SysTick counts lost while the bus is stalled anyway. */
	uint32_t primask;
	__asm volatile ("mrs %0, primask\n cpsid i" : "=r"(primask) :: "memory");

	flash_unlock();
	wait_idle();
	FLASH_IF->SR = FLASH_SR_EOP | FLASH_ERRORS;
	FLASH_IF->CR |= FLASH_CR_PER;
	FLASH_IF->AR = address;
	FLASH_IF->CR |= FLASH_CR_STRT;
	wait_idle();
	FLASH_IF->CR &= ~FLASH_CR_PER;
	uint32_t status = FLASH_IF->SR;
	FLASH_IF->SR = FLASH_SR_EOP | FLASH_ERRORS;
	flash_lock();

	if ((primask & 1u) == 0u) __asm volatile ("cpsie i" ::: "memory");
	return (status & FLASH_ERRORS) == 0u;
}

bool flash_program(uint32_t address, const uint8_t *data, size_t len)
{
	if ((address & 1u) != 0u || (len & 1u) != 0u) return false;

	uint32_t primask;
	__asm volatile ("mrs %0, primask\n cpsid i" : "=r"(primask) :: "memory");

	flash_unlock();
	wait_idle();
	FLASH_IF->SR = FLASH_SR_EOP | FLASH_ERRORS;
	FLASH_IF->CR |= FLASH_CR_PG;

	bool ok = true;
	volatile uint16_t *dst = (volatile uint16_t *)address;
	for (size_t i = 0; i < len / 2u; i++) {
		uint16_t value = (uint16_t)data[i * 2u] |
		                 (uint16_t)((uint16_t)data[i * 2u + 1u] << 8);
		dst[i] = value;
		wait_idle();
		if (FLASH_IF->SR & FLASH_ERRORS) { ok = false; break; }
		if (dst[i] != value) { ok = false; break; } /* readback */
	}

	FLASH_IF->CR &= ~FLASH_CR_PG;
	FLASH_IF->SR = FLASH_SR_EOP | FLASH_ERRORS;
	flash_lock();

	if ((primask & 1u) == 0u) __asm volatile ("cpsie i" ::: "memory");
	return ok;
}

bool flash_is_erased(uint32_t address, size_t len)
{
	const volatile uint8_t *bytes = (const volatile uint8_t *)address;
	for (size_t i = 0; i < len; i++) {
		if (bytes[i] != 0xffu) return false;
	}
	return true;
}

void flash_read(uint32_t address, void *dst, size_t len)
{
	const volatile uint8_t *src = (const volatile uint8_t *)address;
	uint8_t *out = (uint8_t *)dst;
	for (size_t i = 0; i < len; i++) out[i] = src[i];
}

uint32_t flash_crc32(uint32_t address, size_t len)
{
	/* Flash is directly addressable, so this walks it in place rather
	 * than staging it through RAM the firmware does not have to spare. */
	return crc32_compute((const void *)address, len);
}

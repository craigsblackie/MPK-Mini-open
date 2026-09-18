/*
 * Minimal STM32F102R8T6 register definitions.
 *
 * Hand-written from the public ST reference manual (RM0008) register
 * map, covering only the peripherals this project currently uses.
 * Not derived from, or copied out of, the original AKAI firmware --
 * these are standard STM32F1 peripheral addresses/bit layouts, the
 * same for any STM32F1 project.
 */
#ifndef STM32F102_H
#define STM32F102_H

#include <stdint.h>

#define __IO volatile

/* ---- Core peripherals ---- */
typedef struct {
	__IO uint32_t CTRL;
	__IO uint32_t LOAD;
	__IO uint32_t VAL;
	__IO uint32_t CALIB;
} SysTick_TypeDef;
#define SysTick ((SysTick_TypeDef *)0xE000E010u)

#define SysTick_CTRL_ENABLE (1u << 0)
#define SysTick_CTRL_TICKINT (1u << 1)
#define SysTick_CTRL_CLKSOURCE (1u << 2) /* 1 = processor clock (AHB) */
#define SysTick_CTRL_COUNTFLAG (1u << 16)

/* ---- RCC ---- */
typedef struct {
	__IO uint32_t CR;
	__IO uint32_t CFGR;
	__IO uint32_t CIR;
	__IO uint32_t APB2RSTR;
	__IO uint32_t APB1RSTR;
	__IO uint32_t AHBENR;
	__IO uint32_t APB2ENR;
	__IO uint32_t APB1ENR;
} RCC_TypeDef;
#define RCC ((RCC_TypeDef *)0x40021000u)

#define RCC_APB2ENR_AFIOEN  (1u << 0)
#define RCC_APB2ENR_IOPAEN  (1u << 2)
#define RCC_APB2ENR_IOPBEN  (1u << 3)
#define RCC_APB2ENR_IOPCEN  (1u << 4)
#define RCC_APB2ENR_USART1EN (1u << 14)
#define RCC_APB1ENR_USBEN   (1u << 23)

#define RCC_CR_HSEON    (1u << 16)
#define RCC_CR_HSERDY   (1u << 17)
#define RCC_CR_PLLON    (1u << 24)
#define RCC_CR_PLLRDY   (1u << 25)

#define RCC_CFGR_SW_PLL       0x2u
#define RCC_CFGR_SWS_MASK     0xCu
#define RCC_CFGR_SWS_PLL      0x8u
#define RCC_CFGR_PLLSRC_HSE   (1u << 16)
#define RCC_CFGR_PLLXTPRE_DIV1 (0u << 17)
#define RCC_CFGR_PLLMUL6      (0x4u << 18)
/* PLLMUL field encodes (multiplier - 2); 0x4 = x6, giving
 * 8 MHz HSE * 6 = 48 MHz -- exactly USB full-speed's required clock,
 * with no USB prescaler needed (matches this board's 8 MHz crystal,
 * confirmed from the Y1 marking in the board photos). */

/* ---- FLASH interface (for wait-state config at higher clocks) ---- */
typedef struct {
	__IO uint32_t ACR;
	__IO uint32_t KEYR;
	__IO uint32_t OPTKEYR;
	__IO uint32_t SR;
	__IO uint32_t CR;
	__IO uint32_t AR;
} FLASH_TypeDef;
#define FLASH_IF ((FLASH_TypeDef *)0x40022000u)
#define FLASH_ACR_LATENCY_2 0x2u /* 2 wait states, required for 48-72 MHz per RM0008 */
#define FLASH_SR_BSY       (1u << 0)
#define FLASH_SR_PGERR     (1u << 2)
#define FLASH_SR_WRPRTERR  (1u << 4)
#define FLASH_SR_EOP       (1u << 5)
#define FLASH_CR_PG        (1u << 0)
#define FLASH_CR_PER       (1u << 1)
#define FLASH_CR_STRT      (1u << 6)
#define FLASH_CR_LOCK      (1u << 7)

/* ---- GPIO ---- */
typedef struct {
	__IO uint32_t CRL;
	__IO uint32_t CRH;
	__IO uint32_t IDR;
	__IO uint32_t ODR;
	__IO uint32_t BSRR;
	__IO uint32_t BRR;
	__IO uint32_t LCKR;
} GPIO_TypeDef;
#define GPIOA ((GPIO_TypeDef *)0x40010800u)
#define GPIOB ((GPIO_TypeDef *)0x40010C00u)
#define GPIOC ((GPIO_TypeDef *)0x40011000u)

/* ---- Alternate-function I/O ---- */
typedef struct {
	__IO uint32_t EVCR;
	__IO uint32_t MAPR;
} AFIO_TypeDef;
#define AFIO ((AFIO_TypeDef *)0x40010000u)

/* PB3/PB4 power up as JTAG pins.  The LED shift registers use those
 * pins, while SWD itself only needs PA13/PA14, so select SWD-only. */
#define AFIO_MAPR_SWJ_CFG_MASK     (7u << 24)
#define AFIO_MAPR_SWJ_CFG_SWD_ONLY (2u << 24)

/* ---- USART ---- */
typedef struct {
	__IO uint32_t SR;
	__IO uint32_t DR;
	__IO uint32_t BRR;
	__IO uint32_t CR1;
	__IO uint32_t CR2;
	__IO uint32_t CR3;
	__IO uint32_t GTPR;
} USART_TypeDef;
#define USART1 ((USART_TypeDef *)0x40013800u)

#define USART_SR_FE   (1u << 1)
#define USART_SR_NE   (1u << 2)
#define USART_SR_ORE  (1u << 3)
#define USART_SR_RXNE (1u << 5)
#define USART_SR_TXE  (1u << 7)
#define USART_CR1_RE  (1u << 2)
#define USART_CR1_TE  (1u << 3)
#define USART_CR1_UE  (1u << 13)

/* ---- USB device peripheral ---- */
typedef struct {
	__IO uint16_t VALUE;
	uint16_t RESERVED;
} USB_Reg16_TypeDef;

typedef struct {
	USB_Reg16_TypeDef EPR[8];
	uint32_t RESERVED[8];
	USB_Reg16_TypeDef CNTR;
	USB_Reg16_TypeDef ISTR;
	USB_Reg16_TypeDef FNR;
	USB_Reg16_TypeDef DADDR;
	USB_Reg16_TypeDef BTABLE;
} USB_TypeDef;
#define USB ((USB_TypeDef *)0x40005C00u)
#define USB_PMA_BASE 0x40006000u

/* STM32F1 USB registers are 16-bit values spaced on 32-bit addresses.
 * Accessing EPnR as a 32-bit C object breaks its write-zero/toggle-bit
 * semantics on real hardware. */
#define USB_EPR(ep)   (USB->EPR[(ep)].VALUE)
#define USB_CNTR_REG  (USB->CNTR.VALUE)
#define USB_ISTR_REG  (USB->ISTR.VALUE)
#define USB_FNR_REG   (USB->FNR.VALUE)
#define USB_DADDR_REG (USB->DADDR.VALUE)
#define USB_BTABLE_REG (USB->BTABLE.VALUE)

#define USB_CNTR_FRES   (1u << 0)
#define USB_CNTR_PDWN   (1u << 1)
#define USB_CNTR_LP_MODE (1u << 2)
#define USB_CNTR_FSUSP  (1u << 3)
#define USB_CNTR_RESETM (1u << 10)
#define USB_CNTR_CTRM   (1u << 15)

#define USB_ISTR_EP_ID  0x000Fu
#define USB_ISTR_DIR    (1u << 4)
#define USB_ISTR_RESET  (1u << 10)
#define USB_ISTR_CTR    (1u << 15)

/* EPnR STAT_TX/STAT_RX toggle values (write-1-to-toggle bits, per RM0008) */
#define USB_EP_STAT_DISABLED 0u
#define USB_EP_STAT_STALL    1u
#define USB_EP_STAT_NAK      2u
#define USB_EP_STAT_VALID    3u

/* ---- ADC ---- */
typedef struct {
	__IO uint32_t SR;
	__IO uint32_t CR1;
	__IO uint32_t CR2;
	__IO uint32_t SMPR1;
	__IO uint32_t SMPR2;
	__IO uint32_t JOFR[4];
	__IO uint32_t HTR;
	__IO uint32_t LTR;
	__IO uint32_t SQR1;
	__IO uint32_t SQR2;
	__IO uint32_t SQR3;
	__IO uint32_t JSQR;
	__IO uint32_t JDR[4];
	__IO uint32_t DR;
} ADC_TypeDef;
#define ADC1 ((ADC_TypeDef *)0x40012400u)

#define ADC_CR2_ADON     (1u << 0)
#define ADC_CR2_CONT     (1u << 1)
#define ADC_CR2_CAL      (1u << 2)
#define ADC_CR2_RSTCAL   (1u << 3)
#define ADC_CR2_DMA      (1u << 8)
#define ADC_CR2_EXTTRIG  (1u << 20)
#define ADC_CR2_EXTSEL_SWSTART (0x7u << 17) /* SWSTART as trigger source */
#define ADC_CR2_SWSTART  (1u << 22)
#define RCC_APB2ENR_ADC1EN (1u << 9)

/* ---- DMA ---- */
typedef struct {
	__IO uint32_t CCR;
	__IO uint32_t CNDTR;
	__IO uint32_t CPAR;
	__IO uint32_t CMAR;
	uint32_t RESERVED;
} DMA_Channel_TypeDef;
typedef struct {
	__IO uint32_t ISR;
	__IO uint32_t IFCR;
	DMA_Channel_TypeDef CH[7]; /* channels 1..7 -> CH[0..6] */
} DMA_TypeDef;
#define DMA1 ((DMA_TypeDef *)0x40020000u)

#define DMA_CCR_EN     (1u << 0)
#define DMA_CCR_TCIE   (1u << 1)
#define DMA_CCR_CIRC   (1u << 5)
#define DMA_CCR_MINC   (1u << 7)
#define DMA_CCR_PSIZE_16 (1u << 8)
#define DMA_CCR_MSIZE_16 (1u << 10)
#define RCC_AHBENR_DMA1EN (1u << 0)
#define DMA_ISR_TCIF1    (1u << 1)
#define DMA_IFCR_CGIF1   (1u << 0)
#define DMA_IFCR_CTCIF1  (1u << 1)
#define DMA_IFCR_CHTIF1  (1u << 2)
#define DMA_IFCR_CTEIF1  (1u << 3)

#endif /* STM32F102_H */

/* Stock cadence: four software-triggered 16-channel DMA scans, sum >> 4. */
#include "adc.h"
#include "stm32f102.h"

volatile uint16_t adc_raw[ADC_TOTAL_CHANNELS];
volatile uint32_t adc_generation;
static uint16_t dma_samples[ADC_TOTAL_CHANNELS];
static uint32_t sums[ADC_TOTAL_CHANNELS];
static uint8_t batch_count;

static void start_scan(void)
{
	DMA1->CH[0].CCR &= ~DMA_CCR_EN;
	DMA1->IFCR = DMA_IFCR_CGIF1 | DMA_IFCR_CTCIF1 | DMA_IFCR_CHTIF1 | DMA_IFCR_CTEIF1;
	DMA1->CH[0].CNDTR = ADC_TOTAL_CHANNELS;
	DMA1->CH[0].CCR |= DMA_CCR_EN;
	ADC1->CR2 |= ADC_CR2_SWSTART;
}

void adc_init(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN |
	                RCC_APB2ENR_IOPCEN | RCC_APB2ENR_ADC1EN;
	RCC->AHBENR |= RCC_AHBENR_DMA1EN;
	GPIOA->CRL = 0;
	GPIOB->CRL &= ~0x000000ffu;
	GPIOC->CRL &= ~0x00ffffffu;

	ADC1->SQR3 = (0u << 0) | (1u << 5) | (2u << 10) | (3u << 15) |
	             (4u << 20) | (5u << 25);
	ADC1->SQR2 = (6u << 0) | (7u << 5) | (8u << 10) | (9u << 15) |
	             (10u << 20) | (11u << 25);
	ADC1->SQR1 = (12u << 0) | (13u << 5) | (14u << 10) | (15u << 15) |
	             ((uint32_t)(ADC_TOTAL_CHANNELS - 1) << 20);
	ADC1->SMPR2 = 0x3fffffffu;
	ADC1->SMPR1 = 0x00ffffffu;
	ADC1->CR1 = (1u << 8); /* SCAN */
	ADC1->CR2 = ADC_CR2_ADON;
	for (volatile uint16_t i = 0; i < 1000; i++) {}
	ADC1->CR2 |= ADC_CR2_RSTCAL;
	while (ADC1->CR2 & ADC_CR2_RSTCAL) {}
	ADC1->CR2 |= ADC_CR2_CAL;
	while (ADC1->CR2 & ADC_CR2_CAL) {}

	for (uint8_t i = 0; i < ADC_TOTAL_CHANNELS; i++) {
		adc_raw[i] = 0;
		sums[i] = 0;
	}
	batch_count = 0;
	adc_generation = 0;
	DMA1->CH[0].CPAR = (uint32_t)&ADC1->DR;
	DMA1->CH[0].CMAR = (uint32_t)dma_samples;
	DMA1->CH[0].CCR = DMA_CCR_MINC | DMA_CCR_PSIZE_16 | DMA_CCR_MSIZE_16;
	ADC1->CR2 |= ADC_CR2_DMA | ADC_CR2_EXTTRIG | ADC_CR2_EXTSEL_SWSTART;
	start_scan();
}

void adc_process(void)
{
	if ((DMA1->ISR & DMA_ISR_TCIF1) == 0) return;
	DMA1->CH[0].CCR &= ~DMA_CCR_EN;
	DMA1->IFCR = DMA_IFCR_CGIF1 | DMA_IFCR_CTCIF1 | DMA_IFCR_CHTIF1 | DMA_IFCR_CTEIF1;
	for (uint8_t i = 0; i < ADC_TOTAL_CHANNELS; i++) sums[i] += dma_samples[i] & 0x0fffu;
	if (++batch_count >= 4) {
		batch_count = 0;
		for (uint8_t i = 0; i < ADC_TOTAL_CHANNELS; i++) {
			adc_raw[i] = (uint16_t)(sums[i] >> 4);
			sums[i] = 0;
		}
		adc_generation++;
	}
	start_scan();
}

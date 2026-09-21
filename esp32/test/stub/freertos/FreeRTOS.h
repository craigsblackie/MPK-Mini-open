#pragma once
#include <stdint.h>
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1
typedef void* TaskHandle_t;
extern int delay_calls;
static inline void vTaskDelay(uint32_t t){(void)t; delay_calls++;}

/* Added for the editor tests: semaphores and notifications. */
typedef void* SemaphoreHandle_t;
#define pdPASS 1
#define pdFALSE 0

#pragma once
#include "FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreTake(SemaphoreHandle_t s, uint32_t ticks);
int xSemaphoreGive(SemaphoreHandle_t s);

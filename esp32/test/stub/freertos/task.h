#pragma once
#include "FreeRTOS.h"
/* Tests that need the task body to actually run set this hook; without
 * it, creating a task is a no-op as before. */
extern void (*task_create_hook)(void (*entry)(void *), void *param);
static inline int xTaskCreate(void(*f)(void*),const char*n,int s,void*p,int pr,TaskHandle_t*h){(void)n;(void)s;(void)pr; if(h)*h=(void*)1; if(task_create_hook) task_create_hook(f,p); return 1;}
static inline void vTaskDelete(TaskHandle_t h){(void)h;}
static inline uint32_t ulTaskNotifyTake(int c,uint32_t t){(void)c;(void)t;return 0;}
extern int task_notify_give_count;
static inline void xTaskNotifyGive(TaskHandle_t h){(void)h;task_notify_give_count++;}

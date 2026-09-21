#pragma once
#include <stdio.h>
extern int log_warn_count;
#define ESP_LOGE(t,...) do{}while(0)
#define ESP_LOGW(t,...) do{ log_warn_count++; }while(0)
#define ESP_LOGI(t,...) do{}while(0)
#define ESP_LOGD(t,...) do{}while(0)

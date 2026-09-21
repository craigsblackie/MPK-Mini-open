#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "esp_ota_ops.h"
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n);

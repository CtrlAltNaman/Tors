#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct { size_t size; bool encrypted; } esp_partition_t;
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 255
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *label);
esp_err_t esp_partition_read(const esp_partition_t *p,size_t offset,void *out,size_t n);
esp_err_t esp_partition_write(const esp_partition_t *p,size_t offset,const void *in,size_t n);
esp_err_t esp_partition_erase_range(const esp_partition_t *p,size_t offset,size_t n);

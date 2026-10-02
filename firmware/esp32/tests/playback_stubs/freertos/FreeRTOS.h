#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t TickType_t;
typedef unsigned UBaseType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#ifndef portTICK_PERIOD_MS
#define portTICK_PERIOD_MS 1
#endif
#define pdMS_TO_TICKS(x) ((x) / portTICK_PERIOD_MS)
